#pragma once

#include "swiv3d/assets.hpp"

namespace swiv3d::detail {

enum class AlphaIntensity { full, half };
enum class AlphaDestination { ram, video };

class AlphaPresenter {
public:
    AlphaPresenter(std::vector<std::uint8_t>& pixels, const std::vector<std::uint8_t>& table,
                   AlphaDestination destination = AlphaDestination::ram);
    void draw(const std::vector<std::uint8_t>& image, int x, int y, std::uint32_t frame,
              AlphaIntensity intensity = AlphaIntensity::full) const;
    void blend(int x, int y, std::uint8_t code) const;

private:
    void blend_planar(std::size_t destination, std::uint8_t code) const;
    std::vector<std::uint8_t>& pixels_;
    const std::vector<std::uint8_t>& table_;
    AlphaDestination destination_;
};

}
