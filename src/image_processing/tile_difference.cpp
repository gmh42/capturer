#include "capturer/image_processing/tile_difference.hpp"

#include <algorithm>
#include <cstring>
#include <stdexcept>

namespace capturer::image_processing {

namespace {

TileDiff make_diff(std::size_t width, std::size_t height, std::size_t tile_size) {
    TileDiff diff{};
    diff.width = (width + tile_size - 1) / tile_size;
    diff.height = (height + tile_size - 1) / tile_size;
    diff.tiles.reserve(diff.width * diff.height);
    return diff;
}

void validate(std::size_t width, std::size_t height, std::size_t bytes_per_pixel, std::size_t tile_size) {
    if (width == 0 || height == 0 || tile_size == 0 || bytes_per_pixel == 0) {
        throw std::invalid_argument("dimensions, pixel size and tile size must be positive");
    }
}

}  // namespace

TileDiff compute_tile_difference(
    const std::uint8_t* previous,
    std::size_t previous_stride,
    const std::uint8_t* current,
    std::size_t current_stride,
    std::size_t width,
    std::size_t height,
    std::size_t bytes_per_pixel,
    std::size_t tile_size) {
    validate(width, height, bytes_per_pixel, tile_size);
    if (!previous || !current) {
        throw std::invalid_argument("frame buffers must not be null");
    }
    const std::size_t row_bytes = width * bytes_per_pixel;
    if (previous_stride < row_bytes || current_stride < row_bytes) {
        throw std::invalid_argument("stride is smaller than a row of pixels");
    }

    TileDiff diff = make_diff(width, height, tile_size);
    for (std::size_t ty = 0; ty < diff.height; ++ty) {
        const std::size_t y0 = ty * tile_size;
        const std::size_t y1 = std::min(height, y0 + tile_size);
        for (std::size_t tx = 0; tx < diff.width; ++tx) {
            const std::size_t x0 = tx * tile_size * bytes_per_pixel;
            const std::size_t x1 = std::min(width, (tx + 1) * tile_size) * bytes_per_pixel;
            bool changed = false;
            for (std::size_t y = y0; y < y1 && !changed; ++y) {
                changed = std::memcmp(previous + y * previous_stride + x0, current + y * current_stride + x0, x1 - x0) != 0;
            }
            diff.tiles.push_back({changed, static_cast<std::uint8_t>(changed ? 1u : 0u)});
        }
    }
    return diff;
}

TileDiff compute_tile_difference(
    const std::vector<std::uint8_t>& previous,
    const std::vector<std::uint8_t>& current,
    std::size_t width,
    std::size_t height,
    std::size_t tile_size) {
    validate(width, height, 1, tile_size);
    if (previous.size() < width * height || current.size() < width * height) {
        throw std::invalid_argument("frame buffers must cover the given dimensions");
    }
    return compute_tile_difference(previous.data(), width, current.data(), width, width, height, 1, tile_size);
}

TileDiff all_tiles_changed(std::size_t width, std::size_t height, std::size_t tile_size) {
    validate(width, height, 1, tile_size);
    TileDiff diff = make_diff(width, height, tile_size);
    diff.tiles.assign(diff.width * diff.height, {true, 1});
    return diff;
}

}  // namespace capturer::image_processing
