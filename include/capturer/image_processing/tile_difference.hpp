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

// Compares two images given as byte rows. A tile is `tile_size` pixels square; a pixel is `bytes_per_pixel` bytes.
// A tile is changed when any byte in it differs. Strides are in bytes.
TileDiff compute_tile_difference(
    const std::uint8_t* previous,
    std::size_t previous_stride,
    const std::uint8_t* current,
    std::size_t current_stride,
    std::size_t width,
    std::size_t height,
    std::size_t bytes_per_pixel,
    std::size_t tile_size);

// Convenience overload for tightly packed single-byte-per-pixel buffers.
TileDiff compute_tile_difference(
    const std::vector<std::uint8_t>& previous,
    const std::vector<std::uint8_t>& current,
    std::size_t width,
    std::size_t height,
    std::size_t tile_size);

// Result for the first frame, when there is nothing to compare against: every tile is changed.
TileDiff all_tiles_changed(std::size_t width, std::size_t height, std::size_t tile_size);

}  // namespace capturer::image_processing
