#include "capturer/frame/frame_info.hpp"

#include <algorithm>
#include <stdexcept>
#include <unistd.h>

#include <drm_fourcc.h>

namespace capturer::frame {

PixelFormatInfo describe(PixelFormat format) {
    PixelFormatInfo info;
    switch (format) {
    case PixelFormat::RGBA8:
    case PixelFormat::BGRA8:
    case PixelFormat::ARGB8:
        info = {1, 4, 8, 8, 32, 1, 1, true, false};
        break;
    case PixelFormat::RGBX8:
    case PixelFormat::BGRX8:
        info = {1, 3, 8, 8, 32, 1, 1, false, false};
        break;
    case PixelFormat::NV12:
    case PixelFormat::I420:
        info = {format == PixelFormat::NV12 ? 2u : 3u, 3, 8, 8, 12, 2, 2, false, true};
        break;
    case PixelFormat::P010:
        info = {2, 3, 10, 16, 24, 2, 2, false, true};
        break;
    case PixelFormat::Unknown:
        break;
    }
    return info;
}

const char* to_string(PixelFormat format) {
    switch (format) {
    case PixelFormat::RGBA8: return "RGBA8";
    case PixelFormat::BGRA8: return "BGRA8";
    case PixelFormat::RGBX8: return "RGBX8";
    case PixelFormat::BGRX8: return "BGRX8";
    case PixelFormat::ARGB8: return "ARGB8";
    case PixelFormat::NV12: return "NV12";
    case PixelFormat::I420: return "I420";
    case PixelFormat::P010: return "P010";
    case PixelFormat::Unknown: break;
    }
    return "Unknown";
}

const char* to_string(MemoryKind kind) {
    switch (kind) {
    case MemoryKind::Cpu: return "cpu";
    case MemoryKind::DmaBuf: return "dma-buf";
    case MemoryKind::VulkanImage: return "vulkan-image";
    case MemoryKind::D3D11Texture: return "d3d11-texture";
    case MemoryKind::D3D12Resource: return "d3d12-resource";
    }
    return "unknown";
}

std::uint32_t to_drm_fourcc(PixelFormat format) {
    // DRM fourccs name the little-endian packed word, so the byte order is reversed.
    switch (format) {
    case PixelFormat::RGBA8: return DRM_FORMAT_ABGR8888;
    case PixelFormat::BGRA8: return DRM_FORMAT_ARGB8888;
    case PixelFormat::RGBX8: return DRM_FORMAT_XBGR8888;
    case PixelFormat::BGRX8: return DRM_FORMAT_XRGB8888;
    case PixelFormat::ARGB8: return DRM_FORMAT_BGRA8888;
    case PixelFormat::NV12: return DRM_FORMAT_NV12;
    case PixelFormat::I420: return DRM_FORMAT_YUV420;
    case PixelFormat::P010: return DRM_FORMAT_P010;
    case PixelFormat::Unknown: break;
    }
    return 0;
}

std::vector<PlaneLayout> default_planes(PixelFormat format, std::size_t width, std::size_t height, std::size_t stride0) {
    const PixelFormatInfo info = describe(format);
    std::vector<PlaneLayout> planes;
    if (info.plane_count == 0 || width == 0 || height == 0) {
        return planes;
    }

    const std::size_t chroma_height = (height + info.chroma_subsample_y - 1) / info.chroma_subsample_y;
    const std::size_t chroma_width = (width + info.chroma_subsample_x - 1) / info.chroma_subsample_x;

    if (!info.is_yuv) {
        const std::size_t stride = stride0 != 0 ? stride0 : width * (info.bits_per_pixel / 8);
        planes.push_back({0, stride, stride * height});
        return planes;
    }

    const std::size_t sample_bytes = info.container_bits / 8;
    const std::size_t luma_stride = stride0 != 0 ? stride0 : width * sample_bytes;
    planes.push_back({0, luma_stride, luma_stride * height});
    std::size_t offset = luma_stride * height;
    if (format == PixelFormat::I420) {
        const std::size_t chroma_stride = (luma_stride + 1) / 2;
        for (int plane = 0; plane < 2; ++plane) {
            planes.push_back({offset, chroma_stride, chroma_stride * chroma_height});
            offset += chroma_stride * chroma_height;
        }
    } else {
        // Interleaved CbCr has one pair per chroma sample, so a row is as wide in bytes as the luma row.
        const std::size_t chroma_stride = std::max(luma_stride, chroma_width * 2 * sample_bytes);
        planes.push_back({offset, chroma_stride, chroma_stride * chroma_height});
    }
    return planes;
}

FrameBase::FrameBase(std::size_t width, std::size_t height, PixelFormat format, std::vector<PlaneLayout> planes, FrameMetadata metadata)
    : width_(width), height_(height), format_(format), planes_(std::move(planes)), metadata_(metadata) {}

const PlaneLayout& FrameBase::plane(std::size_t index) const {
    if (index >= planes_.size()) {
        throw std::out_of_range("plane index out of range");
    }
    return planes_[index];
}

namespace {

std::size_t required_bytes(const std::vector<PlaneLayout>& planes) {
    std::size_t required = 0;
    for (const auto& plane : planes) {
        required = std::max(required, plane.offset + plane.size);
    }
    return required;
}

}  // namespace

CpuFrame::CpuFrame(
    std::size_t width,
    std::size_t height,
    PixelFormat format,
    std::vector<std::uint8_t> data,
    std::size_t stride0,
    FrameMetadata metadata)
    : CpuFrame(width, height, format, std::move(data), default_planes(format, width, height, stride0), metadata) {}

CpuFrame::CpuFrame(
    std::size_t width,
    std::size_t height,
    PixelFormat format,
    std::vector<std::uint8_t> data,
    std::vector<PlaneLayout> planes,
    FrameMetadata metadata)
    : FrameBase(width, height, format, std::move(planes), metadata), data_(std::move(data)) {
    if (plane_count() == 0) {
        throw std::invalid_argument("CpuFrame needs a known pixel format and non-zero dimensions");
    }
    std::vector<PlaneLayout> layout;
    for (std::size_t index = 0; index < plane_count(); ++index) {
        layout.push_back(plane(index));
    }
    if (data_.size() < required_bytes(layout)) {
        throw std::invalid_argument("CpuFrame data does not cover every plane");
    }
}

GpuFrame::GpuFrame(
    std::size_t width,
    std::size_t height,
    PixelFormat format,
    MemoryKind kind,
    GpuHandle handle,
    std::vector<PlaneLayout> planes,
    FrameMetadata metadata)
    : FrameBase(width, height, format, std::move(planes), metadata), kind_(kind), handle_(std::move(handle)) {
    if (kind_ == MemoryKind::Cpu) {
        throw std::invalid_argument("GpuFrame cannot use MemoryKind::Cpu");
    }
}

GpuFrame::~GpuFrame() {
    for (const int fd : handle_.fds) {
        if (fd >= 0) {
            close(fd);
        }
    }
    if (handle_.acquire_fence_fd >= 0) {
        close(handle_.acquire_fence_fd);
    }
}

}  // namespace capturer::frame
