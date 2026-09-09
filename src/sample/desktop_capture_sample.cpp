#include "capturer/image_processing/tile_difference.hpp"
#include "capturer/gpu/vulkan_runtime.hpp"
#include "capturer/sample/desktop_capture.hpp"

#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#if HAVE_PIPEWIRE
namespace capturer::sample {
PipeWireDesktopCaptureConfig open_wayland_portal_capture();
}
#endif

namespace {

std::unique_ptr<capturer::sample::DesktopCaptureSource> make_capture_source() {
#if HAVE_PIPEWIRE
    return std::make_unique<capturer::sample::PipeWireDesktopCaptureSource>(
        capturer::sample::open_wayland_portal_capture());
#endif

    const char* display_name = std::getenv("DISPLAY");
    return std::make_unique<capturer::sample::X11DesktopCaptureSource>(display_name ? display_name : "");
}

void print_frame_summary(const capturer::sample::DesktopFrame& frame) {
    std::cout << "frame: " << frame.width << "x" << frame.height << " stride=" << frame.stride << '\n';
    std::cout << "storage: " << (frame.has_dma_buf() ? "dma-buf" : "cpu-rgba8") << '\n';
    if (frame.has_dma_buf()) {
        std::cout << "dma-buf-fd: " << frame.dma_buf_fd << '\n';
    }
}

}  // namespace

int main() {
    try {
        auto capture_source = make_capture_source();
        auto runtime = std::make_shared<capturer::gpu::VulkanRuntime>();

        const auto first_frame = capture_source->capture();
        const auto second_frame = capture_source->capture();

        std::cout << "capture source: " << capture_source->name() << '\n';
        print_frame_summary(first_frame);

        if (!first_frame.pixels.empty() && !second_frame.pixels.empty() && first_frame.pixels.size() == second_frame.pixels.size()) {
            std::vector<std::uint8_t> previous(first_frame.pixels.begin(), first_frame.pixels.end());
            std::vector<std::uint8_t> current(second_frame.pixels.begin(), second_frame.pixels.end());
            const auto diff = capturer::image_processing::compute_tile_difference(
                previous,
                current,
                first_frame.width * 4,
                first_frame.height,
                16);

            std::cout << "tiles: " << diff.width << "x" << diff.height << " changed=";

            std::size_t changed_tiles = 0;
            for (const auto& tile : diff.tiles) {
                if (tile.changed) {
                    ++changed_tiles;
                }
            }
            std::cout << changed_tiles << '/' << diff.tiles.size() << '\n';
        } else {
            std::cout << "tiles: skipped for dma-buf frame" << '\n';
        }

        if (!first_frame.pixels.empty() && runtime->initialize()) {
            auto gpu_buffer = runtime->upload_bytes(first_frame.pixels);
            std::cout << "vulkan: " << runtime->describe();
            if (gpu_buffer && gpu_buffer->valid) {
                std::cout << " uploaded=" << gpu_buffer->size << " bytes";
            }
            std::cout << '\n';
        } else {
            std::cout << "vulkan: skipped" << '\n';
        }

        return 0;
    } catch (const std::exception& error) {
        std::cerr << "capturer_sample: " << error.what() << '\n';
        return 1;
    }
}