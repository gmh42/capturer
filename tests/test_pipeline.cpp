#include <cstdlib>
#include <iostream>
#include <memory>

#include "capturer/capture/pipewire_capture.hpp"
#include "capturer/image_processing/image_processor.hpp"

#define CHECK(condition)                                                                    \
    do {                                                                                    \
        if (!(condition)) {                                                                 \
            std::cerr << __FILE__ << ':' << __LINE__ << ": CHECK failed: " #condition "\n"; \
            std::exit(1);                                                                   \
        }                                                                                   \
    } while (false)

int main() {
    using namespace capturer::image_processing;

    auto backend = std::make_unique<capturer::capture::PipeWireCaptureBackend>("test-source");
    ImageProcessorDispatcher dispatcher;
    dispatcher.add(std::make_unique<CpuTileDifferenceProcessor>(16));
    capturer::capture::CapturePipeline pipeline(std::move(backend), std::move(dispatcher));

    auto first = pipeline.process_frame();
    CHECK(first.processor_name == "cpu-tile-difference");
    CHECK(first.backend == ProcessorBackend::Cpu);
    CHECK(first.frame->is_cpu() && first.frame->width() == 64 && first.frame->height() == 64);
    CHECK(first.frame->format() == capturer::frame::PixelFormat::RGBA8 && first.frame->stride() == 256);
    const auto& first_diff = first.result->tile_diff();
    CHECK(first_diff.width == 4 && first_diff.height == 4 && first_diff.tiles.size() == 16);
    for (const auto& tile : first_diff.tiles) {
        CHECK(tile.changed);  // nothing to compare the first frame with
    }

    // The placeholder backend shifts its pattern every frame, so the second frame differs from the first.
    auto second = pipeline.process_frame();
    CHECK(second.result->tile_diff().tiles.size() == 16);
    CHECK(second.result->tile_diff().tiles[0].changed);
    return 0;
}
