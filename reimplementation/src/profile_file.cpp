#include "profile_file.hpp"

#include <fstream>
#include <iterator>
#include <stdexcept>

namespace swiv3d::detail {

std::vector<std::uint8_t> load_profile(const std::filesystem::path& path) {
    std::ifstream input(path, std::ios::binary);
    if (!input) { throw std::runtime_error("Cannot read pilot configuration: " + path.string()); }
    std::vector<std::uint8_t> bytes{std::istreambuf_iterator<char>(input), {}};
    if (input.bad()) { throw std::runtime_error("Cannot finish reading pilot configuration"); }
    return bytes;
}

void save_profile(const std::filesystem::path& path, const std::vector<std::uint8_t>& bytes) {
    if (path.empty()) { return; }
    if (!path.parent_path().empty()) { std::filesystem::create_directories(path.parent_path()); }
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    output.write(reinterpret_cast<const char*>(bytes.data()), bytes.size());
    output.close();
    if (!output) { throw std::runtime_error("Cannot save pilot configuration: " + path.string()); }
}

}
