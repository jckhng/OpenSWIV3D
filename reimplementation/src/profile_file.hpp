#pragma once

#include <cstdint>
#include <filesystem>
#include <vector>

namespace swiv3d::detail {

std::vector<std::uint8_t> load_profile(const std::filesystem::path& path);
void save_profile(const std::filesystem::path& path, const std::vector<std::uint8_t>& bytes);

}
