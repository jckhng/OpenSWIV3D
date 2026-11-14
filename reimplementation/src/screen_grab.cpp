#include "screen_grab.hpp"

#include <fstream>

namespace swiv3d::detail {

bool save_screen_grab(const std::filesystem::path& path, const MissionFrame& frame) {
    if (frame.pixels.size() != image_width * image_height) { return false; }
    std::error_code error;
    if (!path.parent_path().empty()) { std::filesystem::create_directories(path.parent_path(),error); }
    if (error) { return false; }
    std::ofstream output(path,std::ios::binary | std::ios::trunc);
    if (!output) { return false; }

    constexpr std::size_t header_bytes = 54, palette_entries = 256, palette_entry_bytes = 4;
    constexpr std::uint32_t info_header_bytes = 40, colour_planes = 1, bits_per_pixel = 8;
    constexpr std::uint32_t pixel_offset = header_bytes + palette_entries * palette_entry_bytes;
    std::array<std::uint8_t,header_bytes> header{};
    const auto word = [&](std::size_t offset, std::uint32_t value) {
        for (unsigned i = 0; i < 4; ++i) { header[offset+i] = static_cast<std::uint8_t>(value >> (i*8)); }
    };
    header[0] = 'B'; header[1] = 'M';
    word(2,pixel_offset + image_width * image_height);
    word(10,pixel_offset); word(14,info_header_bytes); word(18,image_width); word(22,image_height);
    word(26,colour_planes | (bits_per_pixel << 16)); word(46,palette_entries); word(50,palette_entries);
    output.write(reinterpret_cast<const char*>(header.data()),header.size());
    for (std::size_t i = 0; i < palette_entries; ++i) {
        const std::array<std::uint8_t,palette_entry_bytes> colour{
            static_cast<std::uint8_t>(frame.palette[i*3+2] << 2),
            static_cast<std::uint8_t>(frame.palette[i*3+1] << 2),
            static_cast<std::uint8_t>(frame.palette[i*3] << 2),0};
        output.write(reinterpret_cast<const char*>(colour.data()),colour.size());
    }
    for (std::size_t y = image_height; y > 0; --y) {
        output.write(reinterpret_cast<const char*>(frame.pixels.data() + (y-1)*image_width),image_width);
    }
    output.close();
    return static_cast<bool>(output);
}

}
