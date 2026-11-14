#include "mission_render.hpp"
#include "ground_effects.hpp"

#include <algorithm>
#include <cmath>
#include <list>
#include <stdexcept>

namespace swiv3d::detail {
namespace {

constexpr double pi = 3.14159265358979323846;
constexpr int angle_mask = 511;
constexpr int map_mask = 511;
constexpr int near_depth = 100;
constexpr int far_depth = 2000;
constexpr int projection_bias = 25;
constexpr int screen_center_x = 159;
constexpr int screen_center_y = 199;
constexpr int screen_max_y = 399;
constexpr int screen_min_y = -1;
constexpr int screen_max_x = 319;
constexpr int maximum_search_steps = 4096;
constexpr int terrain_detail_threshold = 170;
constexpr int smooth_model_detail_threshold = 129;
constexpr int textured_shadow_detail_threshold = 65;
constexpr std::uint32_t rotor_phase_mask = 255;
constexpr std::uint32_t water_phase_mask = 31;

int shift(std::int64_t value, unsigned bits) {
    const auto divisor = std::int64_t{1} << bits;
    return static_cast<int>(value >= 0 ? value / divisor : -((-value + divisor - 1) / divisor));
}

int sine(int angle) { return static_cast<int>(std::round(256 * std::sin((angle & angle_mask) * 2 * pi / 512))); }
int cosine(int angle) { return sine(angle + 128); }
int sine320(int angle) { return static_cast<int>(std::round(320 * std::sin((angle & angle_mask) * 2 * pi / 512))); }
int cosine320(int angle) { return sine320(angle + 128); }

struct Vector {
    float x = 0, y = 0, z = 0;
};

Vector unit(Vector value) {
    const auto length = std::sqrt(static_cast<double>(value.x) * value.x +
                                  static_cast<double>(value.y) * value.y + static_cast<double>(value.z) * value.z);
    return {static_cast<float>(value.x / length), static_cast<float>(value.y / length), static_cast<float>(value.z / length)};
}

Vector cross(Vector a, Vector b) {
    return {a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x};
}

struct IntVector {
    int x, y, z;
};

IntVector scaled(Vector v, int factor) {
    return {static_cast<int>(static_cast<double>(v.x) * factor), static_cast<int>(static_cast<double>(v.y) * factor),
            static_cast<int>(static_cast<double>(v.z) * factor)};
}

int dot(IntVector a, IntVector b) {
    return a.x * b.x + a.y * b.y + a.z * b.z;
}

struct Point {
    int x = 0, y = 0, shade = 0, texture = 0, depth = 0;
    bool visible = false;
};

struct MapPoint {
    int x, z;
    bool operator==(MapPoint b) const { return x == b.x && z == b.z; }
    bool operator!=(MapPoint b) const { return !(*this == b); }
};

MapPoint operator+(MapPoint a, MapPoint b) { return {a.x + b.x, a.z + b.z}; }
MapPoint operator-(MapPoint a, MapPoint b) { return {a.x - b.x, a.z - b.z}; }

struct Scanline {
    MapPoint first, last;
};

struct Column {
    int top = screen_max_y;
    int cursor = screen_max_y + 1;
    int previous_y = 99999;
    int u = 0, v = 0, shade = 99999, texture = 0;
    bool rising = false;
    std::vector<std::pair<int, int>> clips;
};

class Renderer {
public:
    Renderer(const MissionState& state, const FrontendState& request,
             const std::vector<std::uint8_t>& bytes, const std::map<std::uint32_t, Resource>& directory,
             const std::function<int(int, int)>& height, const Archive* globals, Camera* retained, RenderTiming timing,
             const std::function<int(int, int)>& surface_height)
        : state_(state), request_(request), bytes_(bytes), directory_(directory), height_(height),
          surface_height_(surface_height ? surface_height : height) {
        const int size = std::clamp(timing.screen_size,0,255) + 64;
        screen_min_x = 159 - size / 2;
        screen_max_x = 319 - screen_min_x;
        screen_min_y = 199 - size * 160 / 256 - 1;
        screen_max_y = 399 - (screen_min_y + 1);
        projection_width_ = screen_max_x - screen_min_x;
        for (auto& column : columns_) { column.top = screen_max_y; column.cursor = screen_max_y + 1; }
        detail_ = timing.detail;
        if (globals != nullptr) {
            flame_ = globals->load(504);
            sprites_ = globals->load(505);
            hud_ = globals->load(506);
        }
        retained_ = retained != nullptr ? retained : &local_camera_;
        if (detail_ == 240) { retained_->water_clock = state.landscape_clock.water; }
        for (int i = 0; i < 64; ++i) {
            const int clock = static_cast<int>(retained_->water_clock & angle_mask);
            water_x_[i] = shift(sine(clock * 9 + i * 8),6);
            water_y_[i] = shift(sine(clock * 7 + i * 8),6);
        }
        frame_.pixels.resize(image_width * image_height);
        const auto palette = directory_.at(11003).offset;
        std::copy_n(bytes_.begin() + palette, palette_bytes, frame_.palette.begin());
        const auto& player = *std::find_if(state.entities.begin(), state.entities.end(), [&](const EntityState& entity) {
            return entity.slot == state.player;
        });
        const auto& w = player.words;
        const int yaw = w[13] & angle_mask;
        Vector at{static_cast<float>(shift(w[7], 8) + shift(sine(yaw), 1)),
                        static_cast<float>(shift(w[8], 8)),
                        static_cast<float>(shift(w[9], 8) + shift(cosine(yaw), 1))};
        retained_->pitch = std::clamp(retained_->pitch,15,100);
        const int camera_pitch = retained_->pitch;
        constexpr int camera_distance = 550;
        const int horizontal = shift(camera_distance * cosine(camera_pitch), 8);
        const int view_yaw = yaw + retained_->yaw;
        Vector position{at.x - shift(sine(view_yaw) * horizontal, 8),
                        at.y + shift(camera_distance * sine(camera_pitch), 8),
                        at.z - shift(cosine(view_yaw) * horizontal, 8)};
        if (state.ground_effects.active != no_entity) {
            constexpr float effect_camera_lift = 900.0f;
            position.x -= shift(sine(view_yaw) * horizontal,8);
            position.z -= shift(cosine(view_yaw) * horizontal,8);
            position.y += effect_camera_lift;
        }
        int floor = 0;
        const int fx = static_cast<int>(position.x * 256);
        const int fz = static_cast<int>(position.z * 256);
        for (auto offset : {MapPoint{0, 0}, MapPoint{30000, 0}, MapPoint{-30000, 0}, MapPoint{0, 30000}, MapPoint{0, -30000}}) {
            floor = std::max(floor, height_(fx + offset.x, fz + offset.z));
        }
        position.y = std::max(position.y, floor / 256.0f + 120.0f);
        float tilt = -w[15] * (1.0f / 75.0f);
        if (retained != nullptr) {
            if (timing.frames_per_second < 4) { throw std::runtime_error("Camera frame rate must be at least four"); }
            if (retained->followed == player.slot) {
                constexpr float half_world = 20000.0f, world_size = 32768.0f;
                const float gain = 1.0f / (timing.frames_per_second / 4);
                const std::array<float, 3> ideal_at{at.x, at.y, at.z}, ideal_position{position.x, position.y, position.z};
                for (std::size_t i = 0; i < 3; ++i) {
                    if (i != 1) {
                        while (retained->target[i] - ideal_at[i] > half_world) { retained->target[i] -= world_size; }
                        while (retained->target[i] - ideal_at[i] < -half_world) { retained->target[i] += world_size; }
                        while (retained->position[i] - ideal_position[i] > half_world) { retained->position[i] -= world_size; }
                        while (retained->position[i] - ideal_position[i] < -half_world) { retained->position[i] += world_size; }
                    }
                    retained->target[i] = static_cast<float>(retained->target[i] + (static_cast<double>(ideal_at[i]) - retained->target[i]) * gain);
                    if (!retained->hold_position) {
                        retained->position[i] = static_cast<float>(retained->position[i] + (static_cast<double>(ideal_position[i]) - retained->position[i]) * gain);
                    }
                }
                if (retained->hold_position) {
                    constexpr float hold_range_squared = 3500000.0f;
                    const float dx = retained->position[0] - retained->target[0], dz = retained->position[2] - retained->target[2];
                    if (dx * dx + dz * dz > hold_range_squared) { retained->hold_position = false; }
                }
                retained->tilt = static_cast<float>(retained->tilt + (static_cast<double>(tilt) - retained->tilt) / timing.frames_per_second);
            } else {
                retained->followed = player.slot;
                retained->target = {at.x, at.y, at.z};
                retained->position = {position.x, position.y, position.z};
                retained->tilt = tilt;
            }
            retained->tilt = std::clamp(retained->tilt, -0.5f, 0.5f);
            at = {retained->target[0], retained->target[1], retained->target[2]};
            position = {retained->position[0], retained->position[1], retained->position[2]};
            tilt = retained->tilt;
        }
        frame_.camera_position = {position.x, position.y, position.z};
        frame_.camera_target = {at.x, at.y, at.z};
        frame_.camera_tilt = tilt;
        camera_ = {static_cast<int>(position.x), static_cast<int>(position.y), static_cast<int>(position.z)};
        forward_ = unit({at.x - position.x, at.y - position.y, at.z - position.z});
        auto right = unit(cross({0, 1, 0}, forward_));
        right.y = tilt;
        right = unit(right);
        auto up = unit(cross(forward_, right));
        forward_ = unit(forward_);
        right = unit(cross(up, forward_));
        right_ = scaled(right, projection_width_);
        up_ = scaled(up, projection_width_ * 5 / 3);
        direction_ = scaled(forward_, 256);
        const auto sound_right = scaled(right, 256);
        frame_.sound_right = {sound_right.x, sound_right.y, sound_right.z};
        horizontal_right_ = scaled(unit({forward_.x, 0, forward_.z}), 256);
        frame_.view_transform = {camera_.x, camera_.y, camera_.z, right_.x, right_.y, right_.z,
                                 up_.x, up_.y, up_.z, direction_.x, direction_.y, direction_.z};
        choose_direction();
    }

    MissionFrame render() {
        terrain();
        frame_.terrain_pixels = frame_.pixels;
        objects();
        target_marker(target_, retained_->lock, 0);
        target_marker(objective_, retained_->objective, 16);
        std::copy(retained_->lock.begin(), retained_->lock.end(), frame_.target_markers.begin());
        std::copy(retained_->objective.begin(), retained_->objective.end(), frame_.target_markers.begin() + 3);
        return frame_;
    }

private:

    int effect_height(int x, int y, int z) const {
        int shade = 0, texture = 0;
        apply_ground_effects(state_.ground_effects,x,z,y,shade,texture);
        return y;
    }

    void target_marker(const std::array<int, 3>& ideal, std::array<int, 3>& marker, int first_frame) {
        if (ideal[2] == -1) { marker[2] = -1; return; }
        if (marker[2] == -1) { marker = ideal; }
        marker[2] += (marker[2] < ideal[2]) - (marker[2] > ideal[2]);
        marker[0] += shift(ideal[0] - marker[0], 1);
        marker[1] += shift(ideal[1] - marker[1], 1);
        if (hud_.empty()) { return; }
        constexpr int width = 28, height = 40, frame_stride = 1160;
        const int top = marker[1] - height / 2;
        if (top < 0) { return; } // Preserve out_target_id's top-edge clipping.
        for (int column = 0; column < width; ++column) {
            const int x = marker[0] - width / 2 + column;
            if (x <= 0 || x > screen_max_x) { continue; }
            for (int row = 0; row < height && top + row < screen_max_y - 1; ++row) {
                const auto code = hud_.at((marker[2] + first_frame) * frame_stride + column * height + row);
                auto& pixel = frame_.pixels[(top + row) * image_width + x];
                pixel = bytes_.at(directory_.at(109).offset + code * 256 + pixel);
            }
        }
    }

    void choose_direction() {
        int best = 0;
        int direction = 0;
        for (int i = 0; i < 8; ++i) {
            const auto angle = 128 + i * 64;
            const auto product = sine(angle) * right_.x + cosine(angle) * right_.z;
            if (product > best) {
                best = product;
                direction = i;
            }
        }
        constexpr MapPoint horizontal[]{{1, 0}, {1, -1}, {0, -1}, {-1, -1}, {-1, 0}, {-1, 1}, {0, 1}, {1, 1}};
        constexpr MapPoint vertical[]{{0, 1}, {0, 1}, {1, 0}, {1, 0}, {0, -1}, {0, -1}, {-1, 0}, {-1, 0}};
        horizontal_ = horizontal[direction];
        vertical_ = vertical[direction];
    }

    int sample(std::uint32_t id, int x, int z) const {
        return bytes_.at(directory_.at(id).offset + (z & map_mask) * map_side + (x & map_mask));
    }

    Point project(MapPoint map, bool detailed, Point retained = {}) {
        int h = sample(100, map.x, map.z) * 4;
        int texture = sample(102, map.x, map.z);
        if (detailed) {
            const auto nx = sample(100, map.x, map.z - 1) - sample(100, map.x, map.z + 1);
            const auto nz = sample(100, map.x - 1, map.z) - sample(100, map.x + 1, map.z);
            auto square = static_cast<unsigned>(nx * nx + 64 * 64 + nz * nz) >> 11;
            square = std::min(square, 4000U);
            if (square == 0) {
                square = 4000;
            }
            const auto root = static_cast<int>(std::sqrt(square * 2048.0));
            const auto reciprocal = 65535 / root;
            const auto light = shift(shift(nx * reciprocal, 8) * -147 + shift(64 * reciprocal, 8) * 147 +
                                     shift(nz * reciprocal, 8) * 147, 9);
            retained.shade = std::max(light, 0) + 30 + state_.lighting[(map.z & 63) * 64 + (map.x & 63)];
            retained.texture = texture;
        }
        if ((request_.water & 1) != 0 && (texture & 224) == 0) {
            const auto phase = static_cast<int>(state_.landscape_clock.water & water_phase_mask);
            const auto wave_index = (map.z * 4 + phase + sine((map.x & 31) * 16)) & water_phase_mask;
            const int wave = static_cast<int>(std::round(16 * std::sin(wave_index * pi / 32)));
            h += wave;
            texture = std::max(wave + 1, 10);
            if (detailed) {
                retained.shade = wave * 2 + 80;
                retained.texture = 0;
            }
        }
        if (detailed) { apply_ground_effects(state_.ground_effects,map.x * 64,map.z * 64,h,retained.shade,retained.texture); }
        else { h = effect_height(map.x * 64,h,map.z * 64); }
        const IntVector relative{map.x * 64 - camera_.x, h - camera_.y, map.z * 64 - camera_.z};
        retained.depth = shift(dot(relative, direction_), 8) + projection_bias;
        if (retained.y < screen_max_y) {
            upper_base_ = true;
        }
        if (retained.depth > 0) {
            retained.x = dot(relative, right_) / retained.depth + screen_center_x;
            retained.y = screen_center_y - dot(relative, up_) / retained.depth;
        }
        if (detailed) {
            retained.shade = std::clamp(std::min(retained.shade, shift(2000 - retained.depth, 2)), 15, 110);
        }
        retained.visible = retained.x >= screen_min_x && retained.x <= screen_max_x && retained.depth > near_depth && retained.depth < far_depth;
        return retained;
    }

    void search_edges(MapPoint& first, MapPoint& last) {
        auto map = first;
        upper_base_ = false;
        Point projected = project(map, false, scratch_);
        int steps = 0;
        if (!projected.visible) {
            while (!projected.visible && first != last) {
                first = map;
                map = map + horizontal_;
                projected = project(map, false, projected);
                guard(++steps);
            }
        } else {
            while (projected.visible) {
                map = map - horizontal_;
                first = map;
                projected = project(map, false, projected);
                guard(++steps);
            }
        }
        map = last;
        projected = project(map, false, projected);
        if (!projected.visible) {
            while (!projected.visible && first != last) {
                last = map;
                map = map - horizontal_;
                projected = project(map, false, projected);
                guard(++steps);
            }
        } else {
            while (projected.visible) {
                map = map + horizontal_;
                last = map;
                projected = project(map, false, projected);
                guard(++steps);
            }
        }
        scratch_ = projected;
    }

    void guard(int count) const {
        if (count > maximum_search_steps) {
            throw std::runtime_error("Terrain search exceeded recovered bounds");
        }
    }

    void terrain() {
        MapPoint origin{static_cast<int>(frame_.camera_target[0] / 64), static_cast<int>(frame_.camera_target[2] / 64)};
        auto projected = project(origin, false);
        int steps = 0;
        while (projected.depth < 1200) {
            const auto side = projected.x < screen_center_x ? horizontal_ : MapPoint{-horizontal_.x, -horizontal_.z};
            origin = origin + vertical_ + side;
            projected = project(origin, false, projected);
            guard(++steps);
        }
        while (projected.depth > 1200) {
            const auto side = projected.x < screen_center_x ? horizontal_ : MapPoint{-horizontal_.x, -horizontal_.z};
            origin = origin - vertical_ + side;
            projected = project(origin, false, projected);
            guard(++steps);
        }
        origin.x &= ~1;
        origin.z &= ~1;
        MapPoint first = origin, last = origin;
        std::vector<Scanline> near, far;
        scratch_ = projected;
        do {
            search_edges(first, last);
            near.push_back({first, last});
            first = first - vertical_;
            last = last - vertical_;
            guard(static_cast<int>(near.size()));
        } while (first != last);
        vertical_ = {vertical_.x * 2, vertical_.z * 2};
        first = origin + vertical_;
        last = first;
        do {
            search_edges(first, last);
            far.push_back({first, last});
            first = first + vertical_;
            last = last + vertical_;
            guard(static_cast<int>(far.size()));
        } while (first != last);
        std::reverse(near.begin(), near.end());
        near.insert(near.end(), far.begin(), far.end());
        for (const auto& line : near) {
            draw_strip(line);
        }
    }

    std::uint8_t texel(std::size_t offset) const {
        const auto water = directory_.at(12000).offset + 0x8100;
        if ((request_.water & 1) == 0 || offset < water || offset >= water + 4096) { return bytes_.at(offset); }
        const auto index = offset - water;
        const int x = static_cast<int>(index & 63), y = static_cast<int>(index >> 6);
        return bytes_.at(water + 4096 + ((y + water_y_[x]) & 63) * 64 + ((x + water_x_[y]) & 63));
    }

    void pixel(int x, int y, int u, int v, int shade, int texture) {
        if (x < 0 || x >= static_cast<int>(image_width) || y < 0 || y >= static_cast<int>(image_height)) {
            return;
        }
        const auto& bank = directory_.at(12000);
        const auto combined = (static_cast<std::uint32_t>(texture) << 12) | (u & 4095U);
        const auto address = ((combined >> 6) & 0x1f03f) + (v & 4032);
        const auto sample = texel(bank.offset + 0x8100 + address);
        const auto filter_index = (shade & ~255) | sample;
        // Strip interpolation can retain the initial column shade. The original
        // byte lookup then aliases the palette or texture bytes in this bank.
        const auto filter_offset = std::int64_t{256} + filter_index;
        if (filter_offset < 0 || filter_offset >= bank.size) {
            throw std::runtime_error("Terrain shade outside filter bank at tick " + std::to_string(state_.tick) +
                                     ": " + std::to_string(filter_index));
        }
        frame_.pixels[y * image_width + x] = bytes_.at(bank.offset + static_cast<std::size_t>(filter_offset));
    }

    void draw_strip(Scanline line) {
        const bool low_detail = detail_ < terrain_detail_threshold;
        auto map = line.first;
        auto point = project(map, true, scratch_);
        int clipped = std::max(screen_min_x - point.x, 0);
        point.x = std::max(point.x, screen_min_x);
        int column = point.x;
        int scans = 0;
        auto& [du, dv, dt, ds, dy] = retained_->terrain_slopes;
        while (map != line.last) {
            const auto old = point;
            const auto old_map = map;
            map = map + horizontal_;
            point = project(map, true, point);
            point.x = std::max(point.x, old.x);
            const auto width = point.x - old.x;
            int y = old.y * 256;
            int u = old_map.x * 2048;
            int v = old_map.z * 2048;
            int texture = old.texture * (low_detail ? 65536 : 16384);
            int shade = old.shade * 65536;
            if (width + clipped > 1) {
                const auto divisor = 65535 / (width + clipped);
                dy = shift(static_cast<std::int64_t>(point.y - old.y) * divisor, 8);
                if (!low_detail) {
                    du = shift((map.x - old_map.x) * divisor, 5);
                    dv = shift((map.z - old_map.z) * divisor, 5);
                }
                dt = shift(static_cast<std::int64_t>(point.texture - old.texture) * divisor, low_detail ? 0 : 2);
                ds = (point.shade - old.shade) * divisor;
                if (clipped > 0) {
                    y += dy * clipped;
                    u += du * clipped;
                    v += dv * clipped;
                    texture += dt * clipped;
                    shade += ds * clipped;
                    clipped = 0;
                }
            }
            point.x = std::min(point.x, screen_max_x + 1);
            while (column < point.x) {
                if (column < 0 || column > screen_max_x) {
                    throw std::runtime_error("Terrain column outside viewport");
                }
                auto& c = columns_[column];
                const int new_texture = shift(texture, low_detail ? 8 : 11);
                const int new_shade = shift(shade + ds, low_detail ? 9 : 8);
                const int new_y = shift(y, 8);
                const int top = std::max(new_y, screen_min_y);
                if (c.top < new_y) {
                    if (c.rising) {
                        c.clips.insert(c.clips.begin(), {std::max(old.depth, point.depth), c.top - 2});
                        c.rising = false;
                    }
                } else {
                    c.rising = true;
                }
                if (top < c.top) {
                    const int old_top = c.top;
                    const auto count = c.top - top;
                    c.top = top;
                    const int distance = c.previous_y - new_y;
                    const auto slope = [&](int difference) {
                        if (distance <= 0 || distance > 4000) {
                            return 0;
                        }
                        return shift(static_cast<std::int64_t>(difference * 2) * (2147483647 / distance), 32) + (difference < 0 ? 1 : 0);
                    };
                    const bool downward = count != 1 && old_top != c.previous_y;
                    const int iu = slope(downward ? c.u - u : u - c.u);
                    const int iv = slope(downward ? c.v - v : v - c.v);
                    const int it = slope(downward ? c.texture - new_texture : new_texture - c.texture);
                    const int is = slope(downward ? c.shade - new_shade : new_shade - c.shade);
                    std::uint32_t combined = (static_cast<std::uint32_t>(downward ? new_texture : c.texture) << 12) |
                                             (static_cast<unsigned>(downward ? u : c.u) & 4095);
                    const auto delta = (static_cast<std::uint32_t>(it) << 12) | (static_cast<unsigned>(iu) & 4095);
                    auto vertical = downward ? v : c.v;
                    auto intensity = downward ? new_shade : c.shade;
                    auto low_texture = downward ? new_texture : c.texture;
                    auto cursor = downward ? c.cursor - count : c.cursor - 1;
                    // The original strip loop keeps its run counter in CL.
                    const int run = count == 256 ? 256 : count & 255;
                    for (int i = 0; i < run; ++i) {
                        if (low_detail) {
                            const auto sample = (intensity & 0xff00) | (shift(low_texture,8) & 255);
                            if (cursor >= 0 && cursor < static_cast<int>(image_height)) {
                                frame_.pixels[cursor * image_width + column] = bytes_.at(directory_.at(106).offset + sample);
                            }
                        } else { pixel(column, cursor, combined & 4095, vertical, intensity, static_cast<int>(combined >> 12)); }
                        low_texture += it;
                        combined += delta;
                        vertical += iv;
                        intensity += is;
                        cursor += downward ? 1 : -1;
                    }
                    c.cursor -= count;
                }
                c.previous_y = new_y;
                if (!low_detail) { c.u = u; c.v = v; }
                c.texture = new_texture;
                c.shade = new_shade;
                u += du;
                v += dv;
                texture += dt;
                shade += ds;
                y += dy;
                ++column;
            }
            if (++scans > maximum_search_steps) {
                throw std::runtime_error("Terrain strip exceeded recovered bounds");
            }
        }
        scratch_ = point;
    }

    int word(std::size_t offset) const {
        if (offset + 4 > bytes_.size()) {
            throw std::runtime_error("Truncated model field");
        }
        const std::uint32_t result = static_cast<std::uint32_t>(bytes_[offset]) |
            static_cast<std::uint32_t>(bytes_[offset + 1]) << 8 |
            static_cast<std::uint32_t>(bytes_[offset + 2]) << 16 |
            static_cast<std::uint32_t>(bytes_[offset + 3]) << 24;
        return result <= INT32_MAX ? static_cast<int>(result) : -1 - static_cast<int>(UINT32_MAX - result);
    }

    int short_word(std::size_t offset) const {
        const int value = bytes_.at(offset) | bytes_.at(offset + 1) << 8;
        return value < 32768 ? value : value - 65536;
    }

    void objects() {
        using Polygon = std::array<std::int32_t, 20>;
        std::list<Polygon> queue(2);
        queue.front()[0] = 9999999;
        queue.back()[0] = -9999999;
        auto cursor = queue.begin();
        const auto enqueue = [&](const Polygon& polygon) {
            bool outside = false;
            for (auto bounds : {std::array<int, 3>{10, screen_min_x, -1}, {10, screen_max_x, 1},
                                {13, screen_min_y, -1}, {13, screen_max_y, 1}}) {
                const auto test = [&](int i) { return bounds[2] < 0 ? polygon[i] < bounds[1] : polygon[i] > bounds[1]; };
                if (polygon[16] <= 2 || polygon[16] == 4) {
                    outside |= test(bounds[0]) && test(bounds[0] + 1) && test(bounds[0] + 2);
                }
            }
            if (outside || queue.size() >= 2997) {
                return;
            }
            if (polygon[0] < (*cursor)[0]) {
                while (polygon[0] < (*cursor)[0]) {
                    ++cursor;
                }
                queue.insert(cursor, polygon);
            } else {
                while ((*cursor)[0] < polygon[0]) {
                    --cursor;
                }
                queue.insert(std::next(cursor), polygon);
            }
        };
        std::map<int, const EntityState*> entities;
        for (const auto& entity : state_.entities) {
            entities.emplace(entity.slot, &entity);
        }
        const auto& player = entities.at(state_.player)->words;
        const int cell_x = shift(player[7], 17), cell_z = shift(player[9], 17);
        const auto component = [&](int field, int angle) { return field == 0 ? sine(angle) : cosine(angle); };
        for (int gz = cell_z - 3; gz < cell_z + 4; ++gz) {
            for (int gx = cell_x - 3; gx < cell_x + 4; ++gx) {
                auto slot = state_.grid[(gz & 63) * 64 + (gx & 63)];
                while (slot != no_entity) {
                    const auto actor_slot = slot;
                    const auto& w = entities.at(slot)->words;
                    slot = w[0];
                    if (w[19] <= 0) {
                        continue;
                    }
                    const IntVector origin{shift(w[7], 8) + (gx < 0 ? -32768 : gx >= 64 ? 32768 : 0),
                                           shift(w[8], 8),
                                           shift(w[9], 8) + (gz < 0 ? -32768 : gz >= 64 ? 32768 : 0)};
                    const IntVector relative{origin.x - camera_.x, origin.y - camera_.y, origin.z - camera_.z};
                    const int center_depth = shift(dot(relative, direction_), 8);
                    if (w[19] < 10000 && (w[1] & 0x400) != 0) {
                        Polygon line{};
                        const auto& parent = entities.at(w[23])->words;
                        const IntVector end{shift(parent[7], 8) - camera_.x, shift(parent[8], 8) - camera_.y,
                                            shift(parent[9], 8) - camera_.z};
                        int index = 0;
                        for (const auto point : {relative, end}) {
                            const int depth = shift(dot(point, direction_), 8) + projection_bias;
                            line[0] += depth;
                            line[10 + index] = depth < 1 ? screen_max_x : screen_center_x + dot(point, right_) / depth;
                            line[13 + index] = depth < 1 ? screen_max_y : screen_center_y - dot(point, up_) / depth;
                            ++index;
                        }
                        line[0] = shift(line[0], 1);
                        line[16] = 42;
                        enqueue(line);
                    }
                    if (w[19] >= 10000) {
                        const IntVector sprite_relative{relative.x,effect_height(origin.x,origin.y,origin.z) - camera_.y,relative.z};
                        const int depth = shift(dot(sprite_relative,direction_),8) + projection_bias;
                        if (depth <= 0) { continue; }
                        Polygon polygon{};
                        polygon[0] = depth;
                        polygon[10] = screen_center_x + dot(sprite_relative, right_) / depth;
                        polygon[13] = screen_center_y - dot(sprite_relative, up_) / depth;
                        if (w[19] < 11000) {
                            const int size = std::clamp(projection_width_ * 21 / depth - 5, 0, 15);
                            polygon[16] = 45;
                            polygon[3] = ((w[19] - 10000) * 16 + 15 - size) * 735;
                        } else {
                            constexpr int sizes[]{15,10,35,40,23};
                            const auto size = sizes[std::clamp(w[19] - 11000, 0, 4)];
                            polygon[11] = projection_width_ * size / depth;
                            polygon[14] = (projection_width_ * 5 / 3) * size / depth;
                            polygon[16] = 46;
                            polygon[3] = w[20] * 4096;
                        }
                        enqueue(polygon);
                        continue;
                    }
                    if (w[19] >= 1000) { throw std::runtime_error("Unsupported mission model"); }
                    if (center_depth <= -10 || center_depth >= far_depth) {
                        continue;
                    }
                    const int center_x = dot(relative, right_) / (center_depth + projection_bias);
                    const int center_y = dot(relative, up_) / (center_depth + projection_bias);
                    const int horizontal_limit = projection_width_ + 20;
                    const int vertical_limit = screen_max_y - screen_min_y + 30;
                    if (center_x <= -horizontal_limit || center_x >= horizontal_limit || center_y <= -vertical_limit || center_y >= vertical_limit) {
                        continue;
                    }
                    if (player[22] != state_.player) {
                        const std::array<int, 3> marker{screen_center_x + center_x, screen_center_y - center_y,
                                                       std::clamp(shift(center_depth, 6) - 11, 0, 15)};
                        if (actor_slot == player[22]) { target_ = marker; }
                        if (!state_.objective_targets.empty() && actor_slot == state_.objective_targets.front().entity) { objective_ = marker; }
                    }
                    const auto model = state_.models[w[19]];
                    const int vertex_count = word(model), face_count = word(model + 4), normal_count = word(model + 8);
                    if (vertex_count <= 0 || vertex_count > 4096 || face_count < 0 || face_count > 4096 || normal_count < 0 || normal_count > 4096) {
                        throw std::runtime_error("Unsupported model dimensions");
                    }
                    const int sin_pitch = sine(-w[14]), cos_pitch = cosine(-w[14]);
                    const int sin_roll = sine(-w[15]), cos_roll = cosine(-w[15]);
                    int sin_yaw = sine(-w[13]), cos_yaw = cosine(-w[13]);
                    if ((w[1] & 128) != 0) {
                        sin_yaw = -horizontal_right_.x;
                        cos_yaw = horizontal_right_.z;
                    }
                    std::vector<Point> vertices;
                    for (int v = 0; v < vertex_count; ++v) {
                        const auto address = model + 32 + v * 16;
                        int x = word(address), y = word(address + 4), z = word(address + 8);
                        const int mode = word(address + 12);
                        if (mode != 1) {
                            const int pitched_y = shift(y * cos_pitch - z * sin_pitch, 8);
                            z = shift(z * cos_pitch + y * sin_pitch, 8);
                            const int rolled_x = shift(x * cos_roll - pitched_y * sin_roll, 8);
                            y = shift(pitched_y * cos_roll + x * sin_roll, 8);
                            x = rolled_x;
                        }
                        const auto world_x = shift(x * cos_yaw - z * sin_yaw, 8) + origin.x;
                        const auto world_z = shift(z * cos_yaw + x * sin_yaw, 8) + origin.z;
                        y += origin.y;
                        if (mode != 0) {
                            const auto floor = shift(surface_height_(world_x * 256, world_z * 256), 8);
                            if (mode == 1 || y < floor) {
                                y = floor;
                            }
                        }
                        y = effect_height(world_x,y,world_z);
                        const IntVector position{world_x - camera_.x, y - camera_.y, world_z - camera_.z};
                        Point point;
                        point.depth = shift(dot(position, direction_), 8) + projection_bias;
                        point.x = point.depth < 1 ? screen_max_x : screen_center_x + dot(position, right_) / point.depth;
                        point.y = point.depth < 1 ? screen_max_y : screen_center_y - dot(position, up_) / point.depth;
                        vertices.push_back(point);
                    }
                    int lx = 147, ly = 147, lz = 147;
                    const int pitch_y = shift(ly * cosine(w[14]) - lz * sine(w[14]), 8);
                    lz = shift(lz * cosine(w[14]) + ly * sine(w[14]), 8);
                    const int roll_x = shift(lx * cosine320(w[15]) - pitch_y * sine320(w[15]), 8);
                    ly = shift(sine320(w[15]) * lx + cosine320(w[15]) * pitch_y, 8);
                    lx = shift(roll_x * cosine(w[13]) - lz * sine(w[13]), 8);
                    lz = shift(sine(w[13]) * roll_x + cosine(w[13]) * lz, 8);
                    std::vector<int> shades;
                    const bool hit = (w[1] & 0x20000) != 0;
                    for (int normal = 0; normal < normal_count; ++normal) {
                        const auto address = model + word(model + 28) + normal * 12;
                        shades.push_back(hit ? 127 : std::min(std::max(shift(word(address) * lx + word(address + 4) * ly + word(address + 8) * lz, 8), 0) + word(model + 20), 110));
                    }
                    if (hit) { frame_.consumed_hits.push_back(actor_slot); }
                    int min_depth = INT32_MAX, max_depth = INT32_MIN;
                    for (const auto& v : vertices) {
                        min_depth = std::min(min_depth, v.depth);
                        max_depth = std::max(max_depth, v.depth);
                    }
                    for (int face = 0; face < face_count; ++face) {
                        const auto address = model + word(model + 24) + face * 42;
                        Polygon polygon{};
                        polygon[3] = word(address + 24);
                        polygon[16] = bytes_.at(address + 40);
                        const auto animation = (w[1] & 0x40000) != 0 ? state_.landscape_clock.water : state_.landscape_clock.water >> 2;
                        const int texture_scroll = (bytes_.at(address + 41) & 2) != 0 && (w[1] & 0x10000) == 0 ?
                            static_cast<int>(animation & 3) * 16 : 0;
                        if (polygon[16] == 5) { polygon[3] = w[20]; }
                        for (int i = 0; i < 3; ++i) {
                            const auto& v = vertices.at(word(address + i * 4));
                            polygon[0] = std::max(polygon[0], v.depth);
                            polygon[4 + i] = short_word(address + 28 + i * 2);
                            polygon[7 + i] = short_word(address + 34 + i * 2) + texture_scroll;
                            polygon[10 + i] = v.x;
                            polygon[13 + i] = v.y;
                            if (polygon[16] == 0 || polygon[16] == 2 || polygon[16] == 4) {
                                polygon[17 + i] = shades.at(word(address + 12 + i * 4));
                            }
                        }
                        const auto area = (polygon[11] - polygon[10]) * (polygon[15] - polygon[13]) -
                                          (polygon[12] - polygon[10]) * (polygon[14] - polygon[13]);
                        if ((polygon[16] == 0 || polygon[16] == 40) && area <= 0) {
                            continue;
                        }
                        if (polygon[16] == 40) {
                            polygon[0] = max_depth;
                        }
                        if (polygon[16] == 5) { polygon[0] = min_depth; }
                        if (polygon[16] == 1) {
                            polygon[0] = area > 0 ? max_depth : min_depth;
                            const auto phase = static_cast<int>(state_.landscape_clock.rotor & rotor_phase_mask);
                            for (int i = 0; i < 3; ++i) {
                                constexpr int offsets[]{0, 85, 171};
                                polygon[4 + i] = shift(component(0, (phase + offsets[i]) * 2) * 36, 8) + 32;
                                polygon[7 + i] = shift(component(1, (phase + offsets[i]) * 2) * 36, 8) + 32;
                            }
                        }
                        enqueue(polygon);
                    }
                }
            }
        }
        for (auto it = std::next(queue.begin()); it != std::prev(queue.end()); ++it) {
            frame_.polygons.push_back(*it);
            if ((*it)[16] == 45 || (*it)[16] == 46) { sprite(*it); }
            else if ((*it)[16] == 42) { line(*it); }
            else { triangle(*it); }
        }
    }

    void line(const std::array<std::int32_t, 20>& polygon) {
        const int first = polygon[11] < polygon[10] ? 1 : 0, second = 1 - first;
        const int left = polygon[10 + first], right = std::min(polygon[10 + second], screen_max_x + 1);
        if (left >= screen_max_x + 1 || right <= screen_min_x || left > right) { return; }
        int y = polygon[13 + first] * 256;
        const int step = (polygon[13 + second] * 256 - y) / (polygon[10 + second] - left + 1);
        int previous = shift(y, 8);
        for (int x = left; x <= right; ++x) {
            y += step;
            if (x < screen_min_x) { continue; }
            const int current = shift(y, 8);
            int top = std::min(previous, current), bottom = std::max(previous, current);
            const int minimum_y = screen_min_y + 1;
            const int maximum_y = screen_max_y - 1;
            if (bottom < 0 || top >= static_cast<int>(image_height) || top >= bottom) { continue; }
            int count = bottom - top + 1;
            if (top < minimum_y) { count += top - minimum_y; top = minimum_y; }
            if (bottom > maximum_y) { count -= bottom - maximum_y; bottom = maximum_y; }
            for (const auto clip : columns_[x].clips) {
                if (clip.first >= polygon[0]) { continue; }
                if (clip.second < top) { count = 0; }
                if (clip.second < bottom) { count -= bottom - clip.second; }
                bottom = clip.second;
            }
            for (int row = 0; row < count; ++row) {
                const auto destination = static_cast<std::size_t>(top + row) * image_width + x;
                if (destination < frame_.pixels.size()) { frame_.pixels[destination] = state_.line_colour; }
            }
            previous = current;
        }
    }

    void sprite(const std::array<std::int32_t, 20>& polygon) {
        if (sprites_.empty() || flame_.empty()) { return; }
        const bool scaled_sprite = polygon[16] == 46;
        const int half_width = scaled_sprite ? polygon[11] : 10;
        const int half_height = scaled_sprite ? polygon[14] : 17;
        if (half_height == 0) { return; }
        const int left = polygon[10] - half_width, top = polygon[13] - half_height;
        const int right = std::min(polygon[10] + half_width, screen_max_x);
        const int bottom = polygon[13] + half_height;
        const int dx = 0x3fff / (half_width * 2 + 1), dy = 0x3f00 / (half_height * 2 + 1);
        int u = 0;
        for (int x = left; x <= right; ++x) {
            u += dx;
            if (x < screen_min_x) { continue; }
            int y1 = std::max(top, screen_min_y + 1), y2 = std::min(bottom, screen_max_y - 1);
            for (auto clip : columns_[x].clips) {
                if (clip.first >= polygon[0]) { continue; }
                if (clip.second < y1) { y2 = y1 - 1; }
                y2 = std::min(y2, clip.second);
            }
            int v = dy * (y1 - top);
            for (int y = y1; y <= y2; ++y) {
                const auto address = polygon[3] + (scaled_sprite ? (u >> 8) + (v >> 8) * 64 : (x - left) * 35 + y - top);
                const auto code = scaled_sprite ? flame_.at(address) : sprites_.at(address);
                auto& destination = frame_.pixels[y * image_width + x];
                destination = bytes_.at(directory_.at(109).offset + code * 256 + destination);
                v += dy;
            }
        }
    }

    void triangle(const std::array<std::int32_t, 20>& polygon) {
        const auto type = detail_ < smooth_model_detail_threshold &&
                          (polygon[16] == 0 || polygon[16] == 2 || polygon[16] == 4) ? 0 : polygon[16];
        int first = 0, last = 0;
        for (int i = 1; i < 3; ++i) {
            if (polygon[10 + i] <= polygon[10 + first]) {
                first = i;
            }
            if (polygon[10 + i] > polygon[10 + last]) {
                last = i;
            }
        }
        int second = (first + 1) % 3, third = (first + 2) % 3;
        const auto distance = [&](int a, int b) { return std::abs(polygon[10 + a] - polygon[10 + b]) + 1; };
        const auto scale = [&](int field) {
            if (field == 4 || field == 7) {
                if (type == 40) {
                    return 65536;
                }
                if (type == 1 || type == 5) {
                    return 32768;
                }
            }
            return 256;
        };
        const auto slope = [&](int a, int b, int field) { return (polygon[field + b] - polygon[field + a]) * scale(field) / distance(a, b); };
        if (slope(first, second, 13) < slope(first, third, 13)) {
            std::swap(second, third);
        }
        std::array<int, 4> top{}, bottom{}, top_step{}, bottom_step{}, middle_step{};
        constexpr int fields[]{13, 4, 7, 17};
        for (int i = 0; i < 4; ++i) {
            top[i] = bottom[i] = polygon[fields[i] + first] * scale(fields[i]);
            top_step[i] = slope(first, third, fields[i]);
            bottom_step[i] = slope(first, second, fields[i]);
            middle_step[i] = slope(second, third, fields[i]);
        }
        // Filtered rotors and shadows include column 320, aliasing column zero
        // of the next row in the original planar buffer.
        const int triangle_max_x = screen_max_x + (type == 1 || type == 40 ? 1 : 0);
        for (int x = polygon[10 + first]; x <= std::min(polygon[10 + last], triangle_max_x); ++x) {
            for (int i = 0; i < 4; ++i) {
                top[i] += top_step[i];
                bottom[i] += bottom_step[i];
            }
            if (x == polygon[10 + second]) {
                bottom_step = middle_step;
            }
            if (x == polygon[10 + third]) {
                for (int i = 0; i < 4; ++i) {
                    top_step[i] = -middle_step[i];
                }
            }
            if (x < screen_min_x || x > triangle_max_x) {
                continue;
            }
            int y1 = shift(top[0], 8), y2 = shift(bottom[0], 8);
            if (y1 >= y2 || (y1 < screen_min_y + 1 && y2 < screen_min_y + 1) || (y1 > screen_max_y - 1 && y2 >= screen_max_y - 1)) {
                continue;
            }
            int count = y2 + 2 - y1;
            const unsigned mapping_shift = type == 40 ? 8 : type == 1 || type == 5 ? 10 : 0;
            const int mapped_u1 = shift(top[1], mapping_shift);
            const int mapped_v1 = shift(top[2], mapping_shift);
            const int mapped_u2 = shift(bottom[1], mapping_shift);
            const int mapped_v2 = shift(bottom[2], mapping_shift);
            const int du = (mapped_u2 - mapped_u1) / count;
            const int dv = (mapped_v2 - mapped_v1) / count;
            const int ds = (bottom[3] - top[3]) / count;
            std::uint32_t uv = (static_cast<std::uint32_t>(mapped_v1) << 16) + mapped_u1;
            const std::uint32_t uv_step = (static_cast<std::uint32_t>(dv) << 16) + du;
            int shade = top[3];
            int separate_u = mapped_u1, separate_v = mapped_v1;
            std::uint32_t rotor = static_cast<std::uint32_t>(mapped_v1 * 2048 + mapped_u1);
            const auto rotor_step = static_cast<std::uint32_t>(dv * 2048 + du);
            if (y1 < screen_min_y + 1) {
                const int clipped = screen_min_y + 1 - y1;
                count -= clipped;
                uv += uv_step * clipped;
                shade += ds * clipped;
                separate_u += du * clipped;
                separate_v += dv * clipped;
                rotor += rotor_step * clipped;
                y1 = screen_min_y + 1;
            }
            if (y2 > screen_max_y - 1) {
                count -= y2 - (screen_max_y - 1);
                y2 = screen_max_y - 1;
            }
            for (auto clip : columns_[x].clips) {
                if (clip.first >= polygon[0]) {
                    continue;
                }
                if (clip.second < y1) {
                    count = 0;
                }
                if (clip.second < y2) {
                    count -= y2 - clip.second;
                }
                y2 = clip.second;
            }
            if (type != 0 && type != 1 && type != 2 && type != 4 && type != 5 && type != 40) {
                throw std::runtime_error("Unsupported polygon shader " + std::to_string(type) + " at mission tick " + std::to_string(state_.tick));
            }
            for (int i = 0; i < count && y1 + i < static_cast<int>(image_height); ++i) {
                if ((y1 + i) * image_width + x >= frame_.pixels.size()) { break; }
                auto& destination = frame_.pixels[(y1 + i) * image_width + x];
                const auto sample = ((uv >> 16) & 0x3f00) | ((uv >> 8) & 63);
                if (type == 40) {
                    if (detail_ < textured_shadow_detail_threshold) { destination = 0; }
                    else {
                        const auto address = (shift(separate_v, 8) & 255) * 256 + (shift(separate_u, 8) & 255);
                        const auto mask = bytes_.at(directory_.at(110).offset + address);
                        destination = bytes_.at(directory_.at(500).offset + mask * 256 + destination);
                    }
                } else if (type == 1 || type == 5) {
                    const auto packed = (rotor & 0xffff0000U) | ((rotor << 5) & 65535);
                    const auto mask = type == 1 ? bytes_.at(directory_.at(500).offset + 8192 + ((packed >> 10) & 4095)) :
                        flame_.at(polygon[3] * 4096 + ((packed >> 10) & 4095));
                    destination = bytes_.at(directory_.at(type == 1 ? 500 : 109).offset + mask * 256 + destination);
                } else {
                    const bool flat = detail_ < smooth_model_detail_threshold || (polygon[17] == polygon[18] && polygon[17] == polygon[19]);
                    const auto address = type == 4 || flat ?
                        ((shift(separate_v, 8) & 63) * 256 + (shift(separate_u, 8) & 63)) : static_cast<int>(sample);
                    const auto sample = texel(state_.textures.at(polygon[3]) + address);
                    if (type != 4 || sample != 0) {
                        const auto intensity = type == 4 ? 100 : detail_ < smooth_model_detail_threshold ? polygon[17] : std::clamp(shift(shade, 8), 0, 127);
                        destination = bytes_.at(state_.filters.at(polygon[3]) + intensity * 256 + sample);
                    }
                }
                uv += uv_step;
                separate_u += du;
                separate_v += dv;
                rotor += rotor_step;
                shade += ds;
            }
        }
    }

    const MissionState& state_;
    const FrontendState& request_;
    const std::vector<std::uint8_t>& bytes_;
    const std::map<std::uint32_t, Resource>& directory_;
    const std::function<int(int, int)>& height_;
    const std::function<int(int, int)>& surface_height_;
    MissionFrame frame_;
    int screen_min_x = 0, screen_max_x = 319, screen_min_y = -1, screen_max_y = 399;
    int projection_width_ = 319, detail_ = 240;
    IntVector camera_{}, direction_{}, right_{}, up_{}, horizontal_right_{};
    Vector forward_{};
    MapPoint horizontal_{}, vertical_{};
    std::array<Column, image_width + 1> columns_{};
    std::array<int,64> water_x_{}, water_y_{};
    Point scratch_{};
    bool upper_base_ = false;
    std::vector<std::uint8_t> flame_, sprites_, hud_;
    Camera local_camera_;
    Camera* retained_ = nullptr;
    std::array<int, 3> target_{0, 0, -1}, objective_{0, 0, -1};
};

}

MissionFrame render_mission(const MissionState& state, const FrontendState& request,
                           const std::vector<std::uint8_t>& bytes, const std::map<std::uint32_t, Resource>& directory,
                           const std::function<std::int32_t(std::int32_t, std::int32_t)>& height, const Archive* globals,
                           Camera* camera, RenderTiming timing,
                           const std::function<std::int32_t(std::int32_t, std::int32_t)>& surface_height) {
    return Renderer(state, request, bytes, directory, height, globals, camera, timing, surface_height).render();
}

}
