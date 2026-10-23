#include "capturer/gpu/vulkan_runtime.hpp"

#include <algorithm>
#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <cstdio>
#include <stdexcept>
#include <unistd.h>

namespace capturer::gpu {

namespace {

constexpr std::uint32_t kApiVersion = VK_API_VERSION_1_3;

constexpr const char* kDmaBufExtensions[] = {
    VK_KHR_EXTERNAL_MEMORY_FD_EXTENSION_NAME,
    VK_EXT_EXTERNAL_MEMORY_DMA_BUF_EXTENSION_NAME,
    VK_EXT_IMAGE_DRM_FORMAT_MODIFIER_EXTENSION_NAME,
};

std::vector<std::string> device_extensions(VkPhysicalDevice device) {
    std::uint32_t count = 0;
    vkEnumerateDeviceExtensionProperties(device, nullptr, &count, nullptr);
    std::vector<VkExtensionProperties> props(count);
    vkEnumerateDeviceExtensionProperties(device, nullptr, &count, props.data());
    std::vector<std::string> names;
    names.reserve(count);
    for (const auto& p : props) {
        names.emplace_back(p.extensionName);
    }
    return names;
}

// Prefers real GPUs that can import dma-bufs; CPU rasterizers rank last.
VkPhysicalDevice pick_physical_device(const std::vector<VkPhysicalDevice>& devices) {
    VkPhysicalDevice best = devices.front();
    int best_score = -1;
    for (VkPhysicalDevice device : devices) {
        VkPhysicalDeviceProperties props{};
        vkGetPhysicalDeviceProperties(device, &props);
        int score = 0;
        if (props.deviceType == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU) score += 2;
        else if (props.deviceType == VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU) score += 1;
        if (props.deviceType == VK_PHYSICAL_DEVICE_TYPE_CPU) score -= 100;
        const auto names = device_extensions(device);
        bool all = props.apiVersion >= VK_API_VERSION_1_2;
        for (const char* required : kDmaBufExtensions) {
            all = all && std::find(names.begin(), names.end(), required) != names.end();
        }
        if (all) score += 10;
        if (score > best_score) {
            best_score = score;
            best = device;
        }
    }
    return best;
}

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
    if (initialization_attempted_) {
        return false;
    }
    initialization_attempted_ = true;
    return create_device();
}

bool VulkanRuntime::fail(std::string message) {
    last_error_ = std::move(message);
    return false;
}

bool VulkanRuntime::create_device() {
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

    physical_device_ = pick_physical_device(devices);

    std::uint32_t queue_family_count = 0;
    vkGetPhysicalDeviceQueueFamilyProperties(physical_device_, &queue_family_count, nullptr);
    std::vector<VkQueueFamilyProperties> queue_families(queue_family_count);
    vkGetPhysicalDeviceQueueFamilyProperties(physical_device_, &queue_family_count, queue_families.data());

    // Compute work needs a compute-capable family; fall back to any transfer family for upload-only use.
    bool found_queue_family = false;
    for (std::uint32_t i = 0; i < queue_family_count && !found_queue_family; ++i) {
        if (queue_families[i].queueFlags & VK_QUEUE_COMPUTE_BIT) {
            queue_family_index_ = i;
            found_queue_family = true;
        }
    }
    for (std::uint32_t i = 0; i < queue_family_count && !found_queue_family; ++i) {
        if (queue_families[i].queueFlags & VK_QUEUE_TRANSFER_BIT) {
            queue_family_index_ = i;
            found_queue_family = true;
        }
    }

    std::vector<const char*> enabled_extensions;
    const std::vector<std::string> available = device_extensions(physical_device_);
    const auto has_extension = [&](const char* name) {
        return std::find(available.begin(), available.end(), name) != available.end();
    };
    VkPhysicalDeviceProperties device_properties{};
    vkGetPhysicalDeviceProperties(physical_device_, &device_properties);
    dma_buf_import_supported_ = device_properties.apiVersion >= VK_API_VERSION_1_2;
    for (const char* name : kDmaBufExtensions) {
        dma_buf_import_supported_ = dma_buf_import_supported_ && has_extension(name);
    }
    if (dma_buf_import_supported_) {
        enabled_extensions.assign(std::begin(kDmaBufExtensions), std::end(kDmaBufExtensions));
        if (has_extension(VK_EXT_QUEUE_FAMILY_FOREIGN_EXTENSION_NAME)) {
            enabled_extensions.push_back(VK_EXT_QUEUE_FAMILY_FOREIGN_EXTENSION_NAME);
            queue_family_foreign_enabled_ = true;
        }
        if (has_extension(VK_KHR_EXTERNAL_SEMAPHORE_FD_EXTENSION_NAME)) {
            enabled_extensions.push_back(VK_KHR_EXTERNAL_SEMAPHORE_FD_EXTENSION_NAME);
            sync_file_import_supported_ = true;
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
    device_info.enabledExtensionCount = static_cast<std::uint32_t>(enabled_extensions.size());
    device_info.ppEnabledExtensionNames = enabled_extensions.data();

    if (vkCreateDevice(physical_device_, &device_info, nullptr, &device_) != VK_SUCCESS) {
        dma_buf_import_supported_ = false;
        sync_file_import_supported_ = false;
        queue_family_foreign_enabled_ = false;
        return false;
    }

    vkGetDeviceQueue(device_, queue_family_index_, 0, &queue_);

    if (dma_buf_import_supported_) {
        get_memory_fd_properties_ = reinterpret_cast<PFN_vkGetMemoryFdPropertiesKHR>(vkGetDeviceProcAddr(device_, "vkGetMemoryFdPropertiesKHR"));
        dma_buf_import_supported_ = get_memory_fd_properties_ != nullptr;
    }
    if (sync_file_import_supported_) {
        import_semaphore_fd_ = reinterpret_cast<PFN_vkImportSemaphoreFdKHR>(vkGetDeviceProcAddr(device_, "vkImportSemaphoreFdKHR"));
        sync_file_import_supported_ = import_semaphore_fd_ != nullptr;
    }

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

GpuImage::~GpuImage() {
    if (runtime) {
        runtime->destroy_image(*this);
    }
}

VkFormat VulkanRuntime::to_vk_format(frame::PixelFormat format) {
    switch (format) {
        case frame::PixelFormat::BGRA8:
        case frame::PixelFormat::BGRX8:
            return VK_FORMAT_B8G8R8A8_UNORM;
        case frame::PixelFormat::RGBA8:
        case frame::PixelFormat::RGBX8:
            return VK_FORMAT_R8G8B8A8_UNORM;
        default:
            return VK_FORMAT_UNDEFINED;
    }
}

std::uint32_t VulkanRuntime::external_queue_family() const {
    return queue_family_foreign_enabled_ ? VK_QUEUE_FAMILY_FOREIGN_EXT : VK_QUEUE_FAMILY_EXTERNAL;
}

bool VulkanRuntime::find_memory_type(std::uint32_t type_bits, VkMemoryPropertyFlags required, std::uint32_t& index) const {
    VkPhysicalDeviceMemoryProperties props{};
    vkGetPhysicalDeviceMemoryProperties(physical_device_, &props);
    for (std::uint32_t i = 0; i < props.memoryTypeCount; ++i) {
        if ((type_bits & (1u << i)) && (props.memoryTypes[i].propertyFlags & required) == required) {
            index = i;
            return true;
        }
    }
    return false;
}

VkResult VulkanRuntime::submit(const VkSubmitInfo& submit_info, VkFence fence) {
    std::lock_guard<std::mutex> lock(queue_mutex_);
    return vkQueueSubmit(queue_, 1, &submit_info, fence);
}

VkResult VulkanRuntime::import_sync_file(VkSemaphore semaphore, int fd) {
    if (!sync_file_import_supported_) {
        return VK_ERROR_FEATURE_NOT_PRESENT;
    }
    VkImportSemaphoreFdInfoKHR info{};
    info.sType = VK_STRUCTURE_TYPE_IMPORT_SEMAPHORE_FD_INFO_KHR;
    info.semaphore = semaphore;
    info.flags = VK_SEMAPHORE_IMPORT_TEMPORARY_BIT;
    info.handleType = VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_SYNC_FD_BIT;
    info.fd = fd;
    return import_semaphore_fd_(device_, &info);
}

bool VulkanRuntime::can_import_dma_buf(VkFormat format, std::uint64_t modifier, VkImageUsageFlags usage, std::string* reason) const {
    const auto no = [&](const char* why) {
        if (reason) *reason = why;
        return false;
    };
    if (!initialized_ || !dma_buf_import_supported_) {
        return no("device lacks dma-buf import extensions");
    }
    if (format == VK_FORMAT_UNDEFINED) {
        return no("pixel format has no Vulkan equivalent");
    }

    VkDrmFormatModifierPropertiesListEXT list{};
    list.sType = VK_STRUCTURE_TYPE_DRM_FORMAT_MODIFIER_PROPERTIES_LIST_EXT;
    VkFormatProperties2 props{};
    props.sType = VK_STRUCTURE_TYPE_FORMAT_PROPERTIES_2;
    props.pNext = &list;
    vkGetPhysicalDeviceFormatProperties2(physical_device_, format, &props);
    std::vector<VkDrmFormatModifierPropertiesEXT> modifiers(list.drmFormatModifierCount);
    list.pDrmFormatModifierProperties = modifiers.data();
    vkGetPhysicalDeviceFormatProperties2(physical_device_, format, &props);

    const VkFormatFeatureFlags needed =
        ((usage & VK_IMAGE_USAGE_SAMPLED_BIT) ? VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT : 0u) |
        ((usage & VK_IMAGE_USAGE_STORAGE_BIT) ? VK_FORMAT_FEATURE_STORAGE_IMAGE_BIT : 0u) |
        ((usage & VK_IMAGE_USAGE_TRANSFER_SRC_BIT) ? VK_FORMAT_FEATURE_TRANSFER_SRC_BIT : 0u);
    bool modifier_ok = false;
    for (const auto& m : modifiers) {
        if (m.drmFormatModifier == modifier && m.drmFormatModifierPlaneCount == 1 && (m.drmFormatModifierTilingFeatures & needed) == needed) {
            modifier_ok = true;
        }
    }
    if (!modifier_ok) {
        return no("DRM modifier not supported by the device for this format");
    }

    VkPhysicalDeviceImageDrmFormatModifierInfoEXT mod_info{};
    mod_info.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_IMAGE_DRM_FORMAT_MODIFIER_INFO_EXT;
    mod_info.drmFormatModifier = modifier;
    mod_info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    VkPhysicalDeviceExternalImageFormatInfo ext_info{};
    ext_info.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTERNAL_IMAGE_FORMAT_INFO;
    ext_info.handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT;
    ext_info.pNext = &mod_info;
    VkPhysicalDeviceImageFormatInfo2 image_info{};
    image_info.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_IMAGE_FORMAT_INFO_2;
    image_info.pNext = &ext_info;
    image_info.format = format;
    image_info.type = VK_IMAGE_TYPE_2D;
    image_info.tiling = VK_IMAGE_TILING_DRM_FORMAT_MODIFIER_EXT;
    image_info.usage = usage;
    VkExternalImageFormatProperties ext_props{};
    ext_props.sType = VK_STRUCTURE_TYPE_EXTERNAL_IMAGE_FORMAT_PROPERTIES;
    VkImageFormatProperties2 out{};
    out.sType = VK_STRUCTURE_TYPE_IMAGE_FORMAT_PROPERTIES_2;
    out.pNext = &ext_props;
    if (vkGetPhysicalDeviceImageFormatProperties2(physical_device_, &image_info, &out) != VK_SUCCESS) {
        return no("image format not supported with this DRM modifier");
    }
    if (!(ext_props.externalMemoryProperties.externalMemoryFeatures & VK_EXTERNAL_MEMORY_FEATURE_IMPORTABLE_BIT)) {
        return no("dma-buf import not allowed for this image format");
    }
    return true;
}

std::shared_ptr<GpuImage> VulkanRuntime::import_dma_buf_image(const frame::IFrameInfo& frame, VkImageUsageFlags usage) {
    if (!initialize()) {
        fail("Vulkan runtime unavailable");
        return nullptr;
    }
    const frame::GpuHandle* handle = frame.gpu_handle();
    if (frame.memory_kind() != frame::MemoryKind::DmaBuf || !handle || handle->fds.size() != 1 || handle->fds[0] < 0) {
        fail("frame is not a single-fd dma-buf");
        return nullptr;
    }
    if (frame.plane_count() != 1 || !handle->has_drm_modifier || handle->drm_modifier == DRM_FORMAT_MOD_INVALID) {
        fail("dma-buf needs one plane and an explicit DRM modifier");
        return nullptr;
    }
    if (handle->drm_fourcc != frame::to_drm_fourcc(frame.format())) {
        fail("dma-buf fourcc does not match the frame pixel format");
        return nullptr;
    }
    const VkFormat format = to_vk_format(frame.format());
    std::string reason;
    if (!can_import_dma_buf(format, handle->drm_modifier, usage, &reason)) {
        fail(reason);
        return nullptr;
    }

    const std::uint32_t width = static_cast<std::uint32_t>(frame.width());
    const std::uint32_t height = static_cast<std::uint32_t>(frame.height());
    auto result = std::make_shared<GpuImage>();
    result->runtime = shared_from_this();
    result->format = format;
    result->width = width;
    result->height = height;

    VkSubresourceLayout plane_layout{};
    plane_layout.offset = frame.plane(0).offset;
    plane_layout.rowPitch = frame.plane(0).stride;
    VkImageDrmFormatModifierExplicitCreateInfoEXT explicit_info{};
    explicit_info.sType = VK_STRUCTURE_TYPE_IMAGE_DRM_FORMAT_MODIFIER_EXPLICIT_CREATE_INFO_EXT;
    explicit_info.drmFormatModifier = handle->drm_modifier;
    explicit_info.drmFormatModifierPlaneCount = 1;
    explicit_info.pPlaneLayouts = &plane_layout;
    VkExternalMemoryImageCreateInfo external_info{};
    external_info.sType = VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_IMAGE_CREATE_INFO;
    external_info.pNext = &explicit_info;
    external_info.handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT;
    VkImageCreateInfo image_info{};
    image_info.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    image_info.pNext = &external_info;
    image_info.imageType = VK_IMAGE_TYPE_2D;
    image_info.format = format;
    image_info.extent = {width, height, 1};
    image_info.mipLevels = 1;
    image_info.arrayLayers = 1;
    image_info.samples = VK_SAMPLE_COUNT_1_BIT;
    image_info.tiling = VK_IMAGE_TILING_DRM_FORMAT_MODIFIER_EXT;
    image_info.usage = usage;
    image_info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    image_info.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    if (vkCreateImage(device_, &image_info, nullptr, &result->image) != VK_SUCCESS) {
        fail("vkCreateImage failed for dma-buf");
        return nullptr;
    }

    VkMemoryRequirements requirements{};
    vkGetImageMemoryRequirements(device_, result->image, &requirements);

    VkMemoryFdPropertiesKHR fd_props{};
    fd_props.sType = VK_STRUCTURE_TYPE_MEMORY_FD_PROPERTIES_KHR;
    if (get_memory_fd_properties_(device_, VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT, handle->fds[0], &fd_props) != VK_SUCCESS) {
        fail("vkGetMemoryFdPropertiesKHR failed");
        return nullptr;
    }
    std::uint32_t memory_type = 0;
    if (!find_memory_type(requirements.memoryTypeBits & fd_props.memoryTypeBits, 0, memory_type)) {
        fail("no memory type accepts this dma-buf");
        return nullptr;
    }

    // Vulkan takes ownership of the fd on a successful import, so give it a duplicate.
    const int import_fd = fcntl(handle->fds[0], F_DUPFD_CLOEXEC, 3);
    if (import_fd < 0) {
        fail("failed to duplicate dma-buf fd");
        return nullptr;
    }
    VkImportMemoryFdInfoKHR import_info{};
    import_info.sType = VK_STRUCTURE_TYPE_IMPORT_MEMORY_FD_INFO_KHR;
    import_info.handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT;
    import_info.fd = import_fd;
    VkMemoryDedicatedAllocateInfo dedicated{};
    dedicated.sType = VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO;
    dedicated.pNext = &import_info;
    dedicated.image = result->image;
    VkMemoryAllocateInfo alloc_info{};
    alloc_info.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    alloc_info.pNext = &dedicated;
    alloc_info.allocationSize = requirements.size;
    alloc_info.memoryTypeIndex = memory_type;
    if (vkAllocateMemory(device_, &alloc_info, nullptr, &result->memory) != VK_SUCCESS) {
        close(import_fd);
        fail("vkAllocateMemory failed importing dma-buf");
        return nullptr;
    }
    if (vkBindImageMemory(device_, result->image, result->memory, 0) != VK_SUCCESS) {
        fail("vkBindImageMemory failed");
        return nullptr;
    }

    VkImageViewCreateInfo view_info{};
    view_info.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    view_info.image = result->image;
    view_info.viewType = VK_IMAGE_VIEW_TYPE_2D;
    view_info.format = format;
    view_info.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    if (vkCreateImageView(device_, &view_info, nullptr, &result->view) != VK_SUCCESS) {
        fail("vkCreateImageView failed");
        return nullptr;
    }
    return result;
}

std::shared_ptr<GpuImage> VulkanRuntime::create_image(std::uint32_t width, std::uint32_t height, VkFormat format, VkImageUsageFlags usage) {
    if (!initialize()) {
        fail("Vulkan runtime unavailable");
        return nullptr;
    }
    auto result = std::make_shared<GpuImage>();
    result->runtime = shared_from_this();
    result->format = format;
    result->width = width;
    result->height = height;

    VkImageCreateInfo image_info{};
    image_info.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    image_info.imageType = VK_IMAGE_TYPE_2D;
    image_info.format = format;
    image_info.extent = {width, height, 1};
    image_info.mipLevels = 1;
    image_info.arrayLayers = 1;
    image_info.samples = VK_SAMPLE_COUNT_1_BIT;
    image_info.tiling = VK_IMAGE_TILING_OPTIMAL;
    image_info.usage = usage;
    image_info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    image_info.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    if (vkCreateImage(device_, &image_info, nullptr, &result->image) != VK_SUCCESS) {
        fail("vkCreateImage failed");
        return nullptr;
    }
    VkMemoryRequirements requirements{};
    vkGetImageMemoryRequirements(device_, result->image, &requirements);
    std::uint32_t memory_type = 0;
    if (!find_memory_type(requirements.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, memory_type) &&
        !find_memory_type(requirements.memoryTypeBits, 0, memory_type)) {
        fail("no memory type for image");
        return nullptr;
    }
    VkMemoryAllocateInfo alloc_info{};
    alloc_info.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    alloc_info.allocationSize = requirements.size;
    alloc_info.memoryTypeIndex = memory_type;
    if (vkAllocateMemory(device_, &alloc_info, nullptr, &result->memory) != VK_SUCCESS ||
        vkBindImageMemory(device_, result->image, result->memory, 0) != VK_SUCCESS) {
        fail("failed to allocate image memory");
        return nullptr;
    }
    VkImageViewCreateInfo view_info{};
    view_info.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    view_info.image = result->image;
    view_info.viewType = VK_IMAGE_VIEW_TYPE_2D;
    view_info.format = format;
    view_info.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    if (vkCreateImageView(device_, &view_info, nullptr, &result->view) != VK_SUCCESS) {
        fail("vkCreateImageView failed");
        return nullptr;
    }
    return result;
}

void VulkanRuntime::destroy_image(GpuImage& image) {
    if (image.view != VK_NULL_HANDLE) {
        vkDestroyImageView(device_, image.view, nullptr);
        image.view = VK_NULL_HANDLE;
    }
    if (image.image != VK_NULL_HANDLE) {
        vkDestroyImage(device_, image.image, nullptr);
        image.image = VK_NULL_HANDLE;
    }
    if (image.memory != VK_NULL_HANDLE) {
        vkFreeMemory(device_, image.memory, nullptr);
        image.memory = VK_NULL_HANDLE;
    }
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
