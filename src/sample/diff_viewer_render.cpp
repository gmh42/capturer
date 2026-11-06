#include "diff_viewer_render.hpp"

#include <SDL.h>

#include <limits>
#include <stdexcept>
#include <string>

namespace capturer::sample {
namespace {

void check(int result, const char* operation) {
    if (result != 0) {
        throw std::runtime_error(std::string(operation) + ": " + SDL_GetError());
    }
}

}  // namespace

void render_tile_diff(SDL_Renderer* renderer, const image_processing::TileDiff& diff) {
    const auto max_dimension = static_cast<std::size_t>(std::numeric_limits<int>::max() / kTileMarkerSize);
    if (!renderer || diff.width > max_dimension || diff.height > max_dimension ||
        (diff.height != 0 && diff.width > std::numeric_limits<std::size_t>::max() / diff.height) ||
        diff.tiles.size() != diff.width * diff.height) {
        throw std::invalid_argument("invalid tile grid for rendering");
    }

    check(SDL_SetRenderDrawColor(renderer, 0, 0, 0, 255), "setting background color");
    check(SDL_RenderClear(renderer), "clearing tile grid");
    check(SDL_SetRenderDrawColor(renderer, 255, 0, 0, 255), "setting marker color");
    for (std::size_t y = 0; y < diff.height; ++y) {
        for (std::size_t x = 0; x < diff.width; ++x) {
            if (diff.tiles[y * diff.width + x].changed) {
                const SDL_Rect rect{static_cast<int>(x * kTileMarkerSize),
                                    static_cast<int>(y * kTileMarkerSize),
                                    kTileMarkerSize, kTileMarkerSize};
                check(SDL_RenderFillRect(renderer, &rect), "drawing tile marker");
            }
        }
    }
    SDL_RenderPresent(renderer);
}

}  // namespace capturer::sample
