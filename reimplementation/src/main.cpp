#include "swiv3d/assets.hpp"

#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>

namespace {

void write(const std::filesystem::path& path, const std::uint8_t* data, std::size_t size) {
    std::ofstream stream(path, std::ios::binary);
    stream.write(reinterpret_cast<const char*>(data), static_cast<std::streamsize>(size));
    if (!stream) {
        throw std::runtime_error("Cannot write output: " + path.string());
    }
}

}

int main(int argc, char** argv) {
    try {
        if (argc != 2 && argc != 4) {
            throw std::runtime_error("Usage: swiv3d_assets ARCHIVE [IMAGE_ID OUTPUT_DIRECTORY]");
        }
        const auto archive = swiv3d::Archive::open(argv[1]);
        std::cout << "{\"resources\":[";
        const char* separator = "";
        for (const auto& resource : archive.resources()) {
            std::cout << separator << "{\"id\":" << resource.id << ",\"offset\":" << resource.offset
                      << ",\"size\":" << resource.size << "}";
            separator = ",";
        }
        std::cout << "]}\n";
        if (argc == 2) {
            return 0;
        }
        const std::string argument = argv[2];
        std::size_t end = 0;
        const auto id = std::stoul(argument, &end);
        if (end != argument.size() || id > UINT32_MAX || argument.front() == '-') {
            throw std::runtime_error("Invalid image ID");
        }
        const auto image = archive.load(static_cast<std::uint32_t>(id));
        swiv3d::Planes planes{};
        swiv3d::copy_image(image, planes);
        const auto pixels = swiv3d::indexed_pixels(planes);
        const std::filesystem::path output = argv[3];
        if (std::filesystem::exists(output)) {
            throw std::runtime_error("Output directory must be fresh");
        }
        std::filesystem::create_directories(output);
        write(output / "resource.bin", image.data(), image.size());
        write(output / "planes.bin", planes.data(), planes.size());
        write(output / "pixels.bin", pixels.data(), pixels.size());
        write(output / "palette.bin", image.data(), swiv3d::palette_bytes);
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
