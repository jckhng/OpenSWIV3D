#include "alpha.hpp"

#include <stdexcept>

namespace swiv3d::detail {
namespace {

constexpr std::uint8_t end_plane = 0xff;
constexpr std::uint8_t long_skip = 0x80;
constexpr std::uint8_t skip_flag = 0x80;
constexpr std::size_t blend_table_bytes = 65536;
constexpr std::size_t plane_row_bytes = image_width / plane_count;

std::uint32_t word(const std::vector<std::uint8_t>& data, std::size_t offset) {
    return static_cast<std::uint32_t>(data.at(offset)) | static_cast<std::uint32_t>(data.at(offset + 1)) << 8 |
           static_cast<std::uint32_t>(data.at(offset + 2)) << 16 | static_cast<std::uint32_t>(data.at(offset + 3)) << 24;
}

}

AlphaPresenter::AlphaPresenter(std::vector<std::uint8_t>& pixels, const std::vector<std::uint8_t>& table,
                               AlphaDestination destination)
    : pixels_(pixels), table_(table), destination_(destination) {
    if (pixels.size() != image_width * image_height || table.size() != blend_table_bytes) {
        throw std::runtime_error("Unsupported alpha destination or blend table");
    }
}

void AlphaPresenter::blend(int x, int y, std::uint8_t code) const {
    if (x < 0 || x >= static_cast<int>(image_width) || y < 0 || y >= static_cast<int>(image_height)) {
        return;
    }
    auto& pixel = pixels_[y * image_width + x];
    const auto background = destination_ == AlphaDestination::video ? pixels_[y * image_width + (x & ~3)] : pixel;
    pixel = table_[static_cast<std::size_t>(code) * 256 + background];
}

void AlphaPresenter::blend_planar(std::size_t destination, std::uint8_t code) const {
    if (destination >= planar_bytes) {
        throw std::runtime_error("Alpha run exceeds planar storage");
    }
    const auto plane = destination / plane_stride;
    const auto offset = destination % plane_stride;
    const auto x = (offset % plane_row_bytes) * plane_count + plane;
    const auto y = offset / plane_row_bytes;
    blend(static_cast<int>(x), static_cast<int>(y), code);
}

void AlphaPresenter::draw(const std::vector<std::uint8_t>& image, int x, int y, std::uint32_t frame, AlphaIntensity intensity) const {
    if (x < 0 || y < 0) {
        throw std::runtime_error("Negative alpha origin is not recovered");
    }
    auto cursor = static_cast<std::size_t>(word(image, static_cast<std::size_t>(frame) * 4));
    auto base = static_cast<std::size_t>(y) * plane_row_bytes + x / plane_count + (x % plane_count) * plane_stride;
    auto group = static_cast<std::uint8_t>(y * 16);
    for (std::size_t plane = 0; plane < plane_count; ++plane) {
        auto destination = base;
        while (true) {
            const auto command = image.at(cursor++);
            if (command == end_plane) {
                break;
            }
            if (command == long_skip) {
                destination += image.at(cursor) | image.at(cursor + 1) << 8;
                cursor += 2;
                continue;
            }
            if ((command & skip_flag) != 0) {
                destination += command & ~skip_flag;
                continue;
            }
            const auto run = command >> 4;
            if (run == 0) {
                group = static_cast<std::uint8_t>((command & 15) << 4);
                continue;
            }
            const auto level = intensity == AlphaIntensity::half ? (command & 15) >> 1 : command & 15;
            const auto code = static_cast<std::uint8_t>(group + level);
            for (int i = 0; i < run; ++i) {
                blend_planar(destination++, code);
            }
        }
        base += plane_stride;
        if (base >= planar_bytes) {
            base -= planar_bytes - 1;
        }
    }
}

}
