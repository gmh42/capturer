#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>

#include "capturer/gpu/vulkan_runtime.hpp"
#include "capturer/image_processing/image_processor.hpp"

namespace capturer::image_processing {

// Tile difference on the GPU over dma-buf frames.
//
// Both the current and the previous frame are imported as Vulkan images (no copy) and compared in a compute pass.
// The processor remembers the image of the last frame it processed, so in steady state each frame is imported once
// and then serves as `previous` for the next call: a ping-pong between two live buffers. That relies on the previous
// frame's producer buffer still being reserved, which holds while the caller keeps its FramePtr and the capture
// source withholds buffers until release (GpuHandle::lifetime). Jobs keep both frames alive until the GPU is done.
//
// A null, mismatched (size/format) or non-importable `previous` reports all tiles changed.
// can_process() is false when the device cannot import the frame (no extensions, unsupported modifier, no
// explicit modifier), so the dispatcher falls through to the next processor. Not thread-safe.
// X formats (BGRX/RGBX) ignore the padding byte; the CPU processor compares it.
class VulkanTileDifferenceProcessor final : public IImageProcessor {
public:
    VulkanTileDifferenceProcessor(std::shared_ptr<gpu::VulkanRuntime> runtime, std::size_t tile_size);
    ~VulkanTileDifferenceProcessor() override;

    std::string name() const override { return "vulkan-tile-difference"; }
    ProcessorBackend backend() const override { return ProcessorBackend::Vulkan; }
    frame::MemoryKindMask supported_memory() const override {
        return frame::mask_of(frame::MemoryKind::DmaBuf);
    }
    bool supports_format(frame::PixelFormat format) const override;
    bool can_process(const frame::IFrameInfo& frame) const override;
    std::unique_ptr<IProcessResult> process(const frame::FramePtr& previous, const frame::FramePtr& current) override;

private:
    struct Pipeline;

    std::shared_ptr<gpu::VulkanRuntime> runtime_;
    std::size_t tile_size_;
    std::shared_ptr<Pipeline> pipeline_;
    // Image of the most recently processed frame. Weak so the processor never extends a producer buffer's lifetime.
    std::weak_ptr<const frame::IFrameInfo> last_frame_;
    std::shared_ptr<gpu::GpuImage> last_image_;
};

}  // namespace capturer::image_processing
