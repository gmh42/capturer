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
// Each frame's dma-buf is imported as a Vulkan image (no copy), compared on a compute queue against a device-local
// copy of the previously processed frame, and stored as the next baseline (the producer recycles its buffers, so
// the previous dma-buf cannot be re-read). The result stays on the GPU until tile_diff() reads it back.
//
// Contract: `previous` must be the frame passed as `current` on the preceding call, or null to restart. Any other
// previous (different size/format/sequence/pts) restarts the comparison and reports all tiles changed.
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
    std::unique_ptr<IProcessResult> process(const frame::IFrameInfo* previous, const frame::IFrameInfo& current) override;

private:
    struct Pipeline;

    std::shared_ptr<gpu::VulkanRuntime> runtime_;
    std::size_t tile_size_;
    std::shared_ptr<Pipeline> pipeline_;
    std::shared_ptr<gpu::GpuImage> state_image_;
    bool state_initialized_ = false;
    std::size_t state_width_ = 0;
    std::size_t state_height_ = 0;
    frame::PixelFormat state_format_ = frame::PixelFormat::RGBA8;
    std::uint64_t last_sequence_ = 0;
    std::int64_t last_pts_ns_ = 0;
};

}  // namespace capturer::image_processing
