#include "capturer/sample/desktop_capture.hpp"

#include <stdexcept>
#include <utility>

#include <X11/Xlib.h>
#include <X11/Xutil.h>

namespace capturer::sample {

namespace {

std::uint8_t scale_component(unsigned long value, unsigned long mask) {
    if (mask == 0) {
        return 0;
    }

    unsigned long shift = 0;
    while (((mask >> shift) & 1UL) == 0UL) {
        ++shift;
    }

    unsigned long width = 0;
    while (((mask >> (shift + width)) & 1UL) != 0UL) {
        ++width;
    }

    const unsigned long component = (value & mask) >> shift;
    if (width >= 8) {
        return static_cast<std::uint8_t>(component >> (width - 8));
    }

    const unsigned long max_value = (1UL << width) - 1UL;
    if (max_value == 0) {
        return 0;
    }

    return static_cast<std::uint8_t>((component * 255UL) / max_value);
}

}  // namespace

struct X11DesktopCaptureSource::Impl {
    Display* display = nullptr;
    int screen = 0;
    Window root = 0;
    std::string display_name;

    explicit Impl(std::string name)
        : display(XOpenDisplay(name.empty() ? nullptr : name.c_str())), display_name(std::move(name)) {
        if (!display) {
            throw std::runtime_error("failed to open X11 display");
        }
        screen = DefaultScreen(display);
        root = RootWindow(display, screen);
    }

    ~Impl() {
        if (display) {
            XCloseDisplay(display);
        }
    }
};

X11DesktopCaptureSource::X11DesktopCaptureSource(std::string display_name)
    : impl_(std::make_unique<Impl>(std::move(display_name))) {}

X11DesktopCaptureSource::~X11DesktopCaptureSource() = default;

std::string X11DesktopCaptureSource::name() const {
    return impl_->display_name.empty() ? "x11-root-window" : impl_->display_name;
}

DesktopFrame X11DesktopCaptureSource::capture() {
    const auto* impl = impl_.get();
    const int width = DisplayWidth(impl->display, impl->screen);
    const int height = DisplayHeight(impl->display, impl->screen);
    XImage* image = XGetImage(
        impl->display,
        impl->root,
        0,
        0,
        static_cast<unsigned int>(width),
        static_cast<unsigned int>(height),
        AllPlanes,
        ZPixmap);

    if (!image) {
        throw std::runtime_error("XGetImage failed while capturing the desktop");
    }

    DesktopFrame frame{};
    frame.width = static_cast<std::size_t>(width);
    frame.height = static_cast<std::size_t>(height);
    frame.stride = frame.width * 4;
    frame.storage = FrameStorage::CpuRgba8;
    frame.pixels.resize(frame.stride * frame.height);

    for (std::size_t y = 0; y < frame.height; ++y) {
        for (std::size_t x = 0; x < frame.width; ++x) {
            const unsigned long pixel = XGetPixel(image, static_cast<int>(x), static_cast<int>(y));
            const std::size_t offset = y * frame.stride + x * 4;
            frame.pixels[offset + 0] = scale_component(pixel, image->red_mask);
            frame.pixels[offset + 1] = scale_component(pixel, image->green_mask);
            frame.pixels[offset + 2] = scale_component(pixel, image->blue_mask);
            frame.pixels[offset + 3] = 255;
        }
    }

    XDestroyImage(image);
    return frame;
}

}  // namespace capturer::sample