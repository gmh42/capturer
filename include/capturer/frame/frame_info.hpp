#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <vector>

namespace capturer::frame {

// Channel order is the byte order in memory (BGRA8 = B, G, R, A), which matches SPA/PipeWire naming.
enum class PixelFormat {
    Unknown,
    RGBA8,
    BGRA8,
    RGBX8,
    BGRX8,
    ARGB8,
    NV12,  // 8-bit 4:2:0, Y plane + interleaved CbCr plane
    I420,  // 8-bit 4:2:0, Y, Cb, Cr planes
    P010,  // 10-bit 4:2:0 stored in 16-bit containers, Y plane + interleaved CbCr plane
};

struct PixelFormatInfo {
    std::uint32_t plane_count = 0;
    std::uint32_t component_count = 0;
    std::uint32_t bits_per_component = 0;  // significant bits per component
    std::uint32_t container_bits = 0;      // storage bits per component
    std::uint32_t bits_per_pixel = 0;      // average storage bits per pixel across all planes
    std::uint32_t chroma_subsample_x = 1;
    std::uint32_t chroma_subsample_y = 1;
    bool has_alpha = false;
    bool is_yuv = false;
};

// Returns an all-zero description for PixelFormat::Unknown.
PixelFormatInfo describe(PixelFormat format);
const char* to_string(PixelFormat format);

// DRM fourcc for the format, or 0 when there is no equivalent.
std::uint32_t to_drm_fourcc(PixelFormat format);

enum class ColorSpace { Unspecified, Srgb, Bt601, Bt709, Bt2020 };
enum class ColorRange { Unspecified, Full, Limited };

enum class MemoryKind {
    Cpu,
    DmaBuf,
    VulkanImage,
    D3D11Texture,
    D3D12Resource,
};

const char* to_string(MemoryKind kind);

using MemoryKindMask = std::uint32_t;
constexpr MemoryKindMask mask_of(MemoryKind kind) {
    return MemoryKindMask{1} << static_cast<std::uint32_t>(kind);
}

struct PlaneLayout {
    std::size_t offset = 0;  // from the start of the plane's buffer
    std::size_t stride = 0;  // bytes per row
    std::size_t size = 0;    // bytes in the plane
};

// Describes GPU-resident memory without exposing Vulkan or D3D types.
struct GpuHandle {
    std::vector<int> fds;                // dma-buf: one fd per memory object, -1 when absent
    std::uint32_t drm_fourcc = 0;
    std::uint64_t drm_modifier = 0;
    bool has_drm_modifier = false;
    std::uint64_t native = 0;            // Win32 shared handle or API-specific handle
    int acquire_fence_fd = -1;           // sync_file the consumer must wait on before reading, if any
    // Keeps the producer's buffer reserved (e.g. a PipeWire buffer withheld from the compositor) until the frame
    // is destroyed. Without it the producer may overwrite the memory while a consumer still reads it.
    std::shared_ptr<void> lifetime;
};

// Computes the plane layout for a tightly packed frame. stride0 of 0 means width * bytes per pixel.
std::vector<PlaneLayout> default_planes(PixelFormat format, std::size_t width, std::size_t height, std::size_t stride0 = 0);

class IFrameInfo {
public:
    virtual ~IFrameInfo() = default;

    virtual std::size_t width() const = 0;
    virtual std::size_t height() const = 0;
    virtual PixelFormat format() const = 0;
    virtual MemoryKind memory_kind() const = 0;
    virtual std::size_t plane_count() const = 0;
    virtual const PlaneLayout& plane(std::size_t index) const = 0;

    // Valid only when memory_kind() == Cpu; nullptr otherwise.
    virtual const std::uint8_t* cpu_data() const = 0;
    // Valid only for GPU memory kinds; nullptr for CPU frames.
    virtual const GpuHandle* gpu_handle() const = 0;

    virtual ColorSpace color_space() const = 0;
    virtual ColorRange color_range() const = 0;
    virtual std::uint64_t sequence() const = 0;
    virtual std::int64_t pts_ns() const = 0;

    bool is_cpu() const { return memory_kind() == MemoryKind::Cpu; }
    bool is_gpu() const { return !is_cpu(); }
    std::size_t stride() const { return plane_count() > 0 ? plane(0).stride : 0; }
};

// Frames are shared so that anything still reading one (a GPU job, a pending result) keeps it, and the producer
// buffer behind it, alive. Hold a FramePtr for as long as the pixels are needed.
using FramePtr = std::shared_ptr<const IFrameInfo>;

struct FrameMetadata {
    ColorSpace color_space = ColorSpace::Unspecified;
    ColorRange color_range = ColorRange::Unspecified;
    std::uint64_t sequence = 0;
    std::int64_t pts_ns = 0;
};

class FrameBase : public IFrameInfo {
public:
    std::size_t width() const override { return width_; }
    std::size_t height() const override { return height_; }
    PixelFormat format() const override { return format_; }
    std::size_t plane_count() const override { return planes_.size(); }
    const PlaneLayout& plane(std::size_t index) const override;
    ColorSpace color_space() const override { return metadata_.color_space; }
    ColorRange color_range() const override { return metadata_.color_range; }
    std::uint64_t sequence() const override { return metadata_.sequence; }
    std::int64_t pts_ns() const override { return metadata_.pts_ns; }

protected:
    FrameBase(std::size_t width, std::size_t height, PixelFormat format, std::vector<PlaneLayout> planes, FrameMetadata metadata);

private:
    std::size_t width_;
    std::size_t height_;
    PixelFormat format_;
    std::vector<PlaneLayout> planes_;
    FrameMetadata metadata_;
};

// Frame whose pixels live in CPU memory owned by the frame. All planes share one buffer.
class CpuFrame final : public FrameBase {
public:
    // Throws std::invalid_argument if the data does not cover every plane.
    CpuFrame(
        std::size_t width,
        std::size_t height,
        PixelFormat format,
        std::vector<std::uint8_t> data,
        std::size_t stride0 = 0,
        FrameMetadata metadata = {});
    CpuFrame(
        std::size_t width,
        std::size_t height,
        PixelFormat format,
        std::vector<std::uint8_t> data,
        std::vector<PlaneLayout> planes,
        FrameMetadata metadata = {});

    MemoryKind memory_kind() const override { return MemoryKind::Cpu; }
    const std::uint8_t* cpu_data() const override { return data_.data(); }
    const GpuHandle* gpu_handle() const override { return nullptr; }

private:
    std::vector<std::uint8_t> data_;
};

// Frame backed by GPU-accessible memory. Owns the dma-buf and fence fds in the handle and closes them on destruction.
class GpuFrame final : public FrameBase {
public:
    GpuFrame(
        std::size_t width,
        std::size_t height,
        PixelFormat format,
        MemoryKind kind,
        GpuHandle handle,
        std::vector<PlaneLayout> planes,
        FrameMetadata metadata = {});
    ~GpuFrame() override;
    GpuFrame(const GpuFrame&) = delete;
    GpuFrame& operator=(const GpuFrame&) = delete;

    MemoryKind memory_kind() const override { return kind_; }
    const std::uint8_t* cpu_data() const override { return nullptr; }
    const GpuHandle* gpu_handle() const override { return &handle_; }

private:
    MemoryKind kind_;
    GpuHandle handle_;
};

}  // namespace capturer::frame
