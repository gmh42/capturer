// End-to-end check of the Vulkan tile processor on real dma-bufs: buffers are allocated with GBM, filled through
// mmap, imported into Vulkan and compared against the CPU processor. Exits 77 (skipped) when the machine has no
// usable GPU/GBM/dma-buf import support.
#include <fcntl.h>
#include <linux/dma-buf.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <unistd.h>

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdlib>
#include <functional>
#include <iostream>
#include <memory>
#include <string>
#include <vector>

#include <gbm.h>

#include "capturer/frame/frame_info.hpp"
#include "capturer/gpu/vulkan_runtime.hpp"
#include "capturer/image_processing/image_processor.hpp"
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
using Pixel = std::array<std::uint8_t, 4>;

namespace {

constexpr int kSkip = 77;
constexpr std::size_t kWidth = 40;
constexpr std::size_t kHeight = 36;
constexpr std::size_t kTile = 16;

struct Made {
    std::unique_ptr<GpuFrame> gpu;
    std::unique_ptr<CpuFrame> cpu;
};

Made make_frame(gbm_device* device, PixelFormat format, std::uint64_t sequence, const std::function<Pixel(std::size_t, std::size_t)>& pixel) {
    const std::uint32_t fourcc = to_drm_fourcc(format);
    gbm_bo* bo = gbm_bo_create(device, kWidth, kHeight, fourcc, GBM_BO_USE_LINEAR | GBM_BO_USE_RENDERING);
    if (!bo) {
        std::cerr << "SKIP: gbm_bo_create failed\n";
        std::exit(kSkip);
    }
    const std::size_t stride = gbm_bo_get_stride(bo);
    const int fd = gbm_bo_get_fd(bo);
    CHECK(fd >= 0);

    const std::size_t bytes = stride * kHeight;
    std::vector<std::uint8_t> shadow(bytes, 0);
    for (std::size_t y = 0; y < kHeight; ++y) {
        for (std::size_t x = 0; x < kWidth; ++x) {
            const Pixel p = pixel(x, y);
            for (std::size_t c = 0; c < 4; ++c) {
                shadow[y * stride + x * 4 + c] = p[c];
            }
        }
    }

    void* map = mmap(nullptr, bytes, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (map == MAP_FAILED) {
        std::cerr << "SKIP: cannot mmap dma-buf\n";
        std::exit(kSkip);
    }
    dma_buf_sync sync{DMA_BUF_SYNC_START | DMA_BUF_SYNC_WRITE};
    ioctl(fd, DMA_BUF_IOCTL_SYNC, &sync);
    std::copy(shadow.begin(), shadow.end(), static_cast<std::uint8_t*>(map));
    sync.flags = DMA_BUF_SYNC_END | DMA_BUF_SYNC_WRITE;
    ioctl(fd, DMA_BUF_IOCTL_SYNC, &sync);
    munmap(map, bytes);
    gbm_bo_destroy(bo);  // the fd keeps the buffer alive

    GpuHandle handle;
    handle.fds.push_back(fd);
    handle.drm_fourcc = fourcc;
    handle.drm_modifier = 0;  // DRM_FORMAT_MOD_LINEAR
    handle.has_drm_modifier = true;
    FrameMetadata metadata;
    metadata.sequence = sequence;

    Made made;
    made.gpu = std::make_unique<GpuFrame>(kWidth, kHeight, format, MemoryKind::DmaBuf, std::move(handle), default_planes(format, kWidth, kHeight, stride), metadata);
    made.cpu = std::make_unique<CpuFrame>(kWidth, kHeight, format, std::move(shadow), stride, metadata);
    return made;
}

Pixel base(std::size_t x, std::size_t y) {
    return {static_cast<std::uint8_t>(x * 5), static_cast<std::uint8_t>(y * 7), static_cast<std::uint8_t>(x + y), 255};
}

std::vector<bool> flags(IProcessResult& result) {
    std::vector<bool> out;
    for (const auto& tile : result.tile_diff().tiles) {
        out.push_back(tile.changed);
    }
    return out;
}

void run_format(gbm_device* device, const std::shared_ptr<capturer::gpu::VulkanRuntime>& runtime, PixelFormat format) {
    VulkanTileDifferenceProcessor gpu(runtime, kTile);
    CpuTileDifferenceProcessor cpu(kTile);

    const auto with_change = [&](std::size_t cx, std::size_t cy, std::size_t channel) {
        return [=](std::size_t x, std::size_t y) {
            Pixel p = base(x, y);
            if (x == cx && y == cy) p[channel] ^= 0x40;
            return p;
        };
    };

    Made a = make_frame(device, format, 1, base);
    if (!gpu.can_process(*a.gpu)) {
        std::cerr << "SKIP: device cannot import this dma-buf\n";
        std::exit(kSkip);
    }

    // First frame: everything changed.
    auto r0 = gpu.process(nullptr, *a.gpu);
    CHECK(!r0->skipped());
    CHECK(r0->location() == ResultLocation::Gpu);
    CHECK(r0->tile_diff().width == 3 && r0->tile_diff().height == 3);
    for (bool changed : flags(*r0)) CHECK(changed);

    // Identical content in a different buffer: nothing changed.
    Made b = make_frame(device, format, 2, base);
    auto r1 = gpu.process(a.gpu.get(), *b.gpu);
    for (bool changed : flags(*r1)) CHECK(!changed);

    // One pixel in tile (1,1) changes; compare with the CPU processor.
    Made c = make_frame(device, format, 3, with_change(20, 20, 1));
    auto r2 = gpu.process(b.gpu.get(), *c.gpu);
    auto expected = cpu.process(b.cpu.get(), *c.cpu);
    CHECK(flags(*r2) == flags(*expected));
    CHECK(flags(*r2)[1 * 3 + 1]);

    // The last pixel lies in a partial edge tile.
    Made d = make_frame(device, format, 4, with_change(kWidth - 1, kHeight - 1, 2));
    auto r3 = gpu.process(c.gpu.get(), *d.gpu);
    auto expected3 = cpu.process(c.cpu.get(), *d.cpu);
    CHECK(flags(*r3) == flags(*expected3));
    CHECK(flags(*r3)[2 * 3 + 2]);

    // Against d, e differs in the corner tile (reverted) and, in the alpha byte only, in tile (0,0).
    // The padding byte of X formats is ignored; alpha of alpha formats is not.
    Made e = make_frame(device, format, 5, with_change(5, 5, 3));
    auto r4 = gpu.process(d.gpu.get(), *e.gpu);
    const std::vector<bool> result4 = flags(*r4);
    CHECK(result4[2 * 3 + 2]);
    CHECK(result4[0] == describe(format).has_alpha);

    // A previous frame that is not the last processed one restarts the comparison.
    auto r5 = gpu.process(a.gpu.get(), *e.gpu);
    for (bool changed : flags(*r5)) CHECK(changed);
    auto r6 = gpu.process(nullptr, *e.gpu);
    for (bool changed : flags(*r6)) CHECK(changed);
}

}  // namespace

int main() {
    int fd = -1;
    for (int minor = 128; minor < 192 && fd < 0; ++minor) {
        const std::string path = "/dev/dri/renderD" + std::to_string(minor);
        fd = open(path.c_str(), O_RDWR | O_CLOEXEC);
    }
    if (fd < 0) {
        std::cerr << "SKIP: no DRM render node\n";
        return kSkip;
    }
    gbm_device* device = gbm_create_device(fd);
    if (!device) {
        std::cerr << "SKIP: cannot create GBM device\n";
        return kSkip;
    }
    auto runtime = std::make_shared<capturer::gpu::VulkanRuntime>();
    if (!runtime->initialize() || !runtime->supports_dma_buf_import()) {
        std::cerr << "SKIP: Vulkan dma-buf import unavailable\n";
        return kSkip;
    }

    run_format(device, runtime, PixelFormat::BGRX8);
    run_format(device, runtime, PixelFormat::BGRA8);

    gbm_device_destroy(device);
    close(fd);
    std::cout << "vulkan dma-buf tests passed on " << runtime->describe() << '\n';
    return 0;
}
