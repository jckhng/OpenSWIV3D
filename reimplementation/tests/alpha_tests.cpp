#include "../src/alpha.hpp"

#include <iostream>
#include <stdexcept>

namespace {

void require(bool value, const char* message) {
    if (!value) {
        throw std::runtime_error(message);
    }
}

void test_unaligned_planes() {
    std::vector<std::uint8_t> table(65536);
    for (std::size_t code = 0; code < 256; ++code) {
        for (std::size_t background = 0; background < 256; ++background) {
            table[code * 256 + background] = static_cast<std::uint8_t>(code ^ background);
        }
    }
    const std::vector<std::uint8_t> image{4,0,0,0, 0,0x11,0xff, 1,0x12,0xff, 2,0x13,0xff, 3,0x14,0xff};
    constexpr std::uint8_t background = 7;
    constexpr std::uint8_t codes[]{1, 18, 35, 52};
    for (int origin = 0; origin < 4; ++origin) {
        std::vector<std::uint8_t> pixels(swiv3d::image_width * swiv3d::image_height, background);
        swiv3d::detail::AlphaPresenter presenter(pixels, table);
        presenter.draw(image, origin, 10, 0);
        for (int x = 0; x < 12; ++x) {
            const auto expected = x >= origin && x < origin + 4 ? codes[x - origin] ^ background : background;
            require(pixels[10 * swiv3d::image_width + x] == expected, "Unaligned alpha plane wrap changed glyph position or blending");
        }
        require(pixels[9 * swiv3d::image_width] == background, "Alpha run crossed its row origin");
    }
}

void test_skips_and_truncation() {
    std::vector<std::uint8_t> table(65536);
    for (std::size_t i = 0; i < table.size(); ++i) {
        table[i] = static_cast<std::uint8_t>(i >> 8);
    }
    std::vector<std::uint8_t> pixels(swiv3d::image_width * swiv3d::image_height);
    const std::vector<std::uint8_t> image{4,0,0,0, 0,0x82,0x11,0x80,77,0,0x12,0xff, 0xff,0xff,0xff};
    swiv3d::detail::AlphaPresenter presenter(pixels, table);
    presenter.draw(image, 0, 0, 0);
    require(pixels[8] == 1 && pixels[swiv3d::image_width] == 2, "Alpha short/long skip units changed");
    bool rejected = false;
    try {
        presenter.draw({4,0,0,0,0x80}, 0, 0, 0);
    } catch (const std::out_of_range&) {
        rejected = true;
    }
    require(rejected, "Truncated alpha skip accepted");
}

void test_half_intensity_preserves_group() {
    std::vector<std::uint8_t> table(65536);
    for (std::size_t i = 0; i < table.size(); ++i) { table[i] = static_cast<std::uint8_t>(i >> 8); }
    std::vector<std::uint8_t> pixels(swiv3d::image_width * swiv3d::image_height);
    swiv3d::detail::AlphaPresenter presenter(pixels, table);
    presenter.draw({4,0,0,0,3,0x1f,0xff,0xff,0xff,0xff}, 0,0,0, swiv3d::detail::AlphaIntensity::half);
    require(pixels[0] == 0x37, "Half-intensity icon shifted its alpha group instead of its low nibble");
}

void test_video_read_plane() {
    std::vector<std::uint8_t> table(65536);
    for (std::size_t i = 0; i < table.size(); ++i) { table[i] = static_cast<std::uint8_t>((i >> 8) + (i & 255)); }
    std::vector<std::uint8_t> pixels(swiv3d::image_width * swiv3d::image_height);
    pixels[0] = 10; pixels[1] = 20; pixels[2] = 30; pixels[3] = 40;
    swiv3d::detail::AlphaPresenter presenter(pixels, table, swiv3d::detail::AlphaDestination::video);
    presenter.draw({4,0,0,0,0x11,0xff,0x11,0xff,0x11,0xff,0x11,0xff}, 0,0,0);
    require(pixels[0] == 11 && pixels[1] == 12 && pixels[2] == 12 && pixels[3] == 12,
            "Video alpha stopped reading plane zero or lost prior plane writes");
}

}

int main() {
    try {
        test_unaligned_planes();
        test_skips_and_truncation();
        test_half_intensity_preserves_group();
        test_video_read_plane();
        std::cout << "Alpha presenter tests passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
