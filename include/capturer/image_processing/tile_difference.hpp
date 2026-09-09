#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

namespace capturer::image_processing {

struct TileDiff {
    std::size_t width;
    std::size_t height;
    struct Tile {
        bool changed;
        std::uint8_t value;
    };
    std::vector<Tile> tiles;
};

TileDiff compute_tile_difference(
    const std::vector<std::uint8_t>& previous,
    const std::vector<std::uint8_t>& current,
    std::size_t width,
    std::size_t height,
    std::size_t tile_size);

}  // namespace capturer::image_processing
