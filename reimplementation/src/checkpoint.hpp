#pragma once

#include "swiv3d/mission.hpp"

#include <fstream>
#include <stdexcept>

namespace swiv3d::detail {

inline void checkpoint(const swiv3d::Mission& mission, const swiv3d::MissionFrame& frame,
                const std::filesystem::path& output) {
    std::filesystem::create_directory(output);
    const auto state = mission.state();
    std::array<const swiv3d::EntityState*, swiv3d::object_slots + 1> entities{};
    std::array<bool, swiv3d::object_slots + 1> linked{};
    for (const auto& actor : state.entities) { entities.at(actor.slot) = &actor; }
    for (auto slot : state.grid) {
        while (slot != swiv3d::no_entity) {
            if (linked.at(slot) || entities.at(slot) == nullptr) { throw std::runtime_error("Invalid mission grid checkpoint"); }
            linked.at(slot) = true;
            slot = entities.at(slot)->words[0];
        }
    }
    std::ofstream json(output / "state.json");
    std::ofstream bounds(output / "model-bounds.bin",std::ios::binary);
    for (unsigned model = 0; model < state.models.size(); ++model) {
        const auto bytes = mission.resource(10000 + model);
        if (bytes.size() < 16) { throw std::runtime_error("Missing model bounds"); }
        bounds.write(reinterpret_cast<const char*>(bytes.data() + 12),4);
    }
    if (!bounds) { throw std::runtime_error("Cannot write model bounds checkpoint"); }
    for (const auto& map : {std::pair<unsigned, const char*>{100,"height-map.bin"}, {102,"texture-map.bin"}}) {
        const auto bytes = mission.resource(map.first);
        std::ofstream stream(output / map.second, std::ios::binary);
        stream.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
        if (!stream) { throw std::runtime_error("Cannot write terrain checkpoint"); }
    }
    json << "{\"tick\":" << state.tick << ",\"landscape_clock\":{\"rotor\":" << state.landscape_clock.rotor
         << ",\"water\":" << state.landscape_clock.water << "},\"control_ticks\":" << state.flight.control_ticks
         << ",\"previous_heading\":" << state.flight.previous_heading << ",\"engine_pitch\":" << state.flight.engine_pitch
         << ",\"ground_impacts\":" << state.flight.ground_impacts;
    const auto array = [&](const char* name, const auto& values) {
        json << ",\"" << name << "\":[";
        const char* separator = "";
        for (auto value : values) {
            json << separator << value;
            separator = ",";
        }
        json << ']';
    };
    array("direction_steps", state.direction_steps);
    json << ",\"ground_effects\":{\"active\":" << state.ground_effects.active
         << ",\"free\":" << state.ground_effects.free << ",\"records\":[";
    for (std::size_t slot = 0; slot < state.ground_effects.records.size(); ++slot) {
        if (slot != 0) { json << ','; }
        const auto& effect = state.ground_effects.records[slot];
        json << '[' << effect.position[0] << ',' << effect.position[1] << ',' << effect.position[2] << ','
             << effect.lifetime << ',' << static_cast<int>(effect.kind) << ',' << effect.next << ']';
    }
    json << "]}";
    array("boss_state",std::array<int,2>{state.boss_destroyed,state.boss_volley});
    json << ",\"node_range\":" << state.node_range;
    array("object_direction", state.object_direction);
    json << ",\"messages\":[";
    const char* message_separator = "";
    for (const auto& message : state.messages) {
        json << message_separator << "{\"group\":" << message.group << ",\"id\":" << message.id << ",\"colour\":" << message.colour
             << ",\"lifetime\":" << message.lifetime << ",\"screen_y\":" << message.screen_y;
        array("position",message.position); array("velocity",message.velocity);
        array("angles",message.angles); array("spin",message.spin);
        json << '}';
        message_separator = ",";
    }
    json << ']';
    json << ",\"maximum_height\":" << state.flight.maximum_height;
    json << ",\"craft\":" << static_cast<int>(state.flight.craft)
         << ",\"campaign_set\":" << state.craft_set << ",\"water_flags\":" << state.water_flags
         << ",\"player_control\":" << static_cast<int>(state.flight.control)
         << ",\"weapon_hold\":" << (state.flight.weapon_hold ? "true" : "false");
    json << ",\"unassigned_air_returns\":" << state.unassigned_air_returns;
    array("ground_control", std::array<int,4>{state.flight.power, state.flight.steering_force,
                                             state.flight.wheel_direction, state.flight.wheel_target});
    array("accumulators", state.flight.attitude);
    array("bindings", state.flight.bindings);
    array("grid", state.grid);
    array("lighting", state.lighting);
    json << ",\"mission\":{\"phase\":" << static_cast<int>(state.phase)
         << ",\"finish\":" << static_cast<int>(state.finish) << ",\"counter\":" << state.finish_counter
         << ",\"objectives_completed\":" << state.objectives_completed << ",\"objective_state\":" << state.objective_state
         << ",\"last_objective\":" << state.last_objective << ",\"extracting\":" << state.extracting
         << ",\"objectives_total\":" << state.objectives << ",\"targets\":[";
    const char* objective_separator = "";
    for (const auto& objective : state.objective_targets) {
        json << objective_separator << '[' << objective.entity << ',' << objective.priority << ']';
        objective_separator = ",";
    }
    json << "]}";
    json << ",\"weapons\":{\"primary_level\":" << state.weapons.primary_level
         << ",\"primary_held\":" << state.weapons.primary_held
         << ",\"selected\":" << state.weapons.selected << ",\"requested\":" << state.weapons.requested
         << ",\"phase\":" << state.weapons.phase << ",\"progress\":" << state.weapons.progress
         << ",\"shield\":" << state.weapons.shield
         << ",\"collected\":" << state.weapons.collected << ",\"secrets\":" << state.weapons.secrets;
    array("ammunition", state.weapons.ammunition);
    array("mounts", state.weapons.mounts);
    array("buddies", state.weapons.buddies);
    json << '}';
    for (const auto& entity : state.entities) {
        if (entity.slot == state.player) {
            array("player", entity.words);
            break;
        }
    }
    json << ",\"objective_heading\":" << state.objective_heading << ",\"shots\":" << state.shots << ",\"kills\":" << state.kills
         << ",\"effects_phase\":" << state.effects_phase << ",\"gun_pitch\":" << state.gun_pitch
         << ",\"free_slot\":" << state.free_slot << ",\"score\":" << state.score << ",\"display_score\":" << state.display_score
         << ",\"health_bar\":" << state.health_bar << ",\"game_counter\":" << state.game_counter << ",\"entities\":[";
    const char* separator = "";
    for (const auto& entity : state.entities) {
        if (!linked.at(entity.slot)) { continue; }
        json << separator << "{\"slot\":" << entity.slot << ",\"behavior\":\""
             << swiv3d::behavior_name(static_cast<swiv3d::Behavior>(entity.words[26])) << "\"";
        array("words", entity.words);
        json << '}';
        separator = ",";
    }
    json << "]}\n";
    std::ofstream pixels(output / "frame-indexed.bin", std::ios::binary);
    pixels.write(reinterpret_cast<const char*>(frame.pixels.data()), static_cast<std::streamsize>(frame.pixels.size()));
    std::ofstream preview(output / "frame.ppm", std::ios::binary);
    constexpr std::size_t width = 640, height = 480;
    preview << "P6\n" << width << ' ' << height << "\n255\n";
    for (std::size_t y = 0; y < height; ++y) {
        for (std::size_t x = 0; x < width; ++x) {
            const auto pixel = frame.pixels[(y * swiv3d::image_height / height) * swiv3d::image_width + x * swiv3d::image_width / width];
            for (std::size_t channel = 0; channel < 3; ++channel) {
                preview.put(static_cast<char>(frame.palette[pixel * 3 + channel] * 4));
            }
        }
    }
    if (!json || !pixels || !preview) {
        throw std::runtime_error("Cannot write flight checkpoint");
    }
}


}
