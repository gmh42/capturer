#include "capturer/image_processing/image_processor.hpp"
#include "capturer/image_processing/vulkan_tile_processor.hpp"
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

        auto first_frame = capture_source->capture();
        auto second_frame = capture_source->capture();

        std::cout << "capture source: " << capture_source->name() << '\n';
        print_frame_summary(first_frame);

        // Consumes the frames below, so anything that needs their pixels runs first.
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

        capturer::image_processing::ImageProcessorDispatcher processor;
        processor.add(std::make_unique<capturer::image_processing::VulkanTileDifferenceProcessor>(runtime, 16));
        processor.add(std::make_unique<capturer::image_processing::CpuTileDifferenceProcessor>(16));

        const auto first_info = capturer::sample::to_frame_info(std::move(first_frame));
        const auto second_info = capturer::sample::to_frame_info(std::move(second_frame));
        // Both frames stay alive (and their producer buffers reserved) until their GPU work completes.
        processor.process(nullptr, first_info);
        auto result = processor.process(first_info, second_info);

        std::cout << "processor: " << processor.selected_name()
                  << " (memory: " << capturer::frame::to_string(second_info->memory_kind())
                  << ", format: " << capturer::frame::to_string(second_info->format()) << ")\n";
        if (result->skipped()) {
            std::cout << "tiles: skipped, no processor accepts " << capturer::frame::to_string(second_info->memory_kind())
                      << " frames" << '\n';
        } else {
            const auto& diff = result->tile_diff();
            std::size_t changed_tiles = 0;
            for (const auto& tile : diff.tiles) {
                if (tile.changed) {
                    ++changed_tiles;
                }
            }
            std::cout << "tiles: " << diff.width << "x" << diff.height << " changed=" << changed_tiles << '/'
                      << diff.tiles.size() << '\n';
        }

        return 0;
    } catch (const std::exception& error) {
        std::cerr << "capturer_sample: " << error.what() << '\n';
        return 1;
    }
}