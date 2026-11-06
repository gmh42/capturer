#include "capturer/gpu/vulkan_runtime.hpp"
#include "capturer/image_processing/image_processor.hpp"
#include "capturer/image_processing/vulkan_tile_processor.hpp"
#include "capturer/sample/desktop_capture.hpp"
#include "diff_viewer_render.hpp"

#include <SDL.h>

#include <chrono>
#include <cstdlib>
#include <iostream>
#include <limits>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <utility>

namespace {

using namespace capturer;
constexpr std::size_t kTileSize = 16;

enum class Backend { Auto, Cpu, Vulkan };
enum class Source { Auto, X11, Wayland };

struct Options {
    Backend backend = Backend::Auto;
    Source source = Source::Auto;
};

Options parse_options(int argc, char** argv) {
    Options options;
    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        if (arg == "--help") {
            std::cout << "Usage: capturer_diff_viewer [--backend=auto|cpu|vulkan] [--source=auto|x11|wayland]\n"
                         "Close the window or press Escape to exit.\n";
            std::exit(0);
        } else if (arg == "--backend=auto") options.backend = Backend::Auto;
        else if (arg == "--backend=cpu") options.backend = Backend::Cpu;
        else if (arg == "--backend=vulkan") options.backend = Backend::Vulkan;
        else if (arg == "--source=auto") options.source = Source::Auto;
        else if (arg == "--source=x11") options.source = Source::X11;
        else if (arg == "--source=wayland") options.source = Source::Wayland;
        else throw std::invalid_argument("unknown option: " + arg + " (use --help)");
    }
    return options;
}

class Capture {
public:
    explicit Capture(const Options& options) {
        const bool wayland = options.source == Source::Wayland ||
            (options.source == Source::Auto && std::getenv("WAYLAND_DISPLAY") != nullptr);
        if (wayland) {
#if HAVE_PIPEWIRE
            auto config = sample::open_wayland_portal_capture();
            config.allow_dma_buf = options.backend != Backend::Cpu;
            pipewire_ = std::make_unique<sample::PipeWireDesktopCaptureSource>(std::move(config));
#else
            throw std::runtime_error("Wayland capture requires PipeWire support");
#endif
        } else {
            if (options.backend == Backend::Vulkan) {
                throw std::runtime_error("X11 capture produces CPU frames; Vulkan requires Wayland dma-buf capture");
            }
            x11_ = std::make_unique<sample::X11DesktopCaptureSource>();
        }
    }

    std::optional<sample::DesktopFrame> next() {
#if HAVE_PIPEWIRE
        if (pipewire_) {
            return pipewire_->try_capture(std::chrono::milliseconds(25));
        }
#endif
        const auto now = std::chrono::steady_clock::now();
        if (now < next_x11_frame_) {
            SDL_Delay(10);
            return std::nullopt;
        }
        next_x11_frame_ = now + std::chrono::milliseconds(100);
        return x11_->capture();
    }

private:
    std::unique_ptr<sample::X11DesktopCaptureSource> x11_;
#if HAVE_PIPEWIRE
    std::unique_ptr<sample::PipeWireDesktopCaptureSource> pipewire_;
#endif
    std::chrono::steady_clock::time_point next_x11_frame_{};
};

struct Sdl {
    Sdl() {
        if (SDL_Init(SDL_INIT_VIDEO) != 0) {
            throw std::runtime_error(std::string("SDL initialization failed: ") + SDL_GetError());
        }
    }
    ~Sdl() { SDL_Quit(); }
};

using Window = std::unique_ptr<SDL_Window, decltype(&SDL_DestroyWindow)>;
using Renderer = std::unique_ptr<SDL_Renderer, decltype(&SDL_DestroyRenderer)>;

void run(const Options& options) {
    Capture capture(options);
    auto runtime = std::make_shared<gpu::VulkanRuntime>();
    image_processing::ImageProcessorDispatcher processor;
    if (options.backend != Backend::Cpu) {
        processor.add(std::make_unique<image_processing::VulkanTileDifferenceProcessor>(runtime, kTileSize));
    }
    if (options.backend != Backend::Vulkan) {
        processor.add(std::make_unique<image_processing::CpuTileDifferenceProcessor>(kTileSize));
    }

    Sdl sdl;
    Window window(SDL_CreateWindow("Tile differencer", SDL_WINDOWPOS_UNDEFINED, SDL_WINDOWPOS_UNDEFINED,
                                   320, 200, SDL_WINDOW_SHOWN), SDL_DestroyWindow);
    if (!window) throw std::runtime_error(std::string("creating window: ") + SDL_GetError());
    Renderer renderer(SDL_CreateRenderer(window.get(), -1, SDL_RENDERER_SOFTWARE), SDL_DestroyRenderer);
    if (!renderer) throw std::runtime_error(std::string("creating renderer: ") + SDL_GetError());

    frame::FramePtr previous;
    std::optional<image_processing::TileDiff> displayed;
    std::string last_processor;
    bool running = true;
    while (running) {
        SDL_Event event;
        while (SDL_PollEvent(&event)) {
            if (event.type == SDL_QUIT || (event.type == SDL_KEYDOWN && event.key.keysym.sym == SDLK_ESCAPE)) {
                running = false;
            } else if (event.type == SDL_WINDOWEVENT && event.window.event == SDL_WINDOWEVENT_EXPOSED && displayed) {
                sample::render_tile_diff(renderer.get(), *displayed);
            }
        }
        if (!running) break;

        auto captured = capture.next();
        if (!captured) continue;
        auto current = sample::to_frame_info(std::move(*captured));
        auto result = processor.process(previous, current);
        if (result->skipped()) {
            throw std::runtime_error("no requested processor accepts captured " +
                                     std::string(frame::to_string(current->memory_kind())) + " / " +
                                     frame::to_string(current->format()) +
                                     " frames; try --backend=cpu or a compatible Wayland dma-buf source");
        }
        if (last_processor != processor.selected_name()) {
            last_processor = processor.selected_name();
            std::cout << "processor: " << last_processor << " ("
                      << frame::to_string(current->memory_kind()) << ", "
                      << frame::to_string(current->format()) << ")" << std::endl;
        }
        const auto& diff = result->tile_diff();
        const auto max_dimension = static_cast<std::size_t>(std::numeric_limits<int>::max() / sample::kTileMarkerSize);
        if (diff.width == 0 || diff.height == 0 || diff.width > max_dimension || diff.height > max_dimension) {
            throw std::runtime_error("tile grid dimensions cannot be displayed");
        }
        if (!displayed || displayed->width != diff.width || displayed->height != diff.height) {
            SDL_SetWindowSize(window.get(), static_cast<int>(diff.width * sample::kTileMarkerSize),
                              static_cast<int>(diff.height * sample::kTileMarkerSize));
        }
        sample::render_tile_diff(renderer.get(), diff);
        displayed = diff;
        previous = std::move(current);
    }
}

}  // namespace

int main(int argc, char** argv) {
    try {
        run(parse_options(argc, argv));
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "capturer_diff_viewer: " << error.what() << '\n';
        return 1;
    }
}
