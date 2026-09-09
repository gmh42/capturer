#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

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

    bool has_dma_buf() const { return dma_buf_fd >= 0; }
};

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
    std::uint32_t framerate_numerator = 30;
    std::uint32_t framerate_denominator = 1;
};

class PipeWireDesktopCaptureSource final : public DesktopCaptureSource {
public:
    explicit PipeWireDesktopCaptureSource(PipeWireDesktopCaptureConfig config = {});
    ~PipeWireDesktopCaptureSource() override;

    std::string name() const override;
    DesktopFrame capture() override;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

#endif

}  // namespace capturer::sample