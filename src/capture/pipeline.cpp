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

FramePtr PipeWireCaptureBackend::capture_frame(std::size_t width, std::size_t height) {
    // Placeholder frame source: this backend does not read a real PipeWire stream (see PipeWireDesktopCaptureSource).
    const std::size_t stride = width * 4;
    std::vector<std::uint8_t> bytes(stride * height);
    for (std::size_t i = 0; i < bytes.size(); ++i) {
        const std::size_t lane = (i + frame_counter_ + source_name_.size()) % 256;
        bytes[i] = static_cast<std::uint8_t>(lane);
    }
    ++frame_counter_;

#if HAVE_PIPEWIRE
    if (frame_callback_) {
        PipeWireFrameEvent event{};
        event.width = width;
        event.height = height;
        event.bytes = bytes;
        frame_callback_(event);
    }
#endif

#if HAVE_PIPEWIRE
    if (!pipewire_initialized_) {
        pw_init(nullptr, nullptr);
        pipewire_initialized_ = true;
    }
#endif

    return std::make_shared<capturer::frame::CpuFrame>(
        width, height, capturer::frame::PixelFormat::RGBA8, std::move(bytes), stride);
}

CaptureResult CapturePipeline::process_frame() {
    constexpr std::size_t width = 64;
    constexpr std::size_t height = 64;

    auto current = backend_->capture_frame(width, height);
    std::shared_ptr<capturer::image_processing::IProcessResult> result = processor_.process(previous_frame_, current);
    previous_frame_ = current;

    return CaptureResult{processor_.selected_name(), processor_.selected_backend(), std::move(current), std::move(result)};
}

}  // namespace capturer::capture
