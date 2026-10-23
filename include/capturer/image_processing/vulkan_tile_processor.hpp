#pragma once

#include <cstddef>
#include <memory>
#include <string>

#include "capturer/gpu/vulkan_runtime.hpp"
#include "capturer/image_processing/image_processor.hpp"

namespace capturer::image_processing {

// Tile difference on the GPU over dma-buf frames.
//
// can_process() only returns true once the runtime can import the frame's dma-buf (see
// VulkanRuntime::supports_dma_buf_import()), so registering this ahead of the CPU processor is safe today:
// the dispatcher falls through to the next processor. The compute pass itself is not implemented yet.
class VulkanTileDifferenceProcessor final : public IImageProcessor {
public:
    VulkanTileDifferenceProcessor(std::shared_ptr<gpu::VulkanRuntime> runtime, std::size_t tile_size);

    std::string name() const override { return "vulkan-tile-difference"; }
    ProcessorBackend backend() const override { return ProcessorBackend::Vulkan; }
    frame::MemoryKindMask supported_memory() const override {
        return frame::mask_of(frame::MemoryKind::DmaBuf) | frame::mask_of(frame::MemoryKind::VulkanImage);
    }
    bool supports_format(frame::PixelFormat format) const override;
    bool can_process(const frame::IFrameInfo& frame) const override;
    std::unique_ptr<IProcessResult> process(const frame::IFrameInfo* previous, const frame::IFrameInfo& current) override;

private:
    std::shared_ptr<gpu::VulkanRuntime> runtime_;
    std::size_t tile_size_;
};

}  // namespace capturer::image_processing
