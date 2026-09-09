#include "capturer/image_processing/tile_difference.hpp"

#include <algorithm>
#include <stdexcept>

namespace capturer::image_processing {

TileDiff compute_tile_difference(
    const std::vector<std::uint8_t>& previous,
    const std::vector<std::uint8_t>& current,
    std::size_t width,
    std::size_t height,
    std::size_t tile_size) {
    if (width == 0 || height == 0 || tile_size == 0) {
        throw std::invalid_argument("dimensions and tile size must be positive");
    }
    if (previous.size() < width * height || current.size() < width * height) {
        throw std::invalid_argument("frame buffers must cover the given dimensions");
    }

    const std::size_t tiles_x = (width + tile_size - 1) / tile_size;
    const std::size_t tiles_y = (height + tile_size - 1) / tile_size;

    TileDiff diff{};
    diff.width = tiles_x;
    diff.height = tiles_y;
    diff.tiles.reserve(tiles_x * tiles_y);

    for (std::size_t ty = 0; ty < tiles_y; ++ty) {
        for (std::size_t tx = 0; tx < tiles_x; ++tx) {
            std::uint32_t sum_prev = 0;
            std::uint32_t sum_curr = 0;
            const std::size_t x0 = tx * tile_size;
            const std::size_t y0 = ty * tile_size;
            for (std::size_t y = y0; y < std::min(height, y0 + tile_size); ++y) {
                for (std::size_t x = x0; x < std::min(width, x0 + tile_size); ++x) {
                    const std::size_t idx = y * width + x;
                    sum_prev += previous[idx];
                    sum_curr += current[idx];
                }
            }
            const bool changed = sum_prev != sum_curr;
            diff.tiles.push_back({changed, static_cast<std::uint8_t>(changed ? 1u : 0u)});
        }
    }

    return diff;
}

}  // namespace capturer::image_processing
