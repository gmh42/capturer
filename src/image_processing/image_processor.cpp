#include "capturer/image_processing/image_processor.hpp"

#include <stdexcept>
#include <utility>

namespace capturer::image_processing {

namespace {

class CpuResult final : public IProcessResult {
public:
    explicit CpuResult(TileDiff diff) : diff_(std::move(diff)) {}
    bool skipped() const override { return false; }
    ResultLocation location() const override { return ResultLocation::Cpu; }
    const TileDiff& tile_diff() override { return diff_; }

private:
    TileDiff diff_;
};

class SkippedResult final : public IProcessResult {
public:
    bool skipped() const override { return true; }
    ResultLocation location() const override { return ResultLocation::None; }
    const TileDiff& tile_diff() override { return empty_; }

private:
    TileDiff empty_{};
};

bool same_layout(const frame::IFrameInfo& a, const frame::IFrameInfo& b) {
    return a.width() == b.width() && a.height() == b.height() && a.format() == b.format() &&
           a.memory_kind() == b.memory_kind();
}

}  // namespace

const char* to_string(ProcessorBackend backend) {
    switch (backend) {
    case ProcessorBackend::None: return "none";
    case ProcessorBackend::Cpu: return "cpu";
    case ProcessorBackend::Vulkan: return "vulkan";
    }
    return "unknown";
}

std::unique_ptr<IProcessResult> NullImageProcessor::process(const frame::FramePtr&, const frame::FramePtr&) {
    return std::make_unique<SkippedResult>();
}

CpuTileDifferenceProcessor::CpuTileDifferenceProcessor(std::size_t tile_size) : tile_size_(tile_size) {
    if (tile_size_ == 0) {
        throw std::invalid_argument("tile size must be positive");
    }
}

std::unique_ptr<IProcessResult> CpuTileDifferenceProcessor::process(const frame::FramePtr& previous_frame, const frame::FramePtr& current_frame) {
    if (!current_frame || !can_process(*current_frame)) {
        throw std::invalid_argument("CpuTileDifferenceProcessor cannot process this frame");
    }
    const frame::IFrameInfo& current = *current_frame;
    const frame::IFrameInfo* previous = previous_frame.get();
    if (!previous || !same_layout(*previous, current)) {
        return std::make_unique<CpuResult>(all_tiles_changed(current.width(), current.height(), tile_size_));
    }

    const frame::PixelFormatInfo info = frame::describe(current.format());
    // Packed formats: whole pixel. YUV: luma only, one container per sample.
    const std::size_t bytes_per_pixel = info.is_yuv ? info.container_bits / 8 : info.bits_per_pixel / 8;
    return std::make_unique<CpuResult>(compute_tile_difference(
        previous->cpu_data() + previous->plane(0).offset,
        previous->plane(0).stride,
        current.cpu_data() + current.plane(0).offset,
        current.plane(0).stride,
        current.width(),
        current.height(),
        bytes_per_pixel,
        tile_size_));
}

ImageProcessorDispatcher::ImageProcessorDispatcher()
    : selected_name_(null_processor_.name()) {}

void ImageProcessorDispatcher::add(std::unique_ptr<IImageProcessor> processor) {
    if (!processor) {
        throw std::invalid_argument("processor must not be null");
    }
    processors_.push_back(std::move(processor));
}

IImageProcessor& ImageProcessorDispatcher::select(const frame::IFrameInfo& frame) {
    for (auto& processor : processors_) {
        if (processor->can_process(frame)) {
            return *processor;
        }
    }
    return null_processor_;
}

std::unique_ptr<IProcessResult> ImageProcessorDispatcher::process(const frame::FramePtr& previous, const frame::FramePtr& current) {
    if (!current) {
        throw std::invalid_argument("current frame must not be null");
    }
    IImageProcessor& processor = select(*current);
    selected_name_ = processor.name();
    selected_backend_ = processor.backend();
    fell_back_ = &processor == &null_processor_ && !processors_.empty();
    return processor.process(previous, current);
}

}  // namespace capturer::image_processing
