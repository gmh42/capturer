#include <cassert>
#include <cstdint>
#include <vector>

#include "capturer/image_processing/tile_difference.hpp"

int main() {
    std::vector<uint8_t> prev(64 * 64, 0);
    std::vector<uint8_t> curr(64 * 64, 0);

    for (std::size_t i = 0; i < prev.size(); ++i) {
        prev[i] = static_cast<uint8_t>(i % 256);
        curr[i] = static_cast<uint8_t>((i + 1) % 256);
    }

    auto diff = capturer::image_processing::compute_tile_difference(prev, curr, 64, 64, 16);

    assert(diff.width == 4);
    assert(diff.height == 4);
    assert(diff.tiles.size() == 16);
    assert(diff.tiles[0].changed == true);
    assert(diff.tiles[0].value == 1);
    assert(diff.tiles[15].changed == true);

    return 0;
}
