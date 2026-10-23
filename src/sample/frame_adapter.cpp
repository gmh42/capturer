#include "capturer/sample/desktop_capture.hpp"

#include <unistd.h>

namespace capturer::sample {

std::shared_ptr<const capturer::frame::IFrameInfo> to_frame_info(DesktopFrame&& frame) {
    using capturer::frame::CpuFrame;
    using capturer::frame::GpuFrame;
    using capturer::frame::GpuHandle;
    using capturer::frame::MemoryKind;

    // Capture sources that cannot report a format deliver 4-byte RGBA pixels.
    const auto format = frame.format == capturer::frame::PixelFormat::Unknown ? capturer::frame::PixelFormat::RGBA8 : frame.format;

    if (frame.has_dma_buf()) {
        GpuHandle handle;
        handle.fds.push_back(frame.dma_buf_fd);
        handle.drm_fourcc = capturer::frame::to_drm_fourcc(format);
        handle.drm_modifier = frame.drm_modifier;
        handle.has_drm_modifier = frame.has_drm_modifier;
        frame.dma_buf_fd = -1;

        auto planes = capturer::frame::default_planes(format, frame.width, frame.height, frame.stride);
        if (!planes.empty()) {
            planes[0].offset = frame.dma_buf_offset;
        }
        return std::make_shared<GpuFrame>(frame.width, frame.height, format, MemoryKind::DmaBuf, std::move(handle), std::move(planes));
    }

    return std::make_shared<CpuFrame>(frame.width, frame.height, format, std::move(frame.pixels), frame.stride);
}

}  // namespace capturer::sample
