#include "capturer/gpu/vulkan_runtime.hpp"

#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <cstdio>
#include <stdexcept>
#include <unistd.h>

namespace capturer::gpu {

namespace {

constexpr std::uint32_t kApiVersion = VK_API_VERSION_1_3;

int open_drm_render_node() {
    for (int minor = 128; minor < 192; ++minor) {
        char path[64]{};
        std::snprintf(path, sizeof(path), "/dev/dri/renderD%d", minor);
        const int fd = open(path, O_RDWR | O_CLOEXEC);
        if (fd >= 0) {
            return fd;
        }
    }
    return -1;
}

}

GpuBuffer::~GpuBuffer() {
    if (runtime) {
        runtime->destroy_buffer(*this);
    }
}

VulkanRuntime::VulkanRuntime(BackendType backend_type) : backend_type_(backend_type) {}

VulkanRuntime::~VulkanRuntime() {
    if (gbm_device_) {
        gbm_device_destroy(gbm_device_);
        gbm_device_ = nullptr;
    }
    if (gbm_device_fd_ >= 0) {
        close(gbm_device_fd_);
        gbm_device_fd_ = -1;
    }
    if (device_ != VK_NULL_HANDLE) {
        vkDestroyDevice(device_, nullptr);
    }
    if (instance_ != VK_NULL_HANDLE) {
        vkDestroyInstance(instance_, nullptr);
    }
}

VulkanRuntimeInfo VulkanRuntime::info() const {
    if (!initialized_) {
        return {false, backend_type_, "Vulkan runtime not initialized"};
    }
    return {supports_gpu(), backend_type_, "Vulkan runtime available"};
}

bool VulkanRuntime::supports_gpu() const {
    return initialized_ && physical_device_ != VK_NULL_HANDLE;
}

std::string VulkanRuntime::describe() const {
    return supports_gpu() ? "vulkan-device" : "software-prototype";
}

bool VulkanRuntime::initialize() {
    if (initialized_) {
        return true;
    }

    VkApplicationInfo app_info{};
    app_info.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
    app_info.pApplicationName = "capturer";
    app_info.applicationVersion = VK_MAKE_VERSION(1, 0, 0);
    app_info.pEngineName = "capturer";
    app_info.engineVersion = VK_MAKE_VERSION(1, 0, 0);
    app_info.apiVersion = kApiVersion;

    VkInstanceCreateInfo instance_info{};
    instance_info.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
    instance_info.pApplicationInfo = &app_info;

    if (vkCreateInstance(&instance_info, nullptr, &instance_) != VK_SUCCESS) {
        return false;
    }

    std::uint32_t physical_device_count = 0;
    if (vkEnumeratePhysicalDevices(instance_, &physical_device_count, nullptr) != VK_SUCCESS || physical_device_count == 0) {
        return false;
    }

    std::vector<VkPhysicalDevice> devices(physical_device_count);
    if (vkEnumeratePhysicalDevices(instance_, &physical_device_count, devices.data()) != VK_SUCCESS) {
        return false;
    }

    physical_device_ = devices[0];

    std::uint32_t queue_family_count = 0;
    vkGetPhysicalDeviceQueueFamilyProperties(physical_device_, &queue_family_count, nullptr);
    std::vector<VkQueueFamilyProperties> queue_families(queue_family_count);
    vkGetPhysicalDeviceQueueFamilyProperties(physical_device_, &queue_family_count, queue_families.data());

    for (std::uint32_t i = 0; i < queue_family_count; ++i) {
        if (queue_families[i].queueFlags & VK_QUEUE_TRANSFER_BIT) {
            queue_family_index_ = i;
            break;
        }
    }

    float queue_priority = 1.0f;
    VkDeviceQueueCreateInfo queue_info{};
    queue_info.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
    queue_info.queueFamilyIndex = queue_family_index_;
    queue_info.queueCount = 1;
    queue_info.pQueuePriorities = &queue_priority;

    VkDeviceCreateInfo device_info{};
    device_info.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
    device_info.queueCreateInfoCount = 1;
    device_info.pQueueCreateInfos = &queue_info;

    if (vkCreateDevice(physical_device_, &device_info, nullptr, &device_) != VK_SUCCESS) {
        return false;
    }

    vkGetDeviceQueue(device_, queue_family_index_, 0, &queue_);

    gbm_device_fd_ = open_drm_render_node();
    if (gbm_device_fd_ >= 0) {
        gbm_device_ = gbm_create_device(gbm_device_fd_);
        if (!gbm_device_) {
            close(gbm_device_fd_);
            gbm_device_fd_ = -1;
        }
    }
    initialized_ = true;
    return true;
}

std::shared_ptr<GpuBuffer> VulkanRuntime::upload_bytes(const std::vector<std::uint8_t>& bytes) {
    if (!initialize()) {
        return nullptr;
    }

    auto buffer = std::make_shared<GpuBuffer>();
    buffer->runtime = shared_from_this();
    buffer->size = bytes.size();

    VkBufferCreateInfo buffer_info{};
    buffer_info.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    buffer_info.size = bytes.size();
    buffer_info.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT;
    buffer_info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;

    if (vkCreateBuffer(device_, &buffer_info, nullptr, &buffer->buffer) != VK_SUCCESS) {
        return nullptr;
    }

    VkMemoryRequirements requirements{};
    vkGetBufferMemoryRequirements(device_, buffer->buffer, &requirements);

    VkMemoryAllocateInfo alloc_info{};
    alloc_info.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    alloc_info.allocationSize = requirements.size;
    alloc_info.memoryTypeIndex = 0;

    VkPhysicalDeviceMemoryProperties memory_properties{};
    vkGetPhysicalDeviceMemoryProperties(physical_device_, &memory_properties);
    for (std::uint32_t i = 0; i < memory_properties.memoryTypeCount; ++i) {
        if ((requirements.memoryTypeBits & (1u << i)) && (memory_properties.memoryTypes[i].propertyFlags & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT)) {
            alloc_info.memoryTypeIndex = i;
            break;
        }
    }

    if (vkAllocateMemory(device_, &alloc_info, nullptr, &buffer->memory) != VK_SUCCESS) {
        vkDestroyBuffer(device_, buffer->buffer, nullptr);
        return nullptr;
    }

    if (vkBindBufferMemory(device_, buffer->buffer, buffer->memory, 0) != VK_SUCCESS) {
        vkFreeMemory(device_, buffer->memory, nullptr);
        vkDestroyBuffer(device_, buffer->buffer, nullptr);
        return nullptr;
    }

    void* mapped = nullptr;
    if (vkMapMemory(device_, buffer->memory, 0, bytes.size(), 0, &mapped) != VK_SUCCESS) {
        vkFreeMemory(device_, buffer->memory, nullptr);
        vkDestroyBuffer(device_, buffer->buffer, nullptr);
        return nullptr;
    }
    std::memcpy(mapped, bytes.data(), bytes.size());
    vkUnmapMemory(device_, buffer->memory);

    buffer->valid = true;
    return buffer;
}

std::shared_ptr<GpuBuffer> VulkanRuntime::create_dma_buf_image(int width, int height) {
    if (!initialize()) {
        return nullptr;
    }

    auto buffer = std::make_shared<GpuBuffer>();
    buffer->runtime = shared_from_this();
    buffer->width = width;
    buffer->height = height;

    if (!gbm_device_) {
        return nullptr;
    }

    const int stride = width * 4;
    const int fd = drmPrimeHandleToFD(0, 0, 0, 0);
    (void)fd;
    const auto gbm_bo = gbm_bo_create(gbm_device_, width, height, GBM_FORMAT_XRGB8888, GBM_BO_USE_RENDERING | GBM_BO_USE_LINEAR);
    if (!gbm_bo) {
        return nullptr;
    }

    const int dma_fd = gbm_bo_get_fd(gbm_bo);
    if (dma_fd < 0) {
        gbm_bo_destroy(gbm_bo);
        return nullptr;
    }

    buffer->dma_buf_fd = dma_fd;
    buffer->is_image = true;
    buffer->size = static_cast<std::size_t>(stride * height);
    buffer->valid = true;

    gbm_bo_destroy(gbm_bo);
    return buffer;
}

std::vector<std::uint8_t> VulkanRuntime::download_bytes(const GpuBuffer& buffer) {
    if (!buffer.valid || buffer.size == 0) {
        return {};
    }
    std::vector<std::uint8_t> out(buffer.size);
    if (buffer.is_image && buffer.memory != VK_NULL_HANDLE) {
        void* mapped = nullptr;
        if (vkMapMemory(device_, buffer.memory, 0, buffer.size, 0, &mapped) == VK_SUCCESS) {
            std::memcpy(out.data(), mapped, buffer.size);
            vkUnmapMemory(device_, buffer.memory);
        }
    } else if (buffer.memory != VK_NULL_HANDLE) {
        void* mapped = nullptr;
        if (vkMapMemory(device_, buffer.memory, 0, buffer.size, 0, &mapped) == VK_SUCCESS) {
            std::memcpy(out.data(), mapped, buffer.size);
            vkUnmapMemory(device_, buffer.memory);
        }
    }
    return out;
}

void VulkanRuntime::destroy_buffer(GpuBuffer& buffer) {
    if (buffer.memory != VK_NULL_HANDLE) {
        vkFreeMemory(device_, buffer.memory, nullptr);
        buffer.memory = VK_NULL_HANDLE;
    }
    if (buffer.buffer != VK_NULL_HANDLE) {
        vkDestroyBuffer(device_, buffer.buffer, nullptr);
        buffer.buffer = VK_NULL_HANDLE;
    }
    if (buffer.image != VK_NULL_HANDLE) {
        vkDestroyImage(device_, buffer.image, nullptr);
        buffer.image = VK_NULL_HANDLE;
    }
    if (buffer.image_memory != VK_NULL_HANDLE) {
        vkFreeMemory(device_, buffer.image_memory, nullptr);
        buffer.image_memory = VK_NULL_HANDLE;
    }
    if (buffer.dma_buf_fd >= 0) {
        close(buffer.dma_buf_fd);
        buffer.dma_buf_fd = -1;
    }
    buffer.valid = false;
}

std::vector<std::uint8_t> run_tile_difference_shader(
    const std::vector<std::uint8_t>& previous,
    const std::vector<std::uint8_t>& current,
    std::size_t width,
    std::size_t height,
    std::size_t tile_size,
    const VulkanRuntime& runtime) {
    (void)runtime;
    std::vector<std::uint8_t> output(width * height, 0);
    for (std::size_t i = 0; i < output.size(); ++i) {
        output[i] = static_cast<std::uint8_t>((previous.size() > i ? previous[i] : 0u) != (current.size() > i ? current[i] : 0u));
    }
    return output;
}

}  // namespace capturer::gpu
