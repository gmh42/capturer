#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "capturer/frame/frame_info.hpp"

#ifndef HAVE_PIPEWIRE
#define HAVE_PIPEWIRE 0
#endif

namespace capturer::sample {

enum class FrameStorage {
    CpuRgba8,
    DmaBuf,
};

struct DesktopFrame {
    std::size_t width = 0;
    std::size_t height = 0;
    std::size_t stride = 0;
    FrameStorage storage = FrameStorage::CpuRgba8;
    std::vector<std::uint8_t> pixels;
    int dma_buf_fd = -1;
    capturer::frame::PixelFormat format = capturer::frame::PixelFormat::Unknown;
    // Single-plane dma-buf layout; the modifier is meaningful only when has_drm_modifier is set.
    std::uint32_t dma_buf_offset = 0;
    std::uint64_t drm_modifier = 0;
    bool has_drm_modifier = false;
    // Producer sequence number and presentation time (ns) from the PipeWire header meta.
    bool has_header = false;
    std::uint64_t sequence = 0;
    std::int64_t pts_ns = 0;
    // Reserves the producer's buffer while a dma-buf frame is in use. Moved into the GpuFrame by to_frame_info();
    // releasing the last reference returns the buffer to the compositor. Null for frames copied to CPU memory.
    std::shared_ptr<void> buffer_lease;

    bool has_dma_buf() const { return dma_buf_fd >= 0; }
};

// Converts a captured frame into the platform-neutral description. A dma-buf frame hands its fd to the result,
// which closes it; the DesktopFrame's fd is cleared.
std::shared_ptr<const capturer::frame::IFrameInfo> to_frame_info(DesktopFrame&& frame);

class DesktopCaptureSource {
public:
    virtual ~DesktopCaptureSource() = default;
    virtual std::string name() const = 0;
    virtual DesktopFrame capture() = 0;
};

class X11DesktopCaptureSource final : public DesktopCaptureSource {
public:
    explicit X11DesktopCaptureSource(std::string display_name = {});
    ~X11DesktopCaptureSource() override;

    std::string name() const override;
    DesktopFrame capture() override;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

#if HAVE_PIPEWIRE

struct PipeWireDesktopCaptureConfig {
    std::string stream_name = "capturer-pipewire";
    int pipewire_fd = -1;
    std::uint32_t target_node_id = 0xffffffffu;
    std::shared_ptr<void> portal_connection;
    std::size_t width = 1920;
    std::size_t height = 1080;
    bool allow_dma_buf = true;
};

class PipeWireDesktopCaptureSource final : public DesktopCaptureSource {
public:
    explicit PipeWireDesktopCaptureSource(PipeWireDesktopCaptureConfig config = {});
    ~PipeWireDesktopCaptureSource() override;

    std::string name() const override;
    DesktopFrame capture() override;
    std::optional<DesktopFrame> try_capture(std::chrono::milliseconds timeout);
    // Producer frame-rate ceiling from format negotiation (0 until negotiated).
    double negotiated_max_framerate() const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

PipeWireDesktopCaptureConfig open_wayland_portal_capture();

#endif

}  // namespace capturer::sample