#include "capturer/sample/desktop_capture.hpp"

#if HAVE_PIPEWIRE

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <exception>
#include <iostream>
#include <string>
#include <utility>
#include <vector>

#include <unistd.h>

namespace {

void release_dma_buf(capturer::sample::DesktopFrame& frame) {
    if (frame.has_dma_buf()) {
        close(frame.dma_buf_fd);
        frame.dma_buf_fd = -1;
    }
    // Returns the producer's buffer to the compositor.
    frame.buffer_lease.reset();
}

void print_intervals(const char* label, std::vector<double> intervals_ms) {
    if (intervals_ms.empty()) {
        std::cout << label << ": no data\n";
        return;
    }
    std::sort(intervals_ms.begin(), intervals_ms.end());
    double sum = 0.0;
    for (const double value : intervals_ms) {
        sum += value;
    }
    const double mean = sum / static_cast<double>(intervals_ms.size());
    double variance = 0.0;
    for (const double value : intervals_ms) {
        variance += (value - mean) * (value - mean);
    }
    const auto percentile = [&](double fraction) {
        return intervals_ms[static_cast<std::size_t>(fraction * static_cast<double>(intervals_ms.size() - 1))];
    };
    std::cout << label << " (ms): mean " << mean
              << ", stddev " << std::sqrt(variance / static_cast<double>(intervals_ms.size()))
              << ", min " << intervals_ms.front()
              << ", p50 " << percentile(0.5)
              << ", p99 " << percentile(0.99)
              << ", max " << intervals_ms.back() << '\n';
}

void run_benchmark(bool allow_dma_buf) {
    {
        std::cout << "opening desktop capture portal..." << std::endl;
        auto config = capturer::sample::open_wayland_portal_capture();
        config.allow_dma_buf = allow_dma_buf;
        std::cout << "portal capture configured; connecting PipeWire stream..." << std::endl;
        capturer::sample::PipeWireDesktopCaptureSource capture_source(std::move(config));
        std::cout << "PipeWire stream connected; capturing warm-up frame..." << std::endl;

        auto warmup_frame = capture_source.capture();
        std::cout << "warm-up frame received: " << warmup_frame.width << 'x' << warmup_frame.height
                  << " (" << (warmup_frame.has_dma_buf() ? "dma-buf" : "cpu-memory") << "), negotiated max fps: "
                  << capture_source.negotiated_max_framerate() << '\n';
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
        std::size_t header_frames = 0;
        std::size_t missed_frames = 0;
        std::size_t sequence_resets = 0;
        std::uint64_t last_sequence = 0;
        bool have_last_sequence = false;
        std::vector<double> arrival_intervals_ms;
        std::vector<double> pts_intervals_ms;
        std::int64_t last_pts_ns = 0;
        std::chrono::steady_clock::time_point last_arrival{};

        while (std::chrono::steady_clock::now() < deadline) {
            auto frame = capture_source.try_capture(std::chrono::milliseconds(50));
            if (frame) {
                width = frame->width;
                height = frame->height;
                ++frame_count;
                const auto arrival = std::chrono::steady_clock::now();
                if (frame_count > 1) {
                    arrival_intervals_ms.push_back(std::chrono::duration<double, std::milli>(arrival - last_arrival).count());
                }
                last_arrival = arrival;
                if (frame->has_header) {
                    ++header_frames;
                    if (have_last_sequence) {
                        if (frame->sequence > last_sequence) {
                            missed_frames += static_cast<std::size_t>(frame->sequence - last_sequence - 1);
                        } else {
                            ++sequence_resets;
                        }
                        if (frame->pts_ns > last_pts_ns) {
                            pts_intervals_ms.push_back(static_cast<double>(frame->pts_ns - last_pts_ns) / 1e6);
                        }
                    }
                    last_sequence = frame->sequence;
                    last_pts_ns = frame->pts_ns;
                    have_last_sequence = true;
                }
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

        if (header_frames == 0) {
            std::cout << "producer sequence: unavailable (no header meta on buffers)\n";
        } else {
            std::cout << "producer sequence: " << missed_frames << " frames produced but not delivered"
                      << " (" << sequence_resets << " non-increasing sequence numbers)\n";
        }
        print_intervals("arrival interval", arrival_intervals_ms);
        print_intervals("producer pts interval", pts_intervals_ms);
    }
}

}  // namespace

int main(int argc, char** argv) {
    // --buffers=auto offers dma-buf with a CPU fallback; cpu offers CPU buffers only; both runs cpu, then auto.
    std::string mode = "auto";
    for (int index = 1; index < argc; ++index) {
        const std::string argument = argv[index];
        if (argument.rfind("--buffers=", 0) == 0) {
            mode = argument.substr(10);
        } else {
            std::cerr << "usage: capturer_pipewire_benchmark [--buffers=auto|cpu|both]\n";
            return 2;
        }
    }
    if (mode != "auto" && mode != "cpu" && mode != "both") {
        std::cerr << "unknown --buffers value: " << mode << " (expected auto, cpu or both)\n";
        return 2;
    }

    try {
        if (mode == "cpu" || mode == "both") {
            std::cout << "=== CPU buffers ===\n";
            run_benchmark(false);
        }
        if (mode == "auto" || mode == "both") {
            std::cout << "=== dma-buf (auto) ===\n";
            run_benchmark(true);
        }
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
