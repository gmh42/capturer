#include "capturer/image_processing/vulkan_tile_processor.hpp"

#include <fcntl.h>
#include <linux/dma-buf.h>
#include <sys/ioctl.h>
#include <unistd.h>

#include <cstring>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include "capturer/image_processing/tile_difference.hpp"

#ifdef CAPTURER_HAS_TILE_SHADER
namespace capturer::gpu::shaders {
extern const unsigned char kTileDifferenceSpirv[];
extern const unsigned long kTileDifferenceSpirv_size;
}
#endif

namespace capturer::image_processing {

namespace {

struct PushConstants {
    std::uint32_t width;
    std::uint32_t height;
    std::uint32_t tile_size;
    std::uint32_t tiles_x;
    std::uint32_t has_previous;
    std::uint32_t compare_alpha;
};

void check(VkResult result, const char* what) {
    if (result != VK_SUCCESS) {
        throw std::runtime_error(std::string("VulkanTileDifferenceProcessor: ") + what + " failed (VkResult " + std::to_string(result) + ")");
    }
}

// Owns one submission's Vulkan objects. Waits for the GPU before releasing anything.
class GpuTileJob final : public IProcessResult {
public:
    explicit GpuTileJob(std::shared_ptr<gpu::VulkanRuntime> rt) : runtime(std::move(rt)) {}

    ~GpuTileJob() override {
        const VkDevice device = runtime->device();
        if (submitted) {
            vkWaitForFences(device, 1, &fence, VK_TRUE, UINT64_MAX);
        }
        if (mapped) vkUnmapMemory(device, memory);
        if (buffer != VK_NULL_HANDLE) vkDestroyBuffer(device, buffer, nullptr);
        if (memory != VK_NULL_HANDLE) vkFreeMemory(device, memory, nullptr);
        if (fence != VK_NULL_HANDLE) vkDestroyFence(device, fence, nullptr);
        if (semaphore != VK_NULL_HANDLE) vkDestroySemaphore(device, semaphore, nullptr);
        if (desc_pool != VK_NULL_HANDLE) vkDestroyDescriptorPool(device, desc_pool, nullptr);
        if (cmd_pool != VK_NULL_HANDLE) vkDestroyCommandPool(device, cmd_pool, nullptr);
    }

    bool skipped() const override { return false; }
    ResultLocation location() const override { return ResultLocation::Gpu; }

    const TileDiff& tile_diff() override {
        if (!read_back) {
            check(vkWaitForFences(runtime->device(), 1, &fence, VK_TRUE, UINT64_MAX), "waiting for the compute fence");
            diff.width = tiles_x;
            diff.height = tiles_y;
            const auto* flags = static_cast<const std::uint32_t*>(mapped);
            const std::size_t count = static_cast<std::size_t>(tiles_x) * tiles_y;
            diff.tiles.reserve(count);
            for (std::size_t i = 0; i < count; ++i) {
                const bool changed = flags[i] != 0;
                diff.tiles.push_back({changed, static_cast<std::uint8_t>(changed ? 1u : 0u)});
            }
            read_back = true;
        }
        return diff;
    }

    std::shared_ptr<gpu::VulkanRuntime> runtime;
    std::shared_ptr<void> keepalive;
    // Frames stay reserved until the GPU is done reading them.
    frame::FramePtr current_frame;
    frame::FramePtr previous_frame;
    std::shared_ptr<gpu::GpuImage> current_image;
    std::shared_ptr<gpu::GpuImage> previous_image;
    VkFence fence = VK_NULL_HANDLE;
    VkSemaphore semaphore = VK_NULL_HANDLE;
    VkCommandPool cmd_pool = VK_NULL_HANDLE;
    VkDescriptorPool desc_pool = VK_NULL_HANDLE;
    VkBuffer buffer = VK_NULL_HANDLE;
    VkDeviceMemory memory = VK_NULL_HANDLE;
    void* mapped = nullptr;
    std::uint32_t tiles_x = 0;
    std::uint32_t tiles_y = 0;
    bool submitted = false;
    bool read_back = false;
    TileDiff diff{};
};

// Returns a sync_file fd for the dma-buf's pending writers, or -1 to rely on implicit synchronization.
int acquire_fence(const frame::GpuHandle& handle) {
    if (handle.acquire_fence_fd >= 0) {
        return fcntl(handle.acquire_fence_fd, F_DUPFD_CLOEXEC, 3);
    }
    dma_buf_export_sync_file request{};
    request.flags = DMA_BUF_SYNC_READ;
    request.fd = -1;
    if (ioctl(handle.fds[0], DMA_BUF_IOCTL_EXPORT_SYNC_FILE, &request) == 0) {
        return request.fd;
    }
    return -1;
}

}  // namespace

struct VulkanTileDifferenceProcessor::Pipeline {
    std::shared_ptr<gpu::VulkanRuntime> runtime;
    VkShaderModule module = VK_NULL_HANDLE;
    VkDescriptorSetLayout set_layout = VK_NULL_HANDLE;
    VkPipelineLayout layout = VK_NULL_HANDLE;
    VkPipeline pipeline = VK_NULL_HANDLE;

    ~Pipeline() {
        const VkDevice device = runtime->device();
        if (pipeline != VK_NULL_HANDLE) vkDestroyPipeline(device, pipeline, nullptr);
        if (layout != VK_NULL_HANDLE) vkDestroyPipelineLayout(device, layout, nullptr);
        if (set_layout != VK_NULL_HANDLE) vkDestroyDescriptorSetLayout(device, set_layout, nullptr);
        if (module != VK_NULL_HANDLE) vkDestroyShaderModule(device, module, nullptr);
    }
};

VulkanTileDifferenceProcessor::VulkanTileDifferenceProcessor(std::shared_ptr<gpu::VulkanRuntime> runtime, std::size_t tile_size)
    : runtime_(std::move(runtime)), tile_size_(tile_size) {
    if (!runtime_ || tile_size_ == 0) {
        throw std::invalid_argument("Vulkan runtime and a positive tile size are required");
    }
}

VulkanTileDifferenceProcessor::~VulkanTileDifferenceProcessor() = default;

bool VulkanTileDifferenceProcessor::supports_format(frame::PixelFormat format) const {
    return gpu::VulkanRuntime::to_vk_format(format) != VK_FORMAT_UNDEFINED;
}

bool VulkanTileDifferenceProcessor::can_process(const frame::IFrameInfo& frame) const {
#ifndef CAPTURER_HAS_TILE_SHADER
    (void)frame;
    return false;
#else
    if (!IImageProcessor::can_process(frame) || !runtime_->initialize() || !runtime_->supports_dma_buf_import()) {
        return false;
    }
    const frame::GpuHandle* handle = frame.gpu_handle();
    if (!handle || handle->fds.size() != 1 || handle->fds[0] < 0 || frame.plane_count() != 1 ||
        !handle->has_drm_modifier || handle->drm_modifier == DRM_FORMAT_MOD_INVALID ||
        handle->drm_fourcc != frame::to_drm_fourcc(frame.format())) {
        return false;
    }
    return runtime_->can_import_dma_buf(gpu::VulkanRuntime::to_vk_format(frame.format()), handle->drm_modifier, VK_IMAGE_USAGE_SAMPLED_BIT);
#endif
}

std::unique_ptr<IProcessResult> VulkanTileDifferenceProcessor::process(const frame::FramePtr& previous, const frame::FramePtr& current_frame) {
#ifndef CAPTURER_HAS_TILE_SHADER
    (void)previous;
    (void)current_frame;
    throw std::invalid_argument("VulkanTileDifferenceProcessor: built without the tile shader");
#else
    if (!current_frame || !can_process(*current_frame)) {
        throw std::invalid_argument("VulkanTileDifferenceProcessor cannot process this frame");
    }
    const frame::IFrameInfo& current = *current_frame;
    const VkDevice device = runtime_->device();
    const frame::GpuHandle& handle = *current.gpu_handle();

    if (!pipeline_) {
        auto pipeline = std::make_shared<Pipeline>();
        pipeline->runtime = runtime_;
        VkShaderModuleCreateInfo module_info{};
        module_info.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
        module_info.codeSize = gpu::shaders::kTileDifferenceSpirv_size;
        module_info.pCode = reinterpret_cast<const std::uint32_t*>(gpu::shaders::kTileDifferenceSpirv);
        check(vkCreateShaderModule(device, &module_info, nullptr, &pipeline->module), "vkCreateShaderModule");

        VkDescriptorSetLayoutBinding bindings[3]{};
        const VkDescriptorType types[3] = {VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER};
        for (std::uint32_t i = 0; i < 3; ++i) {
            bindings[i].binding = i;
            bindings[i].descriptorType = types[i];
            bindings[i].descriptorCount = 1;
            bindings[i].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
        }
        VkDescriptorSetLayoutCreateInfo set_info{};
        set_info.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
        set_info.bindingCount = 3;
        set_info.pBindings = bindings;
        check(vkCreateDescriptorSetLayout(device, &set_info, nullptr, &pipeline->set_layout), "vkCreateDescriptorSetLayout");

        VkPushConstantRange push_range{VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(PushConstants)};
        VkPipelineLayoutCreateInfo layout_info{};
        layout_info.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
        layout_info.setLayoutCount = 1;
        layout_info.pSetLayouts = &pipeline->set_layout;
        layout_info.pushConstantRangeCount = 1;
        layout_info.pPushConstantRanges = &push_range;
        check(vkCreatePipelineLayout(device, &layout_info, nullptr, &pipeline->layout), "vkCreatePipelineLayout");

        VkComputePipelineCreateInfo pipeline_info{};
        pipeline_info.sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO;
        pipeline_info.stage.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
        pipeline_info.stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
        pipeline_info.stage.module = pipeline->module;
        pipeline_info.stage.pName = "main";
        pipeline_info.layout = pipeline->layout;
        check(vkCreateComputePipelines(device, VK_NULL_HANDLE, 1, &pipeline_info, nullptr, &pipeline->pipeline), "vkCreateComputePipelines");
        pipeline_ = std::move(pipeline);
    }

    auto source = runtime_->import_dma_buf_image(current, VK_IMAGE_USAGE_SAMPLED_BIT);
    if (!source) {
        throw std::runtime_error("VulkanTileDifferenceProcessor: " + runtime_->last_error());
    }

    const std::size_t width = current.width();
    const std::size_t height = current.height();
    // Reuse the image imported for the previous call when `previous` is that same frame (the ping-pong case).
    std::shared_ptr<gpu::GpuImage> previous_image;
    bool has_previous = false;
    if (previous && previous->width() == width && previous->height() == height && previous->format() == current.format() &&
        can_process(*previous)) {
        if (last_image_ && last_frame_.lock() == previous) {
            previous_image = last_image_;
        } else {
            previous_image = runtime_->import_dma_buf_image(*previous, VK_IMAGE_USAGE_SAMPLED_BIT);
        }
        has_previous = previous_image != nullptr;
    }
    if (!has_previous) {
        previous_image = source;
    }

    auto job = std::make_unique<GpuTileJob>(runtime_);
    job->keepalive = pipeline_;
    job->current_frame = current_frame;
    job->previous_frame = has_previous ? previous : nullptr;
    job->current_image = source;
    job->previous_image = previous_image;
    job->tiles_x = static_cast<std::uint32_t>((width + tile_size_ - 1) / tile_size_);
    job->tiles_y = static_cast<std::uint32_t>((height + tile_size_ - 1) / tile_size_);
    const VkDeviceSize result_bytes = static_cast<VkDeviceSize>(job->tiles_x) * job->tiles_y * sizeof(std::uint32_t);

    VkBufferCreateInfo buffer_info{};
    buffer_info.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    buffer_info.size = result_bytes;
    buffer_info.usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT;
    buffer_info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    check(vkCreateBuffer(device, &buffer_info, nullptr, &job->buffer), "vkCreateBuffer");
    VkMemoryRequirements requirements{};
    vkGetBufferMemoryRequirements(device, job->buffer, &requirements);
    std::uint32_t memory_type = 0;
    if (!runtime_->find_memory_type(requirements.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT, memory_type)) {
        throw std::runtime_error("VulkanTileDifferenceProcessor: no host-visible coherent memory");
    }
    VkMemoryAllocateInfo alloc_info{};
    alloc_info.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    alloc_info.allocationSize = requirements.size;
    alloc_info.memoryTypeIndex = memory_type;
    check(vkAllocateMemory(device, &alloc_info, nullptr, &job->memory), "vkAllocateMemory");
    check(vkBindBufferMemory(device, job->buffer, job->memory, 0), "vkBindBufferMemory");
    check(vkMapMemory(device, job->memory, 0, VK_WHOLE_SIZE, 0, &job->mapped), "vkMapMemory");
    std::memset(job->mapped, 0, static_cast<std::size_t>(result_bytes));

    VkFenceCreateInfo fence_info{};
    fence_info.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
    check(vkCreateFence(device, &fence_info, nullptr, &job->fence), "vkCreateFence");

    VkDescriptorPoolSize pool_sizes[2] = {{VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, 2}, {VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1}};
    VkDescriptorPoolCreateInfo pool_info{};
    pool_info.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    pool_info.maxSets = 1;
    pool_info.poolSizeCount = 2;
    pool_info.pPoolSizes = pool_sizes;
    check(vkCreateDescriptorPool(device, &pool_info, nullptr, &job->desc_pool), "vkCreateDescriptorPool");
    VkDescriptorSetAllocateInfo set_alloc{};
    set_alloc.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
    set_alloc.descriptorPool = job->desc_pool;
    set_alloc.descriptorSetCount = 1;
    set_alloc.pSetLayouts = &pipeline_->set_layout;
    VkDescriptorSet set = VK_NULL_HANDLE;
    check(vkAllocateDescriptorSets(device, &set_alloc, &set), "vkAllocateDescriptorSets");

    VkDescriptorImageInfo source_info{VK_NULL_HANDLE, source->view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
    VkDescriptorImageInfo previous_info{VK_NULL_HANDLE, previous_image->view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
    VkDescriptorBufferInfo buffer_desc{job->buffer, 0, VK_WHOLE_SIZE};
    VkWriteDescriptorSet writes[3]{};
    for (auto& w : writes) {
        w.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        w.dstSet = set;
        w.descriptorCount = 1;
    }
    writes[0].dstBinding = 0;
    writes[0].descriptorType = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
    writes[0].pImageInfo = &source_info;
    writes[1].dstBinding = 1;
    writes[1].descriptorType = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
    writes[1].pImageInfo = &previous_info;
    writes[2].dstBinding = 2;
    writes[2].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    writes[2].pBufferInfo = &buffer_desc;
    vkUpdateDescriptorSets(device, 3, writes, 0, nullptr);

    VkCommandPoolCreateInfo cmd_pool_info{};
    cmd_pool_info.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
    cmd_pool_info.queueFamilyIndex = runtime_->queue_family_index();
    check(vkCreateCommandPool(device, &cmd_pool_info, nullptr, &job->cmd_pool), "vkCreateCommandPool");
    VkCommandBufferAllocateInfo cmd_alloc{};
    cmd_alloc.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    cmd_alloc.commandPool = job->cmd_pool;
    cmd_alloc.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    cmd_alloc.commandBufferCount = 1;
    VkCommandBuffer cmd = VK_NULL_HANDLE;
    check(vkAllocateCommandBuffers(device, &cmd_alloc, &cmd), "vkAllocateCommandBuffers");

    VkCommandBufferBeginInfo begin{};
    begin.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    check(vkBeginCommandBuffer(cmd, &begin), "vkBeginCommandBuffer");

    const VkImageSubresourceRange color_range{VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    // Acquire each dma-buf from the external owner. Drivers preserve the producer's contents despite UNDEFINED.
    VkImageMemoryBarrier barriers[2]{};
    std::uint32_t barrier_count = 0;
    std::vector<const gpu::GpuImage*> acquired{source.get()};
    if (previous_image != source) {
        acquired.push_back(previous_image.get());
    }
    for (const gpu::GpuImage* image : acquired) {
        VkImageMemoryBarrier& barrier = barriers[barrier_count++];
        barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
        barrier.srcAccessMask = 0;
        barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
        barrier.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        barrier.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        barrier.srcQueueFamilyIndex = runtime_->external_queue_family();
        barrier.dstQueueFamilyIndex = runtime_->queue_family_index();
        barrier.image = image->image;
        barrier.subresourceRange = color_range;
    }
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                         0, 0, nullptr, 0, nullptr, barrier_count, barriers);

    const frame::PixelFormatInfo format_info = frame::describe(current.format());
    PushConstants push{static_cast<std::uint32_t>(width), static_cast<std::uint32_t>(height), static_cast<std::uint32_t>(tile_size_),
                       job->tiles_x, has_previous ? 1u : 0u, format_info.has_alpha ? 1u : 0u};
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, pipeline_->pipeline);
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, pipeline_->layout, 0, 1, &set, 0, nullptr);
    vkCmdPushConstants(cmd, pipeline_->layout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(push), &push);
    vkCmdDispatch(cmd, job->tiles_x, job->tiles_y, 1);

    VkBufferMemoryBarrier to_host{};
    to_host.sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER;
    to_host.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
    to_host.dstAccessMask = VK_ACCESS_HOST_READ_BIT;
    to_host.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    to_host.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    to_host.buffer = job->buffer;
    to_host.size = VK_WHOLE_SIZE;
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_HOST_BIT, 0, 0, nullptr, 1, &to_host, 0, nullptr);
    check(vkEndCommandBuffer(cmd), "vkEndCommandBuffer");

    VkSubmitInfo submit{};
    submit.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    submit.commandBufferCount = 1;
    submit.pCommandBuffers = &cmd;
    const VkPipelineStageFlags wait_stage = VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT;
    if (runtime_->supports_sync_file_import()) {
        const int fence_fd = acquire_fence(handle);
        if (fence_fd >= 0) {
            VkSemaphoreCreateInfo sem_info{};
            sem_info.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;
            if (vkCreateSemaphore(device, &sem_info, nullptr, &job->semaphore) == VK_SUCCESS &&
                runtime_->import_sync_file(job->semaphore, fence_fd) == VK_SUCCESS) {
                submit.waitSemaphoreCount = 1;
                submit.pWaitSemaphores = &job->semaphore;
                submit.pWaitDstStageMask = &wait_stage;
            } else {
                close(fence_fd);
            }
        }
    }
    check(runtime_->submit(submit, job->fence), "vkQueueSubmit");
    job->submitted = true;

    last_frame_ = current_frame;
    last_image_ = source;
    return job;
#endif
}

}  // namespace capturer::image_processing
