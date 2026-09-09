#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
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

class FrameProcessor {
public:
    virtual ~FrameProcessor() = default;
    virtual std::string name() const = 0;
    virtual TileDiff process(const std::vector<std::uint8_t>& previous, const std::vector<std::uint8_t>& current, std::size_t width, std::size_t height) = 0;
};

class TileDifferenceProcessor : public FrameProcessor {
public:
    explicit TileDifferenceProcessor(std::size_t tile_size) : tile_size_(tile_size) {}

    std::string name() const override { return "tile-difference"; }
    TileDiff process(const std::vector<std::uint8_t>& previous, const std::vector<std::uint8_t>& current, std::size_t width, std::size_t height) override;

private:
    std::size_t tile_size_;
};

}  // namespace capturer::image_processing
