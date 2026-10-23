#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>
#include <vulkan/vulkan.hpp>
#include <xf86drm.h>
#include <drm_fourcc.h>
#include <gbm.h>

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

class VulkanRuntime : public std::enable_shared_from_this<VulkanRuntime> {
public:
    explicit VulkanRuntime(BackendType backend_type = BackendType::Auto);
    ~VulkanRuntime();

    VulkanRuntimeInfo info() const;
    bool supports_gpu() const;
    std::string describe() const;
    bool initialize();
    // True once external dma-buf frames can be imported as Vulkan images. Export-only today, so always false.
    bool supports_dma_buf_import() const { return false; }
    std::shared_ptr<GpuBuffer> upload_bytes(const std::vector<std::uint8_t>& bytes);
    std::shared_ptr<GpuBuffer> create_dma_buf_image(int width, int height);
    std::vector<std::uint8_t> download_bytes(const GpuBuffer& buffer);
    void destroy_buffer(GpuBuffer& buffer);

private:
    BackendType backend_type_;
    bool initialized_ = false;
    VkInstance instance_ = VK_NULL_HANDLE;
    VkPhysicalDevice physical_device_ = VK_NULL_HANDLE;
    VkDevice device_ = VK_NULL_HANDLE;
    VkQueue queue_ = VK_NULL_HANDLE;
    std::uint32_t queue_family_index_ = 0;
    gbm_device* gbm_device_ = nullptr;
    int gbm_device_fd_ = -1;
};

std::vector<std::uint8_t> run_tile_difference_shader(
    const std::vector<std::uint8_t>& previous,
    const std::vector<std::uint8_t>& current,
    std::size_t width,
    std::size_t height,
    std::size_t tile_size,
    const VulkanRuntime& runtime);

}  // namespace capturer::gpu
