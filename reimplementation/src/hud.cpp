#include "hud.hpp"
#include "alpha.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <iomanip>
#include <map>
#include <sstream>
#include <stdexcept>

namespace swiv3d::detail {
namespace {

constexpr double pi = 3.14159265358979323846;
constexpr int angle_mask = 511;
constexpr std::uint32_t strings_id = 100;
constexpr std::uint32_t icons_id = 501;
constexpr std::uint32_t energy_id = 503;
constexpr std::uint32_t font_id = 507;
constexpr std::uint32_t widths_id = 508;
constexpr std::uint32_t map_dial_id = 510;
constexpr std::uint32_t map_shell_id = 511;
constexpr std::uint32_t map_arrow_id = 512;
constexpr std::size_t map_width = 64;
constexpr std::size_t energy_rows = 81;
constexpr std::size_t energy_columns = 5;
constexpr std::size_t energy_frame_bytes = plane_count * energy_rows * energy_columns;
constexpr int score_digits = 7;
constexpr int minimap_grid_radius = 4;
constexpr int minimap_marker_radius_squared = 600;

struct MapColumn { int skip, count; };
// Recovered PCSPEC_outmap viewport bounds, in logical 320x400 coordinates.
constexpr std::array<MapColumn, map_width> map_columns{{
    {0,0},{0,0},{0,0},{0,0},{0,0},{0,0},{0,0},{52,8},
    {44,25},{39,34},{36,41},{33,47},{30,52},{28,56},{26,60},{25,63},
    {23,67},{22,69},{20,72},{19,74},{18,77},{17,79},{16,80},{16,81},
    {15,83},{14,84},{14,85},{13,87},{13,87},{12,88},{12,89},{12,89},
    {12,89},{12,89},{12,89},{12,89},{12,89},{12,89},{12,89},{13,88},
    {13,87},{13,87},{14,85},{14,85},{15,83},{16,81},{16,81},{17,79},
    {18,77},{19,75},{21,71},{22,69},{23,67},{25,63},{26,61},{28,57},
    {31,52},{33,47},{36,41},{39,35},{44,25},{52,9},{0,0},{0,0}
}};

int shift(std::int64_t value, unsigned bits) {
    const auto divisor = std::int64_t{1} << bits;
    return static_cast<int>(value >= 0 ? value / divisor : -((-value + divisor - 1) / divisor));
}

int sine(int angle) { return static_cast<int>(std::round(256 * std::sin((angle & angle_mask) * 2 * pi / 512))); }
int cosine(int angle) { return sine(angle + 128); }

std::uint32_t word(const std::vector<std::uint8_t>& bytes, std::size_t offset) {
    return static_cast<std::uint32_t>(bytes.at(offset)) | static_cast<std::uint32_t>(bytes.at(offset + 1)) << 8 |
           static_cast<std::uint32_t>(bytes.at(offset + 2)) << 16 | static_cast<std::uint32_t>(bytes.at(offset + 3)) << 24;
}

std::size_t lookup(const std::vector<std::uint8_t>& bytes, std::size_t table, std::uint32_t key) {
    for (auto cursor = table; cursor + 8 <= bytes.size(); cursor += 8) {
        const auto candidate = word(bytes, cursor);
        if (candidate == key) {
            return word(bytes, cursor + 4);
        }
        if (candidate == UINT32_MAX) {
            break;
        }
    }
    throw std::runtime_error("Missing HUD string key");
}

enum class Align { left, center, right };

class Hud {
public:
    Hud(MissionFrame& frame, const Archive& globals, const FrontendState& request, const MissionState& state,
        const std::vector<std::uint8_t>& map, const std::vector<std::uint8_t>& alpha, const std::vector<std::uint8_t>& palette,
        MapMarkers markers = MapMarkers::normal)
        : frame_(frame), request_(request), state_(state), map_(map), palette_(palette), presenter_(frame.pixels, alpha), markers_(markers) {
        for (auto id : {strings_id, icons_id, energy_id, font_id, widths_id, map_dial_id, map_shell_id, map_arrow_id}) {
            resources_.emplace(id, globals.load(id));
        }
        for (const auto& entity : state.entities) {
            entities_.emplace(entity.slot, &entity);
        }
    }

    void draw() {
        frame_.world_pixels = frame_.pixels;
        const auto& player = entities_.at(state_.player)->words;
        if ((player[1] & 0x200) != 0) {
            inventory();
            std::ostringstream score;
            const auto value = static_cast<std::int64_t>(state_.display_score);
            if (value < 0) {
                score << '-';
            }
            score << std::setw(value < 0 ? score_digits - 1 : score_digits) << std::setfill('0') << (value < 0 ? -value : value);
            text(score.str(), 310, 370, Align::right);
            energy();
        }
        constexpr std::uint32_t title_updates = 80;
        if (state_.tick < title_updates) { text(string(200, request_.level), 160, 10, Align::center); }
        frame_.stats_pixels = frame_.pixels;
        minimap();
    }

    void debug_coordinates(int counter) const {
        const auto& player = entities_.at(state_.player)->words;
        char coordinates[100];
        std::snprintf(coordinates,sizeof(coordinates),"(%d,%d:%d) gs=%d",shift(player[7],14),shift(player[9],14),
                      player[13] & angle_mask,counter);
        text(coordinates,160,0,Align::center);
    }

private:
    void inventory() const {
        const auto& weapon = state_.weapons;
        const int selected = weapon.requested;
        presenter_.draw(resource(icons_id), 4, 354, selected == no_entity ? 0 : selected + 1);
        if (selected == no_entity) { return; }
        std::ostringstream ammunition;
        ammunition << 'x' << std::setw(2) << std::setfill('0') << weapon.ammunition.at(selected);
        text(ammunition.str(), 30, 354, Align::left);
        text(string(100, selected), 30, 372, Align::left);
        int y = 309, count = 0;
        for (int i = 1; i < 20; ++i) {
            const int slot = (selected + i) % 20;
            if (slot == 9 || slot == 19 || weapon.ammunition[slot] <= 0) { continue; }
            if (count <= 5) {
                presenter_.draw(resource(icons_id), 4, y, count == 5 ? 21 : slot + 1, AlphaIntensity::half);
                y -= 40;
            }
            ++count;
        }
    }

    const std::vector<std::uint8_t>& resource(std::uint32_t id) const { return resources_.at(id); }

    std::string string(std::uint32_t group, std::uint32_t key, std::uint32_t language) const {
        const auto& bytes = resource(strings_id);
        const auto start = lookup(bytes, lookup(bytes, 0, group + (language << 16)), key);
        const auto end = std::find(bytes.begin() + start, bytes.end(), 0);
        if (end == bytes.end()) {
            throw std::runtime_error("Unterminated HUD string");
        }
        return {bytes.begin() + start, end};
    }

    std::string string(std::uint32_t group, std::uint32_t key) const { return string(group, key, request_.language); }

    std::uint32_t glyph(unsigned char character) const {
        if (character < 127) {
            return character;
        }
        if (character == 127) {
            return 0;
        }
        const auto pages = string(2, 0, 99);
        return character + (pages.at(request_.language) - '0') * 128;
    }

    void text(const std::string& value, int x, int y, Align alignment) const {
        int width = 0;
        for (unsigned char character : value) {
            width += static_cast<int>(word(resource(widths_id), glyph(character) * 4));
        }
        if (alignment == Align::center) {
            x -= width / 2;
        } else if (alignment == Align::right) {
            x -= width;
        }
        for (unsigned char character : value) {
            const auto index = glyph(character);
            presenter_.draw(resource(font_id), x, y, index);
            x += static_cast<int>(word(resource(widths_id), index * 4));
        }
    }

    void energy() const {
        const auto& bytes = resource(energy_id);
        if (bytes.size() < energy_frame_bytes) {
            throw std::runtime_error("Truncated energy bar");
        }
        const auto& player = entities_.at(state_.player)->words;
        if (player[2] < 100 && (state_.landscape_clock.water & 31) >= 20) { return; }
        std::size_t cursor = static_cast<std::size_t>(state_.health_bar) * energy_frame_bytes;
        for (std::size_t plane = 0; plane < plane_count; ++plane) {
            for (std::size_t row = 0; row < energy_rows; ++row) {
                for (std::size_t column = 0; column < energy_columns; ++column) {
                    presenter_.blend(292 + static_cast<int>(column * plane_count + plane), 10 + static_cast<int>(row), bytes[cursor++]);
                }
            }
        }
    }

    void plot(int x, int y, std::uint32_t colour) const {
        if (x < 0 || x >= static_cast<int>(image_width) || y < 0 || y >= static_cast<int>(image_height)) {
            return;
        }
        frame_.pixels[y * image_width + x] = palette_.at(palette_bytes + colour);
    }

    void pair(int x, int y, std::uint32_t colour) const {
        plot(x, y, colour);
        plot(x, y - 1, colour);
    }

    void minimap() {
        const auto& player = entities_.at(state_.player)->words;
        int heading = state_.objective_heading;
        if (state_.objective_targets.empty()) {
            heading += 8;
        } else {
            const auto& objective = entities_.at(state_.objective_targets.front().entity)->words;
            const int dx = shift(objective[7] - player[7], 8), dz = shift(objective[9] - player[9], 8);
            int best = -99999999;
            constexpr int candidate_offsets[]{0, -4, 4, -170, 170};
            for (int i = 0; i < 5; ++i) {
                const auto candidate = heading + candidate_offsets[i];
                const auto product = sine(candidate) * dx + cosine(candidate) * dz;
                if (product > best) {
                    best = product;
                    heading = candidate + (i == 3 ? 162 : i == 4 ? -162 : 0);
                }
            }
        }
        frame_.objective_heading = heading;
        const int yaw = player[13] & 508;
        const int s = sine(yaw), c = cosine(yaw);
        const int step_x = shift(s * -154, 8), step_z = shift(c * -154, 8);
        const int start_x = step_x * -55 + shift(player[7], 7);
        const int start_z = step_z * -55 + shift(player[9], 7);
        const auto delta = static_cast<std::uint32_t>(step_z) * 65536 + (static_cast<std::uint32_t>(step_x) & 65535);
        for (std::size_t x = 0; x < map_width; ++x) {
            auto source = static_cast<std::uint32_t>(start_z + s * (34 - static_cast<int>(x))) * 65536 +
                          (static_cast<std::uint32_t>(start_x + c * (static_cast<int>(x) - 34)) & 65535);
            source += delta * map_columns[x].skip;
            for (int row = 0; row < map_columns[x].count; ++row) {
                source += delta;
                const auto address = ((source >> 23) & 511) * map_side + ((source >> 7) & 511);
                frame_.pixels[(map_columns[x].skip + row) * image_width + x] = map_.at(address);
            }
        }
        std::vector<const EntityState*> markers;
        const int cell_x = shift(player[7], 17), cell_z = shift(player[9], 17);
        for (int z = cell_z - minimap_grid_radius; z <= cell_z + minimap_grid_radius; ++z) {
            for (int x = cell_x - minimap_grid_radius; x <= cell_x + minimap_grid_radius; ++x) {
                auto slot = state_.grid[(z & 63) * 64 + (x & 63)];
                while (slot != no_entity) {
                    const auto* entity = entities_.at(slot);
                    markers.push_back(entity);
                    slot = entity->words[0];
                }
            }
        }
        const auto relative = [&](const EntityState& entity) {
            const int dx = shift(entity.words[7] - player[7], 13), dz = shift(entity.words[9] - player[9], 13);
            return std::pair<int, int>{shift(dx * c - dz * s, 10), shift(s * dx + c * dz, 10)};
        };
        for (auto it = markers.rbegin(); it != markers.rend(); ++it) {
            const auto& entity = **it;
            const auto flags = entity.words[1] & 0x787f;
            int colour = -1;
            if (flags == 1) {
                colour = 0x147f;
            } else if (flags == 7 || flags == 0x17) {
                colour = 0x47f;
            } else if (flags == 0x41) {
                constexpr int colours[]{0x17f, 0x17f, 0, 0};
                colour = colours[entity.words[6] & 3];
            } else if (flags == 0x1001 || flags == 0x1003 || flags == 0x1013 || flags == 0x1023 || flags == 0x1033) {
                colour = 0x107f;
            }
            const auto [x, z] = relative(entity);
            if (colour == -1 && markers_ == MapMarkers::debug) { colour = 0x157f; }
            if (colour >= 0 && x * x + z * z < minimap_marker_radius_squared) {
                pair(x + 34, 55 - shift(z * 427, 8), colour);
            }
        }
        for (const auto& objective : state_.objective_targets) {
            const auto [x, z] = relative(*entities_.at(objective.entity));
            if (x * x + z * z >= minimap_marker_radius_squared) {
                continue;
            }
            const auto colour = objective.priority == state_.objective_targets.front().priority ? 0x107f : 0x17f;
            const int y = shift(z * 427, 8);
            pair(x + 31, 52 - y, colour);
            pair(x + 33, 52 - y, colour);
            pair(x + 32, 51 - y, colour);
            pair(x + 32, 53 - y, colour);
        }
        const auto direction = shift(-yaw, 2);
        presenter_.draw(resource(map_arrow_id), 0, 0, shift(-(yaw - heading), 2) & 127);
        presenter_.draw(resource(map_dial_id), 0, 0, direction & 31);
        presenter_.draw(resource(map_shell_id), 0, 0, direction & 127);
    }

    MissionFrame& frame_;
    const FrontendState& request_;
    const MissionState& state_;
    const std::vector<std::uint8_t>& map_;
    const std::vector<std::uint8_t>& palette_;
    AlphaPresenter presenter_;
    MapMarkers markers_;
    std::map<std::uint32_t, std::vector<std::uint8_t>> resources_;
    std::map<int, const EntityState*> entities_;
};

}

void draw_initial_hud(MissionFrame& frame, const Archive& globals, const FrontendState& request,
                      const MissionState& state, const std::vector<std::uint8_t>& map,
                      const std::vector<std::uint8_t>& alpha, const std::vector<std::uint8_t>& palette, MapMarkers markers) {
    Hud(frame, globals, request, state, map, alpha, palette, markers).draw();
}

void draw_debug_coordinates(MissionFrame& frame, const Archive& globals, const MissionState& state,
                            const std::vector<std::uint8_t>& alpha, int counter) {
    Hud(frame,globals,{},state,{},alpha,{}).debug_coordinates(counter);
}

}
