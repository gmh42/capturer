#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <vector>
#include <vulkan/vulkan.hpp>
#include <xf86drm.h>
#include <drm_fourcc.h>
#include <gbm.h>

#include "capturer/frame/frame_info.hpp"

namespace capturer::gpu {

class VulkanRuntime;

enum class BackendType {
    Auto,
    Vulkan,
    Software
};

struct VulkanRuntimeInfo {
    bool available;
    BackendType backend_type;
    std::string message;
};

struct GpuBuffer {
    std::shared_ptr<VulkanRuntime> runtime;
    VkBuffer buffer = VK_NULL_HANDLE;
    VkDeviceMemory memory = VK_NULL_HANDLE;
    VkImage image = VK_NULL_HANDLE;
    VkDeviceMemory image_memory = VK_NULL_HANDLE;
    std::size_t size = 0;
    bool valid = false;
    bool is_image = false;
    int dma_buf_fd = -1;
    int width = 0;
    int height = 0;

    ~GpuBuffer();
};

// A Vulkan image with its own memory: either imported from a dma-buf or created by the runtime.
struct GpuImage {
    std::shared_ptr<VulkanRuntime> runtime;
    VkImage image = VK_NULL_HANDLE;
    VkDeviceMemory memory = VK_NULL_HANDLE;
    VkImageView view = VK_NULL_HANDLE;
    VkFormat format = VK_FORMAT_UNDEFINED;
    std::uint32_t width = 0;
    std::uint32_t height = 0;

    GpuImage() = default;
    GpuImage(const GpuImage&) = delete;
    GpuImage& operator=(const GpuImage&) = delete;
    ~GpuImage();
};

class VulkanRuntime : public std::enable_shared_from_this<VulkanRuntime> {
public:
    explicit VulkanRuntime(BackendType backend_type = BackendType::Auto);
    ~VulkanRuntime();

    VulkanRuntimeInfo info() const;
    bool supports_gpu() const;
    std::string describe() const;
    // Idempotent. A failed attempt is remembered, so repeated calls do not retry.
    bool initialize();
    std::shared_ptr<GpuBuffer> upload_bytes(const std::vector<std::uint8_t>& bytes);
    std::shared_ptr<GpuBuffer> create_dma_buf_image(int width, int height);
    std::vector<std::uint8_t> download_bytes(const GpuBuffer& buffer);
    void destroy_buffer(GpuBuffer& buffer);

    // dma-buf import. Valid after a successful initialize().
    //
    // True when the selected device has the external-memory dma-buf and DRM format modifier extensions.
    bool supports_dma_buf_import() const { return dma_buf_import_supported_; }
    // True when a sync_file can be imported as a semaphore, so work can wait on a dma-buf's fences explicitly.
    bool supports_sync_file_import() const { return sync_file_import_supported_; }
    // Checks that `modifier` can be imported for `format` as a single-plane image usable with `usage`.
    // On failure returns false and, when `reason` is given, says why.
    bool can_import_dma_buf(VkFormat format, std::uint64_t modifier, VkImageUsageFlags usage, std::string* reason = nullptr) const;
    // Imports the frame's dma-buf as a sampled image. The frame keeps its own fd; the image holds a duplicate.
    // Returns nullptr and sets last_error() on failure. Only single-plane RGB frames with an explicit modifier are supported.
    std::shared_ptr<GpuImage> import_dma_buf_image(const frame::IFrameInfo& frame, VkImageUsageFlags usage = VK_IMAGE_USAGE_SAMPLED_BIT);
    // Device-local optimally tiled 2D image, e.g. for state kept between frames.
    std::shared_ptr<GpuImage> create_image(std::uint32_t width, std::uint32_t height, VkFormat format, VkImageUsageFlags usage);
    void destroy_image(GpuImage& image);
    // Maps a frame pixel format to the Vulkan format that reads its bytes in logical RGBA order, or VK_FORMAT_UNDEFINED.
    static VkFormat to_vk_format(frame::PixelFormat format);

    // Raw handles for compute work. Valid after a successful initialize().
    VkDevice device() const { return device_; }
    VkPhysicalDevice physical_device() const { return physical_device_; }
    std::uint32_t queue_family_index() const { return queue_family_index_; }
    // The queue family value to use when acquiring/releasing dma-buf images across the external boundary.
    std::uint32_t external_queue_family() const;
    VkResult submit(const VkSubmitInfo& submit_info, VkFence fence);
    // Imports a sync_file into a binary semaphore as a temporary payload. Takes ownership of `fd` only on success.
    VkResult import_sync_file(VkSemaphore semaphore, int fd);
    bool find_memory_type(std::uint32_t type_bits, VkMemoryPropertyFlags required, std::uint32_t& index) const;
    const std::string& last_error() const { return last_error_; }

private:
    bool create_device();
    bool fail(std::string message);

    BackendType backend_type_;
    bool initialized_ = false;
    bool initialization_attempted_ = false;
    bool dma_buf_import_supported_ = false;
    bool sync_file_import_supported_ = false;
    bool queue_family_foreign_enabled_ = false;
    VkInstance instance_ = VK_NULL_HANDLE;
    VkPhysicalDevice physical_device_ = VK_NULL_HANDLE;
    VkDevice device_ = VK_NULL_HANDLE;
    VkQueue queue_ = VK_NULL_HANDLE;
    std::mutex queue_mutex_;
    std::uint32_t queue_family_index_ = 0;
    gbm_device* gbm_device_ = nullptr;
    int gbm_device_fd_ = -1;
    PFN_vkGetMemoryFdPropertiesKHR get_memory_fd_properties_ = nullptr;
    PFN_vkImportSemaphoreFdKHR import_semaphore_fd_ = nullptr;
    std::string last_error_;
};

std::vector<std::uint8_t> run_tile_difference_shader(
    const std::vector<std::uint8_t>& previous,
    const std::vector<std::uint8_t>& current,
    std::size_t width,
    std::size_t height,
    std::size_t tile_size,
    const VulkanRuntime& runtime);

}  // namespace capturer::gpu
