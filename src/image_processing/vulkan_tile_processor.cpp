#include "capturer/image_processing/vulkan_tile_processor.hpp"

#include <stdexcept>
#include <utility>

namespace capturer::image_processing {

VulkanTileDifferenceProcessor::VulkanTileDifferenceProcessor(std::shared_ptr<gpu::VulkanRuntime> runtime, std::size_t tile_size)
    : runtime_(std::move(runtime)), tile_size_(tile_size) {
    if (!runtime_ || tile_size_ == 0) {
        throw std::invalid_argument("Vulkan runtime and a positive tile size are required");
    }
}

bool VulkanTileDifferenceProcessor::supports_format(frame::PixelFormat format) const {
    const frame::PixelFormatInfo info = frame::describe(format);
    return info.plane_count == 1 && !info.is_yuv;
}

bool VulkanTileDifferenceProcessor::can_process(const frame::IFrameInfo& frame) const {
    return IImageProcessor::can_process(frame) && runtime_->supports_dma_buf_import() && frame.gpu_handle() != nullptr;
}

std::unique_ptr<IProcessResult> VulkanTileDifferenceProcessor::process(const frame::IFrameInfo*, const frame::IFrameInfo&) {
    throw std::runtime_error("VulkanTileDifferenceProcessor: dma-buf import and the compute pass are not implemented");
}

}  // namespace capturer::image_processing
