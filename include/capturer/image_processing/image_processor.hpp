#pragma once

#include <cstddef>
#include <memory>
#include <string>
#include <vector>

#include "capturer/frame/frame_info.hpp"
#include "capturer/image_processing/tile_difference.hpp"

namespace capturer::image_processing {

enum class ProcessorBackend { None, Cpu, Vulkan };
enum class ResultLocation { None, Cpu, Gpu };

const char* to_string(ProcessorBackend backend);

// Output of a processor. A GPU-resident result is read back lazily by tile_diff() and cached.
class IProcessResult {
public:
    virtual ~IProcessResult() = default;

    // True when the processor did no work (skipped); tile_diff() is then empty.
    virtual bool skipped() const = 0;
    virtual ResultLocation location() const = 0;
    virtual const TileDiff& tile_diff() = 0;
};

class IImageProcessor {
public:
    virtual ~IImageProcessor() = default;

    virtual std::string name() const = 0;
    virtual ProcessorBackend backend() const = 0;
    virtual frame::MemoryKindMask supported_memory() const = 0;
    virtual bool supports_format(frame::PixelFormat format) const = 0;

    // True when this processor can handle the frame as given, without any CPU/GPU copy.
    virtual bool can_process(const frame::IFrameInfo& frame) const {
        return (supported_memory() & frame::mask_of(frame.memory_kind())) != 0 && supports_format(frame.format());
    }

    // previous is null for the first frame. A previous frame with different dimensions or format is treated as absent.
    // Callers must check can_process() first; process() throws std::invalid_argument for unsupported frames.
    virtual std::unique_ptr<IProcessResult> process(const frame::IFrameInfo* previous, const frame::IFrameInfo& current) = 0;
};

// Skips all work. Used as the final fall-through when no real processor accepts a frame.
class NullImageProcessor final : public IImageProcessor {
public:
    std::string name() const override { return "none"; }
    ProcessorBackend backend() const override { return ProcessorBackend::None; }
    frame::MemoryKindMask supported_memory() const override { return ~frame::MemoryKindMask{0}; }
    bool supports_format(frame::PixelFormat) const override { return true; }
    std::unique_ptr<IProcessResult> process(const frame::IFrameInfo* previous, const frame::IFrameInfo& current) override;
};

// Tile difference in software over CPU-memory frames. Packed RGB formats compare every byte;
// YUV formats compare the luma plane only.
class CpuTileDifferenceProcessor final : public IImageProcessor {
public:
    explicit CpuTileDifferenceProcessor(std::size_t tile_size);

    std::string name() const override { return "cpu-tile-difference"; }
    ProcessorBackend backend() const override { return ProcessorBackend::Cpu; }
    frame::MemoryKindMask supported_memory() const override { return frame::mask_of(frame::MemoryKind::Cpu); }
    bool supports_format(frame::PixelFormat format) const override { return format != frame::PixelFormat::Unknown; }
    std::unique_ptr<IProcessResult> process(const frame::IFrameInfo* previous, const frame::IFrameInfo& current) override;

private:
    std::size_t tile_size_;
};

// Picks a processor per frame: the first registered processor whose can_process() accepts it, otherwise the
// null processor. It never converts between memory kinds, so a mismatch is visible through selected_backend().
class ImageProcessorDispatcher {
public:
    ImageProcessorDispatcher();

    // Earlier processors win, so register the preferred backend (e.g. Vulkan) first.
    void add(std::unique_ptr<IImageProcessor> processor);

    IImageProcessor& select(const frame::IFrameInfo& frame);
    std::unique_ptr<IProcessResult> process(const frame::IFrameInfo* previous, const frame::IFrameInfo& current);

    // Name and backend of the processor used by the most recent process() call.
    const std::string& selected_name() const { return selected_name_; }
    ProcessorBackend selected_backend() const { return selected_backend_; }
    // True when the most recent frame matched no registered processor.
    bool fell_back() const { return fell_back_; }

private:
    std::vector<std::unique_ptr<IImageProcessor>> processors_;
    NullImageProcessor null_processor_;
    std::string selected_name_;
    ProcessorBackend selected_backend_ = ProcessorBackend::None;
    bool fell_back_ = false;
};

}  // namespace capturer::image_processing
