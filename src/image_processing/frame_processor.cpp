#include "capturer/image_processing/frame_processor.hpp"

namespace capturer::image_processing {

TileDiff TileDifferenceProcessor::process(
    const std::vector<std::uint8_t>& previous,
    const std::vector<std::uint8_t>& current,
    std::size_t width,
    std::size_t height) {
    const std::size_t tiles_x = (width + tile_size_ - 1) / tile_size_;
    const std::size_t tiles_y = (height + tile_size_ - 1) / tile_size_;
    TileDiff diff{};
    diff.width = tiles_x;
    diff.height = tiles_y;
    diff.tiles.reserve(tiles_x * tiles_y);

    const bool has_previous = previous.size() == width * height;

    for (std::size_t ty = 0; ty < tiles_y; ++ty) {
        for (std::size_t tx = 0; tx < tiles_x; ++tx) {
            std::uint32_t sum_prev = 0;
            std::uint32_t sum_curr = 0;
            const std::size_t x0 = tx * tile_size_;
            const std::size_t y0 = ty * tile_size_;
            for (std::size_t y = y0; y < std::min(height, y0 + tile_size_); ++y) {
                for (std::size_t x = x0; x < std::min(width, x0 + tile_size_); ++x) {
                    const std::size_t idx = y * width + x;
                    if (has_previous) {
                        sum_prev += previous[idx];
                    }
                    sum_curr += current[idx];
                }
            }
            const bool changed = !has_previous || sum_prev != sum_curr;
            diff.tiles.push_back({changed, static_cast<std::uint8_t>(changed ? 1u : 0u)});
        }
    }
    return diff;
}

}  // namespace capturer::image_processing
