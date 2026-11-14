#include "game_text.hpp"
#include "combat_math.hpp"

#include <list>
#include <stdexcept>

namespace swiv3d::detail {
namespace {
using namespace combat_math;
constexpr int message_capacity = 50;
constexpr int held_lifetime = 1000;
constexpr int glyph_record_bytes = 28;
using Polygon = std::array<std::int32_t, 20>;

std::size_t lookup(const std::vector<std::uint8_t>& bytes, std::size_t table, std::uint32_t key) {
    for (auto offset = table; offset + 8 <= bytes.size(); offset += 8) {
        const auto candidate = static_cast<std::uint32_t>(word(bytes,offset));
        if (candidate == key) { return static_cast<std::uint32_t>(word(bytes,offset + 4)); }
        if (candidate == UINT32_MAX) { break; }
    }
    throw std::runtime_error("Missing game message string");
}

void fill(MissionFrame& frame, const Polygon& polygon, std::uint8_t colour) {
    int first = 0, last = 0;
    for (int i = 1; i < 3; ++i) {
        if (polygon[10 + i] <= polygon[10 + first]) { first = i; }
        if (polygon[10 + i] > polygon[10 + last]) { last = i; }
    }
    int second = (first + 1) % 3, third = (first + 2) % 3;
    const auto slope = [&](int a,int b) {
        return (polygon[13 + b] - polygon[13 + a]) * 256 / (std::abs(polygon[10 + b] - polygon[10 + a]) + 1);
    };
    if (slope(first,second) < slope(first,third)) { std::swap(second,third); }
    int top = polygon[13 + first] * 256, bottom = top;
    int top_step = slope(first,third), bottom_step = slope(first,second);
    const int middle_step = slope(second,third);
    for (int x = polygon[10 + first]; x <= std::min(polygon[10 + last],319); ++x) {
        top += top_step;
        bottom += bottom_step;
        if (x == polygon[10 + second]) { bottom_step = middle_step; }
        if (x == polygon[10 + third]) { top_step = -middle_step; }
        if (x < 0) { continue; }
        const int y1 = shift(top,8), y2 = shift(bottom,8);
        if (y1 >= y2) { continue; }
        for (int y = std::max(y1,0); y <= std::min(y2,399); ++y) { frame.pixels[y * image_width + x] = colour; }
    }
}
}

void cue_message(MissionState& state, std::uint32_t group, std::uint32_t id, int colour, MessageMotion motion) {
    if (state.messages.size() == message_capacity) { return; }
    GameMessage message;
    message.group = group; message.id = id; message.colour = colour;
    int lead = 0;
    if (motion == MessageMotion::flying) {
        message.position = {0,-40000,0};
        message.velocity = {6400,-3000,7000};
        message.angles = {-480,240,160};
        message.spin = {32,63,44};
        message.lifetime = 70;
        lead = 60;
    } else {
        message.velocity = {motion == MessageMotion::held ? -6400 : 6400,motion == MessageMotion::held ? -3000 : -5000,
                            motion == MessageMotion::held ? 7000 : 8000};
        message.angles = {300,0,0};
        message.spin = {0,45,0};
        message.lifetime = motion == MessageMotion::held ? 1080 : 1031;
        message.screen_y = -140;
        lead = motion == MessageMotion::held ? 80 : 30;
    }
    for (int axis = 0; axis < 3; ++axis) {
        message.position[axis] -= message.velocity[axis] * lead;
        message.angles[axis] -= message.spin[axis] * lead;
    }
    state.messages.insert(state.messages.begin(),message);
}

void advance_messages(MissionState& state) {
    for (auto& message : state.messages) {
        if (message.lifetime != held_lifetime) { --message.lifetime; }
        if (message.lifetime < 0) { continue; }
        for (int axis = 0; axis < 3; ++axis) {
            if (message.lifetime > 10) { message.angles[axis] += message.spin[axis]; }
            if (message.lifetime != held_lifetime) { message.position[axis] += message.velocity[axis]; }
        }
    }
    state.messages.erase(std::remove_if(state.messages.begin(),state.messages.end(),[](const auto& message) {
        return message.lifetime < 0;
    }),state.messages.end());
    std::reverse(state.messages.begin(),state.messages.end());
}

void draw_messages(MissionFrame& frame, const MissionState& state, const FrontendState& request,
                   const Archive& globals, const std::vector<std::uint8_t>& palette) {
    if (state.messages.empty()) { return; }
    const auto font = globals.load(200), strings = globals.load(100);
    const auto page = lookup(strings,lookup(strings,0,0x630002),0);
    const int character_page = strings.at(page + request.language) - '0';
    const auto glyph = [&](unsigned char character) {
        return character == 127 ? 0U : character < 128 ? unsigned(character) : unsigned(character + character_page * 128);
    };
    std::list<Polygon> queue(2);
    queue.front()[0] = 9999999; queue.back()[0] = -9999999;
    auto cursor = queue.begin();
    const auto enqueue = [&](const Polygon& polygon) {
        for (auto bound : {std::array<int,3>{10,0,-1},{10,318,1},{13,0,-1},{13,399,1}}) {
            const auto outside = [&](int vertex) {
                return bound[2] < 0 ? polygon[bound[0] + vertex] < bound[1] : polygon[bound[0] + vertex] > bound[1];
            };
            if (outside(0) && outside(1) && outside(2)) { return; }
        }
        if (queue.size() >= 2997) { return; }
        if (polygon[0] < (*cursor)[0]) {
            while (polygon[0] < (*cursor)[0]) { ++cursor; }
            queue.insert(cursor,polygon);
        } else {
            while ((*cursor)[0] < polygon[0]) { --cursor; }
            queue.insert(std::next(cursor),polygon);
        }
    };
    for (const auto& message : state.messages) {
        const auto text = lookup(strings,lookup(strings,0,message.group + (request.language << 16)),message.id);
        auto end = text;
        int line_y = 0;
        while (strings.at(end) != 0) { if (strings.at(end++) == '|') { line_y += 150; } }
        const int rx = shift(message.angles[0],4), ry = shift(message.angles[1],4), rz = shift(message.angles[2],4);
        const int sx = sine(rx), cx = cosine(rx), sy = sine(ry), cy = cosine(ry), sz = sine(rz), cz = cosine(rz);
        auto start = text;
        while (start < end) {
            auto stop = start;
            int left = 0;
            while (stop < end && strings.at(stop) != '|') {
                left -= shift(word(font,glyph(strings.at(stop++)) * glyph_record_bytes),2) + 5;
            }
            for (auto character = start; character < stop; ++character) {
                const auto record = glyph(strings.at(character)) * glyph_record_bytes;
                const int width = shift(word(font,record),2) + 5;
                left += width;
                const int char_x = left * 4;
                left += width;
                const int vertex_count = word(font,record + 8), face_count = word(font,record + 16);
                if (face_count == 0) { continue; }
                const auto vertex_offset = static_cast<std::uint32_t>(word(font,record + 4));
                const auto face_offset = static_cast<std::uint32_t>(word(font,record + 12));
                const auto normal_offset = static_cast<std::uint32_t>(word(font,record + 20));
                std::vector<std::array<int,3>> points;
                for (int i = 0; i < vertex_count; ++i) {
                    const auto offset = vertex_offset + i * 12;
                    const int x = (word(font,offset) + char_x) * 4, y = (word(font,offset + 4) + line_y) * 4, z = word(font,offset + 8) * 4;
                    const int yz = shift(z * cx + y * sx,8), xx = shift(cy * x - sy * yz,8), yy = shift(cx * y - sx * z,8);
                    const int depth = shift(yz * cy + x * sy - message.position[2] * 4,9) + 3425;
                    if (depth < 1) { points.clear(); break; }
                    const int screen_x = (shift(cz * xx - sz * yy,2) - message.position[0] * 4) / depth + 160;
                    const int screen_y = 200 - (shift((std::int64_t(yy) * cz + xx * sz) * 426,10) - message.position[1] * 4) / depth + message.screen_y;
                    points.push_back({screen_x,screen_y,depth});
                }
                if (points.empty()) { continue; }
                for (int face = 0; face < face_count; ++face) {
                    const auto offset = face_offset + face * 16;
                    Polygon polygon{};
                    polygon[16] = 100;
                    for (int vertex = 0; vertex < 3; ++vertex) {
                        const auto point = points.at(word(font,offset + vertex * 4));
                        polygon[10 + vertex] = point[0]; polygon[13 + vertex] = point[1]; polygon[0] += point[2];
                    }
                    const auto normal = normal_offset + word(font,offset + 12) * 12;
                    const int nx = word(font,normal), ny = word(font,normal + 4), nz = word(font,normal + 8);
                    const int light_yaw = ry + shift(char_x,6);
                    const int facing = shift(shift(nz * cx + ny * sx,8) * cosine(light_yaw) + nx * sine(light_yaw),8);
                    polygon[17] = message.colour + std::clamp(shift(facing * -256,9),0,127);
                    const auto area = (polygon[11] - polygon[10]) * (polygon[15] - polygon[13]) -
                                      (polygon[14] - polygon[13]) * (polygon[12] - polygon[10]);
                    if (area > 0) { enqueue(polygon); }
                }
            }
            start = stop + 1;
            line_y -= 300;
        }
    }
    for (auto it = std::next(queue.begin()); it != std::prev(queue.end()); ++it) {
        frame.polygons.push_back(*it);
        fill(frame,*it,palette.at(palette_bytes + (*it)[17]));
    }
}
}
