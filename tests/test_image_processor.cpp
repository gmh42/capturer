#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <memory>
#include <vector>

#include "capturer/frame/frame_info.hpp"
#include "capturer/gpu/vulkan_runtime.hpp"
#include "capturer/image_processing/image_processor.hpp"
#include "capturer/image_processing/tile_difference.hpp"
#include "capturer/image_processing/vulkan_tile_processor.hpp"

#define CHECK(condition)                                                                    \
    do {                                                                                    \
        if (!(condition)) {                                                                 \
            std::cerr << __FILE__ << ':' << __LINE__ << ": CHECK failed: " #condition "\n"; \
            std::exit(1);                                                                   \
        }                                                                                   \
    } while (false)

using namespace capturer::frame;
using namespace capturer::image_processing;

namespace {

CpuFrame make_rgba(std::size_t width, std::size_t height, std::size_t stride = 0) {
    const std::size_t row = stride != 0 ? stride : width * 4;
    return CpuFrame(width, height, PixelFormat::RGBA8, std::vector<std::uint8_t>(row * height, 0), stride);
}

std::shared_ptr<GpuFrame> make_dma_frame() {
    GpuHandle handle;
    handle.fds = {-1};
    return std::make_shared<GpuFrame>(16, 16, PixelFormat::BGRX8, MemoryKind::DmaBuf, handle, default_planes(PixelFormat::BGRX8, 16, 16));
}

void test_tile_difference() {
    std::vector<std::uint8_t> prev(64 * 64), curr(64 * 64);
    for (std::size_t i = 0; i < prev.size(); ++i) {
        prev[i] = static_cast<std::uint8_t>(i % 256);
        curr[i] = static_cast<std::uint8_t>((i + 1) % 256);
    }
    const auto diff = compute_tile_difference(prev, curr, 64, 64, 16);
    CHECK(diff.width == 4 && diff.height == 4 && diff.tiles.size() == 16);
    CHECK(diff.tiles[0].changed && diff.tiles[0].value == 1 && diff.tiles[15].changed);

    const auto same = compute_tile_difference(prev, prev, 64, 64, 16);
    for (const auto& tile : same.tiles) {
        CHECK(!tile.changed);
    }

    // A change that preserves the tile's byte sum must still be detected.
    auto swapped = prev;
    std::swap(swapped[0], swapped[1]);
    CHECK(compute_tile_difference(prev, swapped, 64, 64, 16).tiles[0].changed);
}

void test_cpu_processor_strided_pixels() {
    // 20x20 RGBA with 16-pixel tiles gives 2x2 tiles; rows are padded to 100 bytes.
    CpuTileDifferenceProcessor processor(16);
    auto previous = make_rgba(20, 20, 100);
    auto bytes = std::vector<std::uint8_t>(100 * 20, 0);
    bytes[19 * 100 + 19 * 4 + 3] = 1;  // last pixel, alpha byte: tile (1,1)
    CpuFrame current(20, 20, PixelFormat::RGBA8, bytes, 100);

    CHECK(processor.can_process(current));
    auto result = processor.process(&previous, current);
    CHECK(!result->skipped() && result->location() == ResultLocation::Cpu);
    const auto& diff = result->tile_diff();
    CHECK(diff.width == 2 && diff.height == 2);
    CHECK(!diff.tiles[0].changed && !diff.tiles[1].changed && !diff.tiles[2].changed && diff.tiles[3].changed);

    // Padding bytes beyond the visible row must not cause a change.
    bytes[19 * 100 + 99] = 5;
    CpuFrame padded(20, 20, PixelFormat::RGBA8, bytes, 100);
    CHECK(!processor.process(&current, padded)->tile_diff().tiles[3].changed);
}

void test_cpu_processor_first_frame_and_mismatch() {
    CpuTileDifferenceProcessor processor(16);
    auto frame = make_rgba(32, 32);
    auto first = processor.process(nullptr, frame);
    for (const auto& tile : first->tile_diff().tiles) {
        CHECK(tile.changed);
    }

    auto other_size = make_rgba(16, 16);
    CHECK(processor.process(&other_size, frame)->tile_diff().tiles[0].changed);
}

void test_cpu_processor_yuv_luma() {
    CpuTileDifferenceProcessor processor(8);
    const auto planes = default_planes(PixelFormat::NV12, 16, 16);
    std::vector<std::uint8_t> a(planes[1].offset + planes[1].size, 0);
    auto b = a;
    b[planes[1].offset] = 9;  // chroma only: ignored
    CpuFrame fa(16, 16, PixelFormat::NV12, a), fb(16, 16, PixelFormat::NV12, b);
    auto chroma_only = processor.process(&fa, fb);
    for (const auto& tile : chroma_only->tile_diff().tiles) {
        CHECK(!tile.changed);
    }
    b[0] = 3;  // luma
    CpuFrame fc(16, 16, PixelFormat::NV12, b);
    CHECK(processor.process(&fa, fc)->tile_diff().tiles[0].changed);
}

void test_cpu_processor_rejects_gpu() {
    CpuTileDifferenceProcessor processor(16);
    auto dma = make_dma_frame();
    CHECK(!processor.can_process(*dma));
    bool threw = false;
    try {
        processor.process(nullptr, *dma);
    } catch (const std::invalid_argument&) {
        threw = true;
    }
    CHECK(threw);
}

void test_null_processor() {
    NullImageProcessor processor;
    auto frame = make_rgba(16, 16);
    auto result = processor.process(nullptr, frame);
    CHECK(result->skipped() && result->location() == ResultLocation::None && result->tile_diff().tiles.empty());
}

void test_dispatcher() {
    ImageProcessorDispatcher dispatcher;
    auto runtime = std::make_shared<capturer::gpu::VulkanRuntime>();
    dispatcher.add(std::make_unique<VulkanTileDifferenceProcessor>(runtime, 16));
    dispatcher.add(std::make_unique<CpuTileDifferenceProcessor>(16));

    auto cpu = make_rgba(32, 32);
    auto result = dispatcher.process(nullptr, cpu);
    CHECK(dispatcher.selected_backend() == ProcessorBackend::Cpu && !dispatcher.fell_back() && !result->skipped());

    // Vulkan cannot import dma-bufs yet and the CPU processor refuses GPU memory, so this falls through to skip.
    auto dma = make_dma_frame();
    result = dispatcher.process(nullptr, *dma);
    CHECK(dispatcher.selected_backend() == ProcessorBackend::None && dispatcher.fell_back() && result->skipped());

    ImageProcessorDispatcher empty;
    CHECK(empty.process(nullptr, cpu)->skipped() && !empty.fell_back());
}

void test_vulkan_processor_gated() {
    auto runtime = std::make_shared<capturer::gpu::VulkanRuntime>();
    VulkanTileDifferenceProcessor processor(runtime, 16);
    auto dma = make_dma_frame();
    CHECK(!processor.can_process(*dma));
    auto cpu = make_rgba(16, 16);
    CHECK(!processor.can_process(cpu));
}

}  // namespace

int main() {
    test_tile_difference();
    test_cpu_processor_strided_pixels();
    test_cpu_processor_first_frame_and_mismatch();
    test_cpu_processor_yuv_luma();
    test_cpu_processor_rejects_gpu();
    test_null_processor();
    test_dispatcher();
    test_vulkan_processor_gated();
    return 0;
}
