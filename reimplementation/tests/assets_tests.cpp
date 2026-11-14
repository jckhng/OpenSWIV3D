#include "swiv3d/assets.hpp"

#include <iostream>
#include <stdexcept>

namespace {

void require(bool condition, const char* message) {
    if (!condition) {
        throw std::runtime_error(message);
    }
}

void put(std::vector<std::uint8_t>& bytes, std::size_t offset, std::uint32_t value) {
    for (std::size_t i = 0; i < sizeof(value); ++i) {
        bytes.at(offset + i) = static_cast<std::uint8_t>(value >> (i * 8));
    }
}

void test_archive() {
    std::vector<std::uint8_t> bytes(28);
    bytes[0] = 7;
    bytes[1] = 11;
    put(bytes, 4, 0);
    put(bytes, 8, 2);
    put(bytes, 12, 100);
    put(bytes, 16, 2); // Count includes the terminal record.
    put(bytes, 20, 0xdeadbeef); // Terminal middle word is opaque.
    const swiv3d::Archive archive(bytes);
    require(archive.load(100) == std::vector<std::uint8_t>({7, 11}), "Wrong member payload");
    bool rejected = false;
    try {
        archive.load(101);
    } catch (const std::runtime_error&) {
        rejected = true;
    }
    require(rejected, "Unknown resource accepted");
    put(bytes, 8, 5);
    rejected = false;
    try {
        const swiv3d::Archive invalid(bytes);
    } catch (const std::runtime_error&) {
        rejected = true;
    }
    require(rejected, "Payload overlapping directory accepted");
    put(bytes, 16, UINT32_MAX);
    rejected = false;
    try {
        const swiv3d::Archive invalid(bytes);
    } catch (const std::runtime_error&) {
        rejected = true;
    }
    require(rejected, "Overflowing directory count accepted");
}

void test_image() {
    std::vector<std::uint8_t> image(swiv3d::image_bytes);
    for (std::size_t plane = 0; plane < swiv3d::plane_count; ++plane) {
        const auto start = swiv3d::palette_bytes + swiv3d::image_table_bytes + plane * swiv3d::plane_pixels;
        for (std::size_t pixel = 0; pixel < swiv3d::plane_pixels; ++pixel) {
            image[start + pixel] = static_cast<std::uint8_t>(plane * 37 + pixel);
        }
    }
    swiv3d::Planes planes;
    constexpr std::uint8_t residue = 0xa5;
    planes.fill(residue);
    swiv3d::copy_image(image, planes);
    const auto pixels = swiv3d::indexed_pixels(planes);
    for (std::size_t plane = 0; plane < swiv3d::plane_count; ++plane) {
        for (std::size_t tail = swiv3d::plane_pixels; tail < swiv3d::plane_stride; ++tail) {
            require(planes[plane * swiv3d::plane_stride + tail] == residue, "Plane residue overwritten");
        }
    }
    for (std::size_t pixel = 0; pixel < pixels.size(); ++pixel) {
        const auto expected = static_cast<std::uint8_t>((pixel % 4) * 37 + pixel / 4);
        require(pixels[pixel] == expected, "Wrong plane interleave");
    }
}

}

int main() {
    try {
        test_archive();
        test_image();
        std::cout << "Archive bounds and retained plane residue passed\n";
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
