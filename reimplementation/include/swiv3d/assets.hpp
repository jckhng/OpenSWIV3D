#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <vector>

namespace swiv3d {

struct Resource {
    std::uint32_t offset;
    std::uint32_t size;
    std::uint32_t id;
};

class Archive {
public:
    explicit Archive(std::vector<std::uint8_t> bytes);
    static Archive open(const std::filesystem::path& path);
    const std::vector<Resource>& resources() const;
    std::vector<std::uint8_t> load(std::uint32_t id) const;

private:
    std::vector<std::uint8_t> bytes_;
    std::vector<Resource> resources_;
};

constexpr std::size_t image_width = 320;
constexpr std::size_t image_height = 400;
constexpr std::size_t palette_bytes = 256 * 3;
constexpr std::size_t image_table_bytes = 0x2000;
constexpr std::size_t plane_count = 4;
constexpr std::size_t plane_pixels = image_width * image_height / plane_count;
constexpr std::size_t plane_stride = 0x8000;
constexpr std::size_t planar_bytes = plane_count * plane_stride;
constexpr std::size_t image_bytes = palette_bytes + image_table_bytes + plane_count * plane_pixels;
using Planes = std::array<std::uint8_t, planar_bytes>;

// PCSPEC_copyimage_: preserve each plane's unused tail, including prior residue.
void copy_image(const std::vector<std::uint8_t>& image, Planes& destination);
std::vector<std::uint8_t> indexed_pixels(const Planes& planes);

}
