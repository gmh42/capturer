#include <cassert>
#include <memory>

#include "capturer/capture/pipewire_capture.hpp"
#include "capturer/image_processing/frame_processor.hpp"

int main() {
    auto backend = std::make_unique<capturer::capture::PipeWireCaptureBackend>("test-source");
    auto processor = std::make_unique<capturer::image_processing::TileDifferenceProcessor>(16);
    capturer::capture::CapturePipeline pipeline(std::move(backend), std::move(processor));

    auto result = pipeline.process_frame();

    assert(result.processor_name == "tile-difference");
    assert(result.tile_diff.width == 4);
    assert(result.tile_diff.height == 4);
    assert(result.tile_diff.tiles.size() == 16);
    assert(result.frame.bytes.size() == 4096);
    assert(result.frame.bytes[0] < 256);
    return 0;
}
