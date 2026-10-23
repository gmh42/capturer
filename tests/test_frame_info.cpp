#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <stdexcept>
#include <vector>

#include "capturer/frame/frame_info.hpp"

// assert() is compiled out in release builds, so tests use their own check.
#define CHECK(condition)                                                                    \
    do {                                                                                    \
        if (!(condition)) {                                                                 \
            std::cerr << __FILE__ << ':' << __LINE__ << ": CHECK failed: " #condition "\n"; \
            std::exit(1);                                                                   \
        }                                                                                   \
    } while (false)

using namespace capturer::frame;

namespace {

void test_describe() {
    const auto bgra = describe(PixelFormat::BGRA8);
    CHECK(bgra.plane_count == 1 && bgra.component_count == 4 && bgra.bits_per_pixel == 32 && bgra.has_alpha && !bgra.is_yuv);

    const auto bgrx = describe(PixelFormat::BGRX8);
    CHECK(bgrx.bits_per_pixel == 32 && !bgrx.has_alpha && bgrx.component_count == 3);

    const auto nv12 = describe(PixelFormat::NV12);
    CHECK(nv12.plane_count == 2 && nv12.bits_per_pixel == 12 && nv12.is_yuv && nv12.chroma_subsample_x == 2);

    const auto i420 = describe(PixelFormat::I420);
    CHECK(i420.plane_count == 3 && i420.bits_per_pixel == 12);

    const auto p010 = describe(PixelFormat::P010);
    CHECK(p010.bits_per_component == 10 && p010.container_bits == 16 && p010.bits_per_pixel == 24);

    CHECK(describe(PixelFormat::Unknown).plane_count == 0);
}

void test_packed_planes() {
    const auto planes = default_planes(PixelFormat::RGBA8, 10, 4);
    CHECK(planes.size() == 1 && planes[0].stride == 40 && planes[0].size == 160 && planes[0].offset == 0);

    const auto padded = default_planes(PixelFormat::RGBA8, 10, 4, 64);
    CHECK(padded[0].stride == 64 && padded[0].size == 256);
}

void test_yuv_planes() {
    const auto nv12 = default_planes(PixelFormat::NV12, 8, 6);
    CHECK(nv12.size() == 2);
    CHECK(nv12[0].stride == 8 && nv12[0].size == 48);
    CHECK(nv12[1].offset == 48 && nv12[1].stride == 8 && nv12[1].size == 24);

    const auto i420 = default_planes(PixelFormat::I420, 8, 6);
    CHECK(i420.size() == 3);
    CHECK(i420[1].offset == 48 && i420[1].stride == 4 && i420[1].size == 12);
    CHECK(i420[2].offset == 60 && i420[2].size == 12);

    // Odd dimensions round the chroma planes up.
    const auto odd = default_planes(PixelFormat::NV12, 7, 5);
    CHECK(odd[1].size == odd[1].stride * 3);

    const auto p010 = default_planes(PixelFormat::P010, 8, 4);
    CHECK(p010[0].stride == 16 && p010[1].offset == 64 && p010[1].size == 32);
}

void test_cpu_frame() {
    CpuFrame frame(4, 2, PixelFormat::BGRA8, std::vector<std::uint8_t>(32, 7), 0, {ColorSpace::Srgb, ColorRange::Full, 9, 1234});
    CHECK(frame.is_cpu() && !frame.is_gpu());
    CHECK(frame.memory_kind() == MemoryKind::Cpu);
    CHECK(frame.cpu_data() != nullptr && frame.gpu_handle() == nullptr);
    CHECK(frame.width() == 4 && frame.height() == 2 && frame.stride() == 16);
    CHECK(frame.sequence() == 9 && frame.pts_ns() == 1234 && frame.color_space() == ColorSpace::Srgb);

    bool threw = false;
    try {
        CpuFrame too_small(4, 2, PixelFormat::BGRA8, std::vector<std::uint8_t>(31));
    } catch (const std::invalid_argument&) {
        threw = true;
    }
    CHECK(threw);

    threw = false;
    try {
        CpuFrame unknown(4, 2, PixelFormat::Unknown, std::vector<std::uint8_t>(64));
    } catch (const std::invalid_argument&) {
        threw = true;
    }
    CHECK(threw);

    threw = false;
    try {
        (void)frame.plane(1);
    } catch (const std::out_of_range&) {
        threw = true;
    }
    CHECK(threw);
}

void test_gpu_frame() {
    GpuHandle handle;
    handle.fds = {-1};
    GpuFrame frame(4, 2, PixelFormat::BGRX8, MemoryKind::DmaBuf, handle, default_planes(PixelFormat::BGRX8, 4, 2));
    CHECK(frame.is_gpu() && frame.memory_kind() == MemoryKind::DmaBuf);
    CHECK(frame.cpu_data() == nullptr && frame.gpu_handle() != nullptr);

    bool threw = false;
    try {
        GpuFrame bad(4, 2, PixelFormat::BGRX8, MemoryKind::Cpu, {}, default_planes(PixelFormat::BGRX8, 4, 2));
    } catch (const std::invalid_argument&) {
        threw = true;
    }
    CHECK(threw);
}

void test_fourcc_and_masks() {
    CHECK(to_drm_fourcc(PixelFormat::BGRX8) != 0 && to_drm_fourcc(PixelFormat::Unknown) == 0);
    CHECK((mask_of(MemoryKind::Cpu) & mask_of(MemoryKind::DmaBuf)) == 0);
}

}  // namespace

int main() {
    test_describe();
    test_packed_planes();
    test_yuv_planes();
    test_cpu_frame();
    test_gpu_frame();
    test_fourcc_and_masks();
    return 0;
}
