#include "capturer/sample/desktop_capture.hpp"

#if HAVE_PIPEWIRE

#include <chrono>
#include <cstddef>
#include <exception>
#include <iostream>
#include <utility>

#include <unistd.h>

namespace {

void release_dma_buf(capturer::sample::DesktopFrame& frame) {
    if (frame.has_dma_buf()) {
        close(frame.dma_buf_fd);
        frame.dma_buf_fd = -1;
    }
}

}  // namespace

int main() {
    try {
        std::cout << "opening desktop capture portal..." << std::endl;
        auto config = capturer::sample::open_wayland_portal_capture();
        std::cout << "portal capture configured; connecting PipeWire stream..." << std::endl;
        capturer::sample::PipeWireDesktopCaptureSource capture_source(std::move(config));
        std::cout << "PipeWire stream connected; capturing warm-up frame..." << std::endl;

        auto warmup_frame = capture_source.capture();
        std::cout << "warm-up frame received: " << warmup_frame.width << 'x' << warmup_frame.height
                  << " (" << (warmup_frame.has_dma_buf() ? "dma-buf" : "cpu-memory") << ")\n";
        release_dma_buf(warmup_frame);

        constexpr auto benchmark_duration = std::chrono::seconds(3);
        std::cout << "starting three-second benchmark..." << std::endl;
        const auto start = std::chrono::steady_clock::now();
        const auto deadline = start + benchmark_duration;
        std::size_t frame_count = 0;
        std::size_t width = 0;
        std::size_t height = 0;
        std::size_t polling_timeouts = 0;
        std::size_t dma_buf_frames = 0;
        std::size_t cpu_memory_frames = 0;

        while (std::chrono::steady_clock::now() < deadline) {
            auto frame = capture_source.try_capture(std::chrono::milliseconds(50));
            if (frame) {
                width = frame->width;
                height = frame->height;
                ++frame_count;
                if (frame->has_dma_buf()) {
                    ++dma_buf_frames;
                } else {
                    ++cpu_memory_frames;
                }
                release_dma_buf(*frame);
            } else {
                ++polling_timeouts;
            }
        }

        const auto elapsed = std::chrono::steady_clock::now() - start;
        const auto elapsed_seconds = std::chrono::duration<double>(elapsed).count();
        const double frames_per_second = static_cast<double>(frame_count) / elapsed_seconds;

        std::cout << "captured " << frame_count << " frames at " << width << 'x' << height
                  << " in " << elapsed_seconds << " seconds\n"
                  << "capture fps: " << frames_per_second
                  << " (dma-buf frames: " << dma_buf_frames
                  << ", cpu-memory frames: " << cpu_memory_frames
                  << ", no-frame polls: " << polling_timeouts << ")\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "capturer_pipewire_benchmark: " << error.what() << '\n';
        return 1;
    }
}

#else

#include <iostream>

int main() {
    std::cerr << "capturer_pipewire_benchmark requires PipeWire development headers and libraries\n";
    return 1;
}

#endif
