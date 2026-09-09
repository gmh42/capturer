#include "capturer/capture/pipewire_capture.hpp"

#include <algorithm>
#include <cstddef>
#include <vector>

#if HAVE_PIPEWIRE
#include <pipewire/main-loop.h>
#include <pipewire/pipewire.h>
#include <pipewire/stream.h>
#include <spa/param/video/format-utils.h>
#include <spa/param/format-utils.h>
#endif

namespace capturer::capture {

namespace {

#if HAVE_PIPEWIRE
struct PipeWireStreamState {
    pw_stream* stream = nullptr;
    pw_main_loop* loop = nullptr;
    bool initialized = false;
    std::string node_name;
};

static void on_process_cb(pw_stream* stream, void* data) {
    (void)stream;
    (void)data;
}
#endif

}  // namespace

PipeWireCaptureBackend::~PipeWireCaptureBackend() {
#if HAVE_PIPEWIRE
    if (pipewire_initialized_) {
        pw_deinit();
    }
#endif
}

void PipeWireCaptureBackend::set_frame_callback(std::function<void(const PipeWireFrameEvent&)> callback) {
    frame_callback_ = std::move(callback);
}

void PipeWireCaptureBackend::set_stream_config(const PipeWireStreamConfig& config) {
    stream_config_ = config;
    if (!config.node_name.empty()) {
        source_name_ = config.node_name;
    }
}

FrameBuffer PipeWireCaptureBackend::capture_frame(std::size_t width, std::size_t height) {
    FrameBuffer frame{};
    frame.width = width;
    frame.height = height;
    frame.on_gpu = false;
    frame.bytes.resize(width * height, 0);

#if HAVE_PIPEWIRE
    (void)stream_config_;
#endif

    for (std::size_t i = 0; i < frame.bytes.size(); ++i) {
        const std::size_t lane = (i + frame_counter_ + source_name_.size()) % 256;
        frame.bytes[i] = static_cast<std::uint8_t>(lane);
    }
    ++frame_counter_;

    if (runtime_ && runtime_->initialize()) {
        auto dma_image = runtime_->create_dma_buf_image(static_cast<int>(width), static_cast<int>(height));
        if (dma_image) {
            frame.on_gpu = true;
            frame.gpu_buffer = dma_image;
        }
    }

#if HAVE_PIPEWIRE
    if (frame_callback_) {
        PipeWireFrameEvent event{};
        event.width = width;
        event.height = height;
        event.bytes = frame.bytes;
        frame_callback_(event);
    }
#endif

#if HAVE_PIPEWIRE
    if (!pipewire_initialized_) {
        pw_init(nullptr, nullptr);
        pipewire_initialized_ = true;
    }
#endif

    return frame;
}

CaptureResult CapturePipeline::process_frame() {
    constexpr std::size_t width = 64;
    constexpr std::size_t height = 64;

    const auto current = backend_->capture_frame(width, height);
    const auto diff = processor_->process(previous_frame_.bytes, current.bytes, width, height);
    previous_frame_ = current;

    return CaptureResult{processor_->name(), current, diff};
}

}  // namespace capturer::capture
