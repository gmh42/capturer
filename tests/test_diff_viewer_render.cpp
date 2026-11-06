#include "../src/sample/diff_viewer_render.hpp"
#include "capturer/frame/frame_info.hpp"
#include "capturer/image_processing/image_processor.hpp"

#include <SDL.h>

#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <utility>
#include <vector>

#define CHECK(condition) \
    do { \
        if (!(condition)) { \
            std::cerr << __FILE__ << ':' << __LINE__ << ": CHECK failed: " #condition "\n"; \
            std::exit(1); \
        } \
    } while (false)

int main() {
    using namespace capturer;
    using Surface = std::unique_ptr<SDL_Surface, decltype(&SDL_FreeSurface)>;
    using Renderer = std::unique_ptr<SDL_Renderer, decltype(&SDL_DestroyRenderer)>;
    Surface surface(SDL_CreateRGBSurfaceWithFormat(0, 12, 8, 32, SDL_PIXELFORMAT_RGBA32), SDL_FreeSurface);
    CHECK(surface);
    Renderer renderer(SDL_CreateSoftwareRenderer(surface.get()), SDL_DestroyRenderer);
    CHECK(renderer);

    constexpr std::size_t width = 33;
    constexpr std::size_t height = 17;
    std::vector<std::uint8_t> previous(width * height * 4, 0);
    auto current = previous;
    current[0] = 1;
    current[32 * 4] = 1;
    current[(16 * width + 16) * 4] = 1;
    auto first = std::make_shared<frame::CpuFrame>(
        width, height, frame::PixelFormat::RGBA8, std::move(previous), width * 4);
    auto second = std::make_shared<frame::CpuFrame>(
        width, height, frame::PixelFormat::RGBA8, std::move(current), width * 4);
    image_processing::CpuTileDifferenceProcessor processor(16);
    auto initial = processor.process(nullptr, first);
    sample::render_tile_diff(renderer.get(), initial->tile_diff());
    for (int y = 0; y < surface->h; ++y) {
        const auto* row = reinterpret_cast<const std::uint32_t*>(
            static_cast<const std::uint8_t*>(surface->pixels) + y * surface->pitch);
        for (int x = 0; x < surface->w; ++x) {
            CHECK(row[x] == SDL_MapRGBA(surface->format, 255, 0, 0, 255));
        }
    }

    auto result = processor.process(first, second);
    auto diff = result->tile_diff();
    CHECK(diff.width == 3 && diff.height == 2);
    sample::render_tile_diff(renderer.get(), diff);
    for (int y = 0; y < surface->h; ++y) {
        for (int x = 0; x < surface->w; ++x) {
            const auto* row = reinterpret_cast<const std::uint32_t*>(
                static_cast<const std::uint8_t*>(surface->pixels) + y * surface->pitch);
            std::uint8_t r, g, b, a;
            SDL_GetRGBA(row[x], surface->format, &r, &g, &b, &a);
            const bool changed = (y < 4 && (x < 4 || x >= 8)) || (y >= 4 && x >= 4 && x < 8);
            CHECK(r == (changed ? 255 : 0));
            CHECK(g == 0 && b == 0 && a == 255);
        }
    }

    diff.tiles.assign(6, {false, 0});
    sample::render_tile_diff(renderer.get(), diff);
    for (int y = 0; y < surface->h; ++y) {
        const auto* row = reinterpret_cast<const std::uint32_t*>(
            static_cast<const std::uint8_t*>(surface->pixels) + y * surface->pitch);
        for (int x = 0; x < surface->w; ++x) {
            CHECK(row[x] == SDL_MapRGBA(surface->format, 0, 0, 0, 255));
        }
    }

    diff.tiles.pop_back();
    bool rejected = false;
    try {
        sample::render_tile_diff(renderer.get(), diff);
    } catch (const std::invalid_argument&) {
        rejected = true;
    }
    CHECK(rejected);
}
