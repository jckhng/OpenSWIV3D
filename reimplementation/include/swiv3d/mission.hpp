#pragma once

#include "swiv3d/frontend.hpp"

namespace swiv3d {

constexpr std::size_t map_side = 512;
constexpr std::size_t object_slots = 3000;
constexpr std::int32_t no_entity = -1;

enum class Behavior : std::int32_t { none, pose, helicopter, turret_top, tank, tank_top, aircraft, pickup,
    bullet, enemy_bullet, bang, explosion, smoke, smoker, wreck, dead_aircraft, ground_explosion,
    debris_large, debris_small, aircraft_explosion, secret_pickup, mounted_weapon,
    missile_single, missile_pair, missile_coast, fire_trail, shield, falling_bullet, homing_bullet, dummy_craft,
    falling_casing, cruise_missile, scatter_missile, shard_bomb, pulse_ring, pulse_wall,
    cluster_missile, cluster_child, bomber_missile, carpet_missile, bomblet,
    apc_wheel, pillbox, tank_factory, plane_factory, radar, air_patrol, attack_plane,
    enemy_missile, polaris_missile, boat, strafe_plane, strafe_factory, change_station, parked_helicopter,
    helicopter_change, buggy_change, ground_buggy, ground_ski, wheel,
    burst_factory, intruder_factory, intruder, conditional_pickup, hover, ski_station, ski_change, submarine,
    amphibious_tank, completion_pickup, boss_core, boss_base, boss_top, boss_air,
    drone, drone_gun, slug_drone, grav_tank, grav_top, carryall, hover_turret_base, hover_turret_top, ground_exit, buggy_exit,
    stationary_tank, stationary_top, trailer, trailer_missile,
    boss2_core, boss2_top, pyramid, pyramid_gun, pyramid_eye, pyramid_shot, pyramid_dive, mars_face, teleport,
    bomber_buddy, buddy_bomb, rocket_buddy, tracking_buddy, demo_helicopter };

enum class SoundAction { effect, loop, stop, stop_all };

struct SoundEvent {
    std::uint32_t tick;
    std::int32_t actor;
    std::int32_t sound;
    SoundAction action = SoundAction::effect;
};

enum class MissionPhase { playing, failed, completed };
enum class FinishKind { none = 0, success = 1, death = 2, delayed_success = 100 };

struct EntityState {
    std::int32_t slot = no_entity;
    // Original 108-byte domain record. References are slot IDs; behavior is an enum.
    std::array<std::int32_t, 27> words{};
};

struct ObjectiveState {
    std::int32_t entity;
    std::int32_t priority;
};

using KeyboardState = std::array<bool, 256>;
constexpr std::uint32_t mission_tick_rate = 40;
enum class CraftKind : std::int32_t { helicopter, buggy, ski, hover };
enum class PlayerControl { normal, disabled, ground_transition };

struct FlightState {
    CraftKind craft = CraftKind::helicopter;
    PlayerControl control = PlayerControl::normal;
    bool weapon_hold = false;
    std::int32_t power = 0, steering_force = 0, wheel_direction = 0, wheel_target = 0;
    std::array<std::int32_t, 3> attitude{}; // Original yaw/pitch/roll accumulators.
    std::array<std::uint32_t, 11> bindings{};
    ControllerKind controller = ControllerKind::keyboard;
    std::array<std::uint8_t, 4> axis_bindings{1,2,0,1};
    std::array<int, 3> axes{};
    std::uint32_t control_ticks = 0;
    std::int32_t previous_heading = 0;
    std::int32_t engine_pitch = 0;
    std::int32_t maximum_height = INT32_MAX - 7;
    std::uint32_t ground_impacts = 0;
};

struct WeaponState {
    std::int32_t primary_level = 0;
    std::int32_t primary_held = 0;
    std::array<std::int32_t, 20> ammunition{};
    std::int32_t selected = no_entity;
    std::int32_t requested = no_entity;
    std::int32_t phase = 0;
    std::int32_t progress = 0;
    std::int32_t shield = no_entity;
    std::array<std::int32_t, 21> mounts;
    std::array<std::int32_t, 4> buddies{{no_entity,no_entity,no_entity,no_entity}};
    std::uint32_t collected = 0;
    std::uint32_t secrets = 0;
    WeaponState() { mounts.fill(no_entity); }
};

struct GameMessage {
    std::uint32_t group = 0, id = 0;
    std::int32_t colour = 0;
    std::array<std::int32_t, 3> position{}, velocity{}, angles{}, spin{};
    std::int32_t lifetime = 0, screen_y = 0;
};

constexpr std::size_t ground_effect_slots = 5;
enum class GroundEffectKind : std::int32_t { pulse_ring, pulse_wall, teleport };

struct GroundEffect {
    std::array<std::int32_t, 3> position{};
    std::int32_t lifetime = 0;
    GroundEffectKind kind = GroundEffectKind::pulse_ring;
    std::int32_t next = no_entity;
};

struct GroundEffects {
    std::array<GroundEffect, ground_effect_slots> records{};
    std::int32_t active = no_entity, free = ground_effect_slots - 1;
    GroundEffects() {
        for (std::size_t i = 1; i < records.size(); ++i) { records[i].next = static_cast<int>(i) - 1; }
    }
};

struct MissionState {
    bool camera_jump = false;
    std::int32_t boss_destroyed = 0, boss_volley = 0;
    std::int32_t node_range = 0;
    std::array<std::int32_t, 2> object_direction{};
    std::uint32_t craft_set = 0;
    std::uint32_t water_flags = 0;
    std::int32_t free_slot = object_slots - 1;
    std::int32_t player = no_entity;
    std::int32_t objectives = 0;
    std::int32_t pickups = 0;
    std::int32_t secrets = 0;
    MissionPhase phase = MissionPhase::playing;
    FinishKind finish = FinishKind::none;
    std::int32_t finish_counter = -1;
    std::int32_t objectives_completed = 0;
    std::int32_t objective_state = 0;
    std::int32_t last_objective = 0;
    bool extracting = false;
    std::uint8_t line_colour = 0;
    std::uint32_t tick = 0;
    LandscapeClock landscape_clock;
    FlightState flight;
    WeaponState weapons;
    std::int32_t objective_heading = 0;
    std::int32_t score = 0;
    std::int32_t display_score = 0;
    std::int32_t health_bar = 0;
    std::uint32_t game_counter = 0;
    std::uint32_t shots = 0;
    std::uint32_t kills = 0;
    std::uint32_t effects_phase = 0;
    std::array<int, 5> direction_steps{-150,-2,0,2,150};
    std::int32_t gun_pitch = 0;
    std::int32_t unassigned_air_returns = 0;
    std::vector<EntityState> entities;
    std::vector<ObjectiveState> objective_targets;
    std::vector<SoundEvent> sound_events;
    std::vector<GameMessage> messages;
    GroundEffects ground_effects;
    std::array<std::int32_t, 4096> grid{};
    std::array<std::int32_t, 4096> lighting{};
    std::array<std::uint32_t, 1000> models{};
    std::array<std::uint32_t, 100> textures{};
    std::array<std::uint32_t, 100> filters{};
};

struct MissionFrame {
    std::vector<std::uint8_t> pixels;
    std::vector<std::uint8_t> terrain_pixels;
    std::vector<std::uint8_t> world_pixels;
    std::vector<std::uint8_t> stats_pixels;
    std::vector<std::array<std::int32_t, 20>> polygons;
    std::array<std::uint8_t, palette_bytes> palette{}; // Level resource palette, not live VGA DAC state.
    std::array<float, 3> camera_position{};
    std::array<float, 3> camera_target{};
    float camera_tilt = 0;
    std::array<int,4> camera_controls{}; // Hold mode, orbit, elevation and distance after render-time controls.
    std::array<std::int32_t, 12> view_transform{};
    std::array<int, 3> sound_right{};
    std::array<std::int32_t, 6> target_markers{};
    std::int32_t objective_heading = 0;
    std::vector<std::int32_t> consumed_hits;
};

enum class OverlayMode { mission, none, statistics };
enum class FrameCounterReset { render_end, event_loop_end };

struct RenderTiming {
    std::int32_t frames_per_second = mission_tick_rate;
    OverlayMode overlays = OverlayMode::mission;
    int screen_size = 255;
    int detail = 240;
    bool debug_controls = false;
    int debug_counter = initial_debug_counter;
    bool cheats_unlocked = false;
    KeyboardState shortcut_keys{};
    FrameCounterReset counter_reset = FrameCounterReset::render_end;
};

class Mission {
public:
    Mission(const Archive& level, const FrontendState& request,
            const std::vector<std::uint8_t>& configuration, const Archive* globals = nullptr);
    ~Mission();
    MissionState state() const;
    MissionResult result() const;
    std::vector<SoundEvent> take_sound_events();
    void begin_results();
    void menu_header(const FrontendState& frontend);
    void end_menu();
    void advance_menu();
    std::vector<std::uint8_t> resource(std::uint32_t id) const;
    std::int32_t height(std::int32_t x, std::int32_t z) const;
    // One recovered helicopter update. NPC AI, weapons and effects remain separate.
    void advance_flight(const KeyboardState& keys);
    void advance(const KeyboardState& keys, const DeviceInput& devices = {});
    void finish_event_loop();
    void apply_cheat(int action);
    void configure_controls(const std::vector<std::uint8_t>& configuration);
    void reload_background();
    void restart();
    MissionFrame render(const Archive& globals);
    MissionFrame render(const Archive& globals, RenderTiming timing);
    // Compatibility entry points; render the current world/HUD state.
    MissionFrame first_frame() const;
    MissionFrame first_frame(const Archive& globals) const;

private:
    class Impl;
    std::unique_ptr<Impl> impl_;
};

const char* behavior_name(Behavior behavior);

}
