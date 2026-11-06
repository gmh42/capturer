#pragma once

#include "capturer/image_processing/tile_difference.hpp"

struct SDL_Renderer;

namespace capturer::sample {

constexpr int kTileMarkerSize = 4;

// Draw a compact tile grid, independent of how the diff was produced.
void render_tile_diff(SDL_Renderer* renderer, const image_processing::TileDiff& diff);

}  // namespace capturer::sample
