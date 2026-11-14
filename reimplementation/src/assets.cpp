#include "swiv3d/assets.hpp"

#include <algorithm>
#include <fstream>
#include <iterator>
#include <stdexcept>
#include <utility>

namespace swiv3d {
namespace {

constexpr std::size_t directory_record_bytes = 12;

std::uint32_t read_u32(const std::vector<std::uint8_t>& bytes, std::size_t offset) {
    if (offset > bytes.size() || bytes.size() - offset < sizeof(std::uint32_t)) {
        throw std::runtime_error("Truncated resource directory");
    }
    std::uint32_t result = 0;
    for (std::size_t index = 0; index < sizeof(result); ++index) {
        result |= static_cast<std::uint32_t>(bytes[offset + index]) << (index * 8);
    }
    return result;
}

}

Archive::Archive(std::vector<std::uint8_t> bytes) : bytes_(std::move(bytes)) {
    if (bytes_.size() < directory_record_bytes) {
        throw std::runtime_error("Missing resource directory footer");
    }
    const auto footer = bytes_.size() - directory_record_bytes;
    const auto count = read_u32(bytes_, footer);
    if (count == 0 || count > bytes_.size() / directory_record_bytes ||
        read_u32(bytes_, footer + 8) != 0) {
        throw std::runtime_error("Invalid resource directory footer");
    }
    const auto start = bytes_.size() - static_cast<std::size_t>(count) * directory_record_bytes;
    for (auto cursor = start; cursor < footer; cursor += directory_record_bytes) {
        const Resource entry{read_u32(bytes_, cursor), read_u32(bytes_, cursor + 4),
                             read_u32(bytes_, cursor + 8)};
        if (entry.id == 0 || entry.offset > start || entry.size > start - entry.offset) {
            throw std::runtime_error("Resource exceeds archive payload");
        }
        resources_.push_back(entry);
    }
}

Archive Archive::open(const std::filesystem::path& path) {
    std::ifstream stream(path, std::ios::binary);
    if (!stream) {
        throw std::runtime_error("Cannot open archive: " + path.string());
    }
    std::vector<std::uint8_t> bytes{std::istreambuf_iterator<char>(stream), {}};
    if (stream.bad()) {
        throw std::runtime_error("Cannot read archive: " + path.string());
    }
    return Archive(std::move(bytes));
}

const std::vector<Resource>& Archive::resources() const {
    return resources_;
}

std::vector<std::uint8_t> Archive::load(std::uint32_t id) const {
    const auto found = std::find_if(resources_.begin(), resources_.end(), [id](const Resource& entry) {
        return entry.id == id;
    });
    if (found == resources_.end()) {
        throw std::runtime_error("Unknown resource ID");
    }
    return {bytes_.begin() + found->offset, bytes_.begin() + found->offset + found->size};
}

void copy_image(const std::vector<std::uint8_t>& image, Planes& destination) {
    if (image.size() != image_bytes) {
        throw std::runtime_error("Unsupported global image size");
    }
    const auto source = image.begin() + palette_bytes + image_table_bytes;
    for (std::size_t plane = 0; plane < plane_count; ++plane) {
        std::copy_n(source + plane * plane_pixels, plane_pixels,
                    destination.begin() + plane * plane_stride);
    }
}

std::vector<std::uint8_t> indexed_pixels(const Planes& planes) {
    std::vector<std::uint8_t> pixels(image_width * image_height);
    for (std::size_t y = 0; y < image_height; ++y) {
        for (std::size_t x = 0; x < image_width; ++x) {
            pixels[y * image_width + x] = planes[(x % plane_count) * plane_stride +
                                                y * (image_width / plane_count) + x / plane_count];
        }
    }
    return pixels;
}

}
