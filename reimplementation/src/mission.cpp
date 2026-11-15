#include "swiv3d/mission.hpp"
#include "mission_render.hpp"
#include "hud.hpp"
#include "flight.hpp"
#include "combat.hpp"
#include "combat_math.hpp"
#include "game_text.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <map>
#include <stdexcept>

namespace swiv3d {
namespace {

constexpr std::size_t command_bytes = 64;
constexpr std::int32_t player_command = 1000;
constexpr std::int32_t demo_player_command = 1001;
constexpr std::uint32_t model_group = 10;
constexpr std::uint32_t texture_group = 12;
constexpr std::uint32_t resource_group_size = 1000;
constexpr std::size_t texture_header_bytes = 256;
constexpr std::size_t filter_bytes = 32768;
constexpr std::size_t texture_pixels_offset = texture_header_bytes + filter_bytes;
constexpr std::int32_t player_slot = object_slots;
constexpr std::int32_t fixed_scale = 256;
constexpr std::uint32_t rotor_clock_step = 10;
constexpr int pause_enter_sound = 6000, pause_return_sound = 6001;
constexpr int activity_side = 11;
// LSCAPE_gamelp execution classes around the player: full, dormant, omitted.
constexpr std::array<int, activity_side * activity_side> activity{{
    2,2,2,1,1,1,1,1,2,2,2,
    2,2,1,1,0,0,0,1,1,2,2,
    2,1,1,0,0,0,0,0,1,1,2,
    1,1,0,0,0,0,0,0,0,1,1,
    1,0,0,0,0,0,0,0,0,0,1,
    1,0,0,0,0,0,0,0,0,0,1,
    1,0,0,0,0,0,0,0,0,0,1,
    1,1,0,0,0,0,0,0,0,1,1,
    2,1,1,0,0,0,0,0,1,1,2,
    2,2,1,1,0,0,0,1,1,2,2,
    2,2,2,1,1,1,1,1,2,2,2
}};

std::uint32_t u32(const std::vector<std::uint8_t>& data, std::size_t offset) {
    if (offset > data.size() || data.size() - offset < 4) {
        throw std::runtime_error("Truncated level structure");
    }
    return static_cast<std::uint32_t>(data[offset]) | static_cast<std::uint32_t>(data[offset + 1]) << 8 |
           static_cast<std::uint32_t>(data[offset + 2]) << 16 | static_cast<std::uint32_t>(data[offset + 3]) << 24;
}

std::int32_t i32(const std::vector<std::uint8_t>& data, std::size_t offset) {
    const auto value = u32(data, offset);
    std::int32_t result;
    std::memcpy(&result, &value, sizeof(result));
    return result;
}

std::int32_t asr(std::int64_t value, unsigned bits) {
    const auto divisor = std::int64_t{1} << bits;
    return static_cast<std::int32_t>(value >= 0 ? value / divisor : -((-value + divisor - 1) / divisor));
}

std::size_t map_index(std::int32_t x, std::int32_t z) {
    return (static_cast<std::uint32_t>(z) & (map_side - 1)) * map_side +
           (static_cast<std::uint32_t>(x) & (map_side - 1));
}

std::size_t cell(const EntityState& entity) {
    return (asr(entity.words[7], 17) & 63) + (asr(entity.words[9], 11) & 4032);
}

}

class Mission::Impl {
public:
    Impl(const Archive& level, const FrontendState& request, const std::vector<std::uint8_t>& configuration, const Archive* globals)
        : request_(request) {
        state_.score = i32(profile(), 40);
        state_.display_score = state_.score;
        state_.game_counter = request.game_counter;
        if (globals != nullptr) { damage_ = globals->load(700); }
        state_.landscape_clock = request.landscape_clock;
        state_.objective_heading = request.objective_heading;
        state_.flight.control_ticks = request.control_ticks;
        state_.effects_phase = request.effects_phase;
        state_.flight.engine_pitch = request.engine_pitch;
        state_.weapons.progress = request.weapon_progress;
        state_.flight.attitude = request.flight_attitude;
        state_.node_range = request.node_range;
        state_.direction_steps = request.direction_steps;
        state_.craft_set = request.craft;
        state_.water_flags = request.water;
        if (request.phase != FrontendPhase::mission_request || request.craft > 3 || u32(profile(), 44) > 3) {
            throw std::runtime_error("Unsupported mission craft or campaign");
        }
        load_level(level);
        state_.grid.fill(no_entity);
        setup(configuration);
        initial_state_ = state_;
        initial_bytes_ = bytes_;
    }

    std::vector<std::uint8_t> resource(std::uint32_t id) const {
        if (id >= 10000 && id < 11000 && directory_.find(id) == directory_.end()) {
            const auto offset = state_.models[id - 10000];
            const auto alias = std::find_if(directory_.begin(),directory_.end(),[&](const auto& entry) {
                return entry.first >= 10000 && entry.first < 11000 && entry.second.offset == offset;
            });
            if (alias == directory_.end()) { throw std::runtime_error("Unbound model alias"); }
            id = alias->first;
        }
        const auto& entry = member(id);
        return {bytes_.begin() + entry.offset, bytes_.begin() + entry.offset + entry.size};
    }

    std::int32_t height(std::int32_t x, std::int32_t z) const {
        const auto map_x = asr(x, 14);
        const auto map_z = asr(z, 14);
        const auto frac_x = asr(x, 6) & 255;
        const auto frac_z = asr(z, 6) & 255;
        const auto base = member(100).offset;
        const auto sample = [&](int dx, int dz) {
            return static_cast<std::int32_t>(bytes_[base + map_index(map_x + dx, map_z + dz)]) * 1024;
        };
        const auto top = sample(0, 0) + asr((sample(1, 0) - sample(0, 0)) * frac_x, 8);
        const auto bottom = sample(0, 1) + asr((sample(1, 1) - sample(0, 1)) * frac_x, 8);
        return top + asr((bottom - top) * frac_z, 8);
    }

    MissionState state() const { return state_; }

    MissionResult result() const {
        if (state_.phase == MissionPhase::playing) { throw std::runtime_error("Mission has no result"); }
        MissionResult result{state_.phase == MissionPhase::completed ? MissionOutcome::completed : MissionOutcome::failed};
        result.profile = request_.mission_profile;
        result.elapsed_updates = outcome_tick_;
        result.landscape_clock = state_.landscape_clock;
        result.game_counter = state_.game_counter;
        result.objective_heading = state_.objective_heading;
        result.control_ticks = state_.flight.control_ticks;
        result.effects_phase = state_.effects_phase;
        result.engine_pitch = state_.flight.engine_pitch;
        result.weapon_progress = state_.weapons.progress;
        result.direction_steps = state_.direction_steps;
        result.flight_attitude = state_.flight.attitude;
        result.node_range = state_.node_range;
        result.level_score = state_.score - i32(profile(), 40);
        result.ratios = {state_.objectives == 0 ? 0 : state_.objectives_completed * 100 / state_.objectives,
                         state_.pickups == 0 ? 0 : static_cast<int>(state_.weapons.collected) * 100 / state_.pickups,
                         state_.secrets == 0 ? 0 : static_cast<int>(state_.weapons.secrets) * 100 / state_.secrets};
        const auto& player = state_.entities.at(entity_indices_.at(state_.player)).words;
        for (std::size_t i = 0; i < state_.weapons.ammunition.size(); ++i) {
            const auto ammo = static_cast<std::uint32_t>(state_.weapons.ammunition[i]);
            result.profile[i * 2] = static_cast<std::uint8_t>(ammo);
            result.profile[i * 2 + 1] = static_cast<std::uint8_t>(ammo >> 8);
        }
        const auto write = [&](std::size_t offset, std::uint32_t value) {
            for (std::size_t i = 0; i < 4; ++i) { result.profile[offset + i] = static_cast<std::uint8_t>(value >> (i * 8)); }
        };
        write(40, state_.score);
        write(44, static_cast<std::uint32_t>(state_.flight.craft));
        write(48, player[2]);
        write(52, state_.weapons.primary_level);
        return result;
    }

    void begin_results() {
        if (state_.phase == MissionPhase::playing) { throw std::runtime_error("Mission has not ended"); }
        results_active_ = true;
    }

    void end_menu() {
        for (auto& message : state_.messages) {
            if (message.lifetime > 999) { message.lifetime -= 980; }
        }
    }

    void advance_menu() {
        detail::advance_messages(state_);
        ++camera_.yaw;
    }

    void menu_header(const FrontendState& frontend) {
        if (frontend.mission_menu && frontend.phase != FrontendPhase::mission_grab &&
            frontend.phase != FrontendPhase::grab_name && !mission_menu_yaw_) { mission_menu_yaw_ = camera_.yaw; }
        const auto phase = frontend.phase;
        if (menu_phase_ == FrontendPhase::main_menu && phase == FrontendPhase::exit_confirmation) { camera_.yaw = main_menu_yaw_; }
        if (phase == FrontendPhase::exit_confirmation) { submenu_yaw_ = camera_.yaw; }
        if (menu_phase_ == FrontendPhase::exit_confirmation && phase == FrontendPhase::main_menu) { camera_.yaw = submenu_yaw_; }
        request_.language = frontend.language;
        if (menu_phase_ == FrontendPhase::main_menu && phase == FrontendPhase::pilot_selection) { camera_.yaw = main_menu_yaw_; }
        if (phase == FrontendPhase::pilot_selection && menu_phase_ != FrontendPhase::pilot_name &&
            menu_phase_ != FrontendPhase::pilot_deletion) { pilot_menu_yaw_ = camera_.yaw; }
        if ((menu_phase_ == FrontendPhase::pilot_selection || menu_phase_ == FrontendPhase::pilot_name) &&
            (phase == FrontendPhase::controller_type || phase == FrontendPhase::main_menu)) { camera_.yaw = pilot_menu_yaw_; }
        if (menu_phase_ == FrontendPhase::controller_type) { camera_.yaw = controller_menu_yaw_; }
        if (menu_phase_ == FrontendPhase::main_menu && phase == FrontendPhase::level_selection) { camera_.yaw = main_menu_yaw_; }
        if (menu_phase_ == FrontendPhase::level_selection && phase == FrontendPhase::main_menu) { camera_.yaw = level_menu_yaw_; }
        if (phase == FrontendPhase::controller_type) { controller_menu_yaw_ = camera_.yaw; }
        if (phase == FrontendPhase::main_menu && menu_phase_ != FrontendPhase::settings &&
            menu_phase_ != FrontendPhase::exit_confirmation) { main_menu_yaw_ = camera_.yaw; }
        if (phase == FrontendPhase::level_selection && menu_phase_ != FrontendPhase::level_selection) { level_menu_yaw_ = camera_.yaw; }
        if (menu_phase_ == FrontendPhase::settings && phase != FrontendPhase::main_menu) { submenu_yaw_ = camera_.yaw; }
        if ((menu_phase_ == FrontendPhase::sound_volume || menu_phase_ == FrontendPhase::language_selection ||
             menu_phase_ == FrontendPhase::music_selection) && phase == FrontendPhase::settings) { camera_.yaw = submenu_yaw_; }
        menu_phase_ = phase;
        if (phase != FrontendPhase::mission_pause) { end_menu(); }
        using detail::MessageMotion;
        switch (phase) {
        case FrontendPhase::mission_pause:
            detail::cue_message(state_,4,0,0x400,MessageMotion::notice);
            state_.sound_events.push_back({state_.tick,no_entity,pause_enter_sound,SoundAction::effect});
            break;
        case FrontendPhase::mission_abort: detail::cue_message(state_,13,0,0x1400,MessageMotion::notice); break;
        case FrontendPhase::pilot_selection: detail::cue_message(state_,0,0,0x1500,MessageMotion::notice); break;
        case FrontendPhase::pilot_name: detail::cue_message(state_,0,50,0x1400,MessageMotion::notice); break;
        case FrontendPhase::controller_type: detail::cue_message(state_,3,100,0x1000,MessageMotion::notice); break;
        case FrontendPhase::axis_bindings: detail::cue_message(state_,3,1000,0x500,MessageMotion::notice); break;
        case FrontendPhase::cheat_menu: detail::cue_message(state_,100,1000,0x100,MessageMotion::notice); break;
        case FrontendPhase::controller_bindings: detail::cue_message(state_,3,0,0x400,MessageMotion::notice); break;
        case FrontendPhase::main_menu: detail::cue_message(state_,1,0,0x1000,MessageMotion::notice); break;
        case FrontendPhase::level_selection: detail::cue_message(state_,11,frontend.cheat_mode ? 20 : 0,0x400,MessageMotion::notice); break;
        case FrontendPhase::settings: detail::cue_message(state_,2,0,0x1500,MessageMotion::notice); break;
        case FrontendPhase::pilot_deletion: detail::cue_message(state_,0,60,0x1000,MessageMotion::notice); break;
        case FrontendPhase::key_capture: detail::cue_message(state_,3,33,0x1400,MessageMotion::notice); break;
        case FrontendPhase::sound_volume: detail::cue_message(state_,8,0,0x1500,MessageMotion::notice); break;
        case FrontendPhase::language_selection: detail::cue_message(state_,2,100,0x400,MessageMotion::notice); break;
        case FrontendPhase::gamma_selection: detail::cue_message(state_,6,0,0x100,MessageMotion::notice); break;
        case FrontendPhase::music_selection: detail::cue_message(state_,9,0,0x1500,MessageMotion::notice); break;
        case FrontendPhase::exit_confirmation: detail::cue_message(state_,13,20,0x1400,MessageMotion::notice); break;
        default: break;
        }
    }

    std::vector<SoundEvent> take_sound_events() {
        std::vector<SoundEvent> events;
        events.swap(state_.sound_events);
        return events;
    }

    void restart() {
        if (state_.phase != MissionPhase::failed) { throw std::runtime_error("Restart requires a failed mission"); }
        reset_scene();
    }

    void reload_background() {
        if (initial_state_.entities.at(entity_indices_.at(state_.player)).words[26] != static_cast<int>(Behavior::demo_helicopter)) {
            throw std::runtime_error("Background reload requires DEMO");
        }
        const auto tick = state_.tick;
        reset_scene();
        state_.tick = tick;
        menu_phase_ = FrontendPhase::pilot_selection;
        pilot_menu_yaw_ = controller_menu_yaw_ = main_menu_yaw_ = level_menu_yaw_ = submenu_yaw_ = 0;
    }

    void configure_controls(const std::vector<std::uint8_t>& configuration) {
        // Controller defaults and axis bindings belong to the retained profile.
        auto configured = state_.flight;
        bool controls = false;
        for (std::size_t offset = 0; offset + 128 <= configuration.size(); offset += 128) {
            if (i32(configuration, offset) == 20 && i32(configuration, offset + 4) == request_.player &&
                i32(configuration, offset + 8) == 2) {
                const auto type = u32(configuration, offset + 12);
                if (type < 1 || type > 6) {
                    throw std::runtime_error("Unsupported mission controller");
                }
                configured.controller = static_cast<ControllerKind>(type);
                std::copy_n(configuration.begin() + offset + 0x3c,4,configured.axis_bindings.begin());
                const unsigned axis_count = type == 2 || type == 4 ? 2 : type == 3 || type == 5 ? 3 : 0;
                for (std::size_t i = 0; i < 3; ++i) {
                    if (configured.axis_bindings[i] > axis_count) { configured.axis_bindings[i] = 0; }
                }
                for (std::size_t i = 0; i < configured.bindings.size(); ++i) {
                    const auto scan = u32(configuration, offset + 16 + i * 4);
                    if (scan >= KeyboardState{}.size()) {
                        throw std::runtime_error("Controller scan code exceeds keyboard state");
                    }
                    configured.bindings[i] = scan;
                }
                controls = true;
                break;
            }
        }
        if (!controls) {
            throw std::runtime_error("Mission has no retained controller record");
        }
        state_.flight = configured;
        initial_state_.flight.controller = configured.controller;
        initial_state_.flight.bindings = configured.bindings;
        initial_state_.flight.axis_bindings = configured.axis_bindings;
        if (mission_menu_yaw_) {
            camera_.yaw = *mission_menu_yaw_;
            mission_menu_yaw_.reset();
            if (menu_phase_ == FrontendPhase::mission_pause) {
                state_.sound_events.push_back({state_.tick,no_entity,pause_return_sound,SoundAction::effect});
            }
        }
    }

    void apply_cheat(int action) {
        if (!request_.cheat_mode) { return; }
        ensure_combat();
        combat_->apply_cheat(action);
    }

    void advance(const KeyboardState& keyboard, const DeviceInput& devices) {
        auto keys = keyboard;
        for (std::size_t i = 0; i < devices.mouse_buttons.size(); ++i) { keys[0x90 + i] = keys[0x90 + i] || devices.mouse_buttons[i]; }
        for (std::size_t i = 0; i < devices.joystick_buttons.size(); ++i) { keys[0x93 + i] = keys[0x93 + i] || (devices.joystick_connected && devices.joystick_buttons[i]); }
        state_.flight.axes = state_.flight.controller == ControllerKind::mouse ? devices.mouse :
            devices.joystick_connected ? devices.joystick : std::array<int,3>{};
        for (auto& axis : state_.flight.axes) { axis = std::clamp(axis,-128,128); }
        if ((state_.flight.controller == ControllerKind::joystick || state_.flight.controller == ControllerKind::gamepad) &&
            state_.flight.axis_bindings[3] == 0) {
            for (auto& axis : state_.flight.axes) { if (axis > -26 && axis < 26) { axis = 0; } }
        }
        ensure_combat();
        combat_->advance(keys);
        detail::advance_messages(state_);
        if (state_.camera_jump) { camera_ = {}; state_.camera_jump = false; }
        if (results_active_ || entity(state_.player).words[26] == static_cast<int>(Behavior::demo_helicopter)) { ++camera_.yaw; }
        if (state_.phase != MissionPhase::playing && outcome_tick_ == 0) { outcome_tick_ = state_.tick; }
    }

    void advance_flight(const KeyboardState& keys) {
        auto& player = entity(state_.player);
        const int cell_x = asr(player.words[7], 17), cell_z = asr(player.words[9], 17);
        std::vector<std::size_t> execution;
        for (int z = 0; z < activity_side; ++z) {
            for (int x = 0; x < activity_side; ++x) {
                const auto index = ((cell_z + z - activity_side / 2) & 63) * 64 + ((cell_x + x - activity_side / 2) & 63);
                if (activity[z * activity_side + x] == 0 && state_.grid[index] != no_entity) {
                    execution.push_back(static_cast<std::size_t>(index));
                }
            }
        }
        for (auto index : execution) {
            if (cell(player) != index) {
                continue;
            }
            unlink(player);
            detail::move_helicopter(player, state_.flight,
                [this](std::int32_t x, std::int32_t z) { return height(x, z); });
            link(player);
        }
        detail::control_helicopter(player, state_.flight, keys, entity(player.words[22]));
        detail::search_flight_target(state_);
        ++state_.tick;
        state_.landscape_clock.rotor += rotor_clock_step;
        ++state_.landscape_clock.water;
    }

    MissionFrame first_frame() const {
        return detail::render_mission(state_, request_, bytes_, directory_,
            [this](std::int32_t x, std::int32_t z) { return height(x, z); },nullptr,nullptr,{},
            [this](std::int32_t x, std::int32_t z) { return surface_height(x, z); });
    }

    MissionFrame first_frame(const Archive& globals) const {
        auto frame = detail::render_mission(state_, request_, bytes_, directory_,
            [this](int x, int z) { return height(x, z); }, &globals,nullptr,{},
            [this](int x, int z) { return surface_height(x, z); });
        detail::draw_initial_hud(frame, globals, request_, state_, resource(103), resource(108), resource(11003));
        detail::draw_messages(frame,state_,request_,globals,resource(11003));
        return frame;
    }

    MissionFrame render(const Archive& globals, RenderTiming timing) {
        const auto map_markers = debug_markers_ ? detail::MapMarkers::debug : detail::MapMarkers::normal;
        auto frame = detail::render_mission(state_, request_, bytes_, directory_,
            [this](int x, int z) { return height(x, z); }, &globals, &camera_, timing,
            [this](int x, int z) { return surface_height(x, z); });
        // The original samples these keys after drawing this camera.
        constexpr int camera_step = 6;
        if (timing.cheats_unlocked) {
            if (timing.shortcut_keys[0x3f]) { camera_.yaw += camera_step; }
            if (timing.shortcut_keys[0x40]) { camera_.yaw -= camera_step; }
            if (timing.shortcut_keys[0x41]) { camera_.pitch += camera_step; }
            if (timing.shortcut_keys[0x42]) { camera_.pitch -= camera_step; }
        }
        if (timing.overlays == OverlayMode::mission && state_.finish == FinishKind::none) {
            if (timing.cheats_unlocked && timing.shortcut_keys[0x3e]) { camera_.hold_position = true; debug_markers_ = true; }
            if (timing.shortcut_keys[0x3d]) { debug_markers_ = false; }
        }
        frame.camera_controls = {camera_.hold_position ? 1 : 0,camera_.yaw,camera_.pitch,550};
        if (timing.overlays != OverlayMode::none) {
            const int target_slot = entity(state_.player).words[22];
            const auto target = std::find_if(state_.entities.begin(), state_.entities.end(),
                [&](const auto& actor) { return actor.slot == target_slot; });
            if (target != state_.entities.end() && (target->words[1] & 0x2000) != 0 && !state_.objective_targets.empty()) {
                const auto found = std::find_if(state_.objective_targets.begin(), state_.objective_targets.end(),
                    [&](const auto& objective) { return objective.entity == target_slot && objective.priority == state_.objective_targets.front().priority; });
                if (found != state_.objective_targets.end()) {
                    const auto objective = *found;
                    state_.objective_targets.erase(found);
                    state_.objective_targets.insert(state_.objective_targets.begin(), objective);
                }
            }
            detail::draw_initial_hud(frame, globals, request_, state_, resource(103), resource(108), resource(11003),
                                     map_markers);
            state_.objective_heading = frame.objective_heading;
        }
        for (auto slot : frame.consumed_hits) {
            const auto found = std::find_if(state_.entities.begin(), state_.entities.end(),
                [slot](const auto& actor) { return actor.slot == slot; });
            if (found != state_.entities.end()) { found->words[1] &= 0xdffff; }
        }
        if (timing.overlays == OverlayMode::mission || entity(state_.player).words[26] == static_cast<int>(Behavior::demo_helicopter)) {
            // The live mission loop drains timer messages after this render.
            frame_counter_reset_pending_ = true;
            if (timing.counter_reset == FrameCounterReset::render_end) { finish_event_loop(); }
        }
        detail::draw_messages(frame,state_,request_,globals,resource(11003));
        if (timing.debug_controls) { detail::draw_debug_coordinates(frame,globals,state_,resource(108),timing.debug_counter); }
        return frame;
    }

    void finish_event_loop() {
        if (frame_counter_reset_pending_) {
            state_.game_counter = 0;
            frame_counter_reset_pending_ = false;
        }
    }

private:
    void reset_scene() {
        const auto clock = state_.landscape_clock;
        const auto game_counter = state_.game_counter;
        const auto weapon_progress = state_.weapons.progress;
        combat_.reset();
        camera_ = {};
        outcome_tick_ = 0;
        results_active_ = false;
        frame_counter_reset_pending_ = false;
        state_ = initial_state_;
        state_.landscape_clock = clock;
        state_.game_counter = game_counter;
        state_.weapons.progress = weapon_progress;
        bytes_ = initial_bytes_;
        entity_indices_.clear();
        for (std::size_t i = 0; i < state_.entities.size(); ++i) { entity_indices_.emplace(state_.entities[i].slot, i); }
    }

    void set_model_bound(int model,int bound) {
        const auto offset = state_.models.at(model) + 12;
        for (unsigned i = 0; i < 4; ++i) { bytes_.at(offset + i) = static_cast<std::uint8_t>(static_cast<std::uint32_t>(bound) >> (i * 8)); }
    }

    std::int32_t surface_height(int x, int z) const {
        if ((request_.water & 2) == 0) { return height(x, z); }
        const int mx = asr(x, 14), mz = asr(z, 14), fx = asr(x, 6) & 255, fz = asr(z, 6) & 255;
        const auto sample = [&](int dx, int dz) {
            const auto index = map_index(mx + dx, mz + dz);
            int value = bytes_[member(100).offset + index] * 1024;
            if ((bytes_[member(102).offset + index] & 224) == 0) {
                const auto phase = ((mz + dz) * 4 + static_cast<int>(state_.landscape_clock.water) +
                                    detail::combat_math::sine(((mx + dx) & 31) * 16)) & 31;
                value += static_cast<int>(std::round(16 * std::sin(phase * detail::combat_math::pi / 32))) * 256;
            }
            return value;
        };
        const int top = sample(0, 0) + asr((sample(1, 0) - sample(0, 0)) * fx, 8);
        const int bottom = sample(0, 1) + asr((sample(1, 1) - sample(0, 1)) * fx, 8);
        return top + asr((bottom - top) * fz, 8);
    }

    void deform(const EntityState& actor, int size) {
        if (damage_.size() != 3072) { throw std::runtime_error("Terrain damage needs GLOBAL.BD resource 700"); }
        const auto& w = actor.words;
        const int map_x = asr(w[7], 14), map_z = asr(w[9], 14);
        int sweep_x = map_x;
        for (int row = 0; row < 2; ++row) {
            const int z = (map_z + row * 4) >> 3;
            if (row == 1 && z == (map_z >> 3)) { break; }
            for (int column = 0; column < 2; ++column) {
                const int x = sweep_x >> 3;
                auto slot = state_.grid[(z & 63) * 64 + (x & 63)];
                while (slot != no_entity) {
                    const auto& other = *std::find_if(state_.entities.begin(), state_.entities.end(),
                        [slot](const auto& candidate) { return candidate.slot == slot; });
                    slot = other.words[0];
                    if ((other.words[1] & 0x12) != 0 || other.slot == actor.slot) { continue; }
                    const int dx = asr(w[7], 8) - asr(other.words[7], 8), dz = asr(w[9], 8) - asr(other.words[9], 8);
                    if (dx * dx + dz * dz + 1 < (size + 1) * 10000) { return; }
                }
                // The original carries its X sweep into the second grid row.
                sweep_x += 4;
                if (column == 0 && (sweep_x >> 3) == x) { break; }
            }
        }
        const auto hmap = member(100).offset, tmap = member(102).offset;
        for (int z = map_z - 2; z < map_z + 4; ++z) {
            for (int x = map_x - 2; x < map_x + 4; ++x) {
                if (bytes_[tmap + map_index(x, z)] == 0) { return; }
            }
        }
        auto cursor = size * 1024 + (((asr(w[9], 12) & 3) * 4 + (asr(w[7], 12) & 3)) * 64);
        for (int z = map_z - 1; z < map_z + 3; ++z) {
            for (int x = map_x - 1; x < map_x + 3; ++x) {
                const auto index = map_index(x, z);
                const int amount = i32(damage_, cursor);
                cursor += 4;
                if (bytes_[tmap + index] != 0) { bytes_[tmap + index] = static_cast<std::uint8_t>(std::min(240, bytes_[tmap + index] + amount * 2)); }
                bytes_[hmap + index] = static_cast<std::uint8_t>(std::max(0, bytes_[hmap + index] - amount));
            }
        }
        for (int z = map_z - 1; z < map_z + 3; ++z) {
            for (int x = map_x - 1; x < map_x + 3; ++x) {
                const auto index = map_index(x, z);
                int total = 0;
                bool smooth = false;
                for (int dz = -1; dz <= 1; ++dz) {
                    for (int dx = -1; dx <= 1; ++dx) {
                        const int value = bytes_[hmap + map_index(x + dx, z + dz)];
                        total += value;
                        smooth |= std::abs(value - bytes_[hmap + index]) > 19;
                    }
                }
                if (smooth) { bytes_[hmap + index] = static_cast<std::uint8_t>(total / 9); }
            }
        }
    }

    void unlink(EntityState& player) {
        auto* link = &state_.grid[cell(player)];
        while (*link != player.slot) {
            if (*link == no_entity) {
                throw std::runtime_error("Player is missing from its grid cell");
            }
            link = &entity(*link).words[0];
        }
        *link = player.words[0];
    }
    const Resource& member(std::uint32_t id) const {
        const auto found = directory_.find(id);
        if (found == directory_.end()) {
            throw std::runtime_error("Missing mission resource");
        }
        return found->second;
    }

    void load_level(const Archive& level) {
        for (const auto& entry : level.resources()) {
            directory_.emplace(entry.id, entry);
            bytes_.resize(std::max(bytes_.size(), static_cast<std::size_t>(entry.offset) + entry.size));
            const auto data = level.load(entry.id);
            std::copy(data.begin(), data.end(), bytes_.begin() + entry.offset);
        }
        for (auto id : {100U, 102U, 103U}) {
            if (member(id).size != map_side * map_side) {
                throw std::runtime_error("Unsupported mission map dimensions");
            }
        }
        bind_assets(level);
    }

    void bind_assets(const Archive& level) {
        bool first_model = true;
        for (const auto& entry : level.resources()) {
            const auto group = entry.id / resource_group_size;
            const auto index = entry.id % resource_group_size;
            if (group == model_group) {
                if (entry.size < 32 || index >= state_.models.size()) {
                    throw std::runtime_error("Unsupported model header");
                }
                state_.models[index] = entry.offset;
                if (first_model) {
                    state_.models.fill(entry.offset);
                    first_model = false;
                }
                for (auto field : {24U, 28U}) {
                    if (u32(bytes_, entry.offset + field) >= entry.size) {
                        throw std::runtime_error("Model data exceeds its member");
                    }
                }
            }
            if (group != texture_group) {
                continue;
            }
            if (entry.size < texture_pixels_offset || index * 16 + 15 >= state_.textures.size()) {
                throw std::runtime_error("Unsupported texture bank");
            }
            // Resolve offsets on access. The DOS loader copies the filter to an
            // aligned address; its bytes and aliases have no host-pointer identity.
            if (index == 0) {
                state_.textures.fill(entry.offset + texture_pixels_offset);
                state_.filters.fill(entry.offset + texture_header_bytes);
                continue;
            }
            for (std::size_t tile = 0; tile < 16; ++tile) {
                const auto offset = (tile / 4) * 16384 + (tile % 4) * 64;
                if (texture_pixels_offset + offset + 63 * 256 + 63 >= entry.size) {
                    throw std::runtime_error("Texture tile exceeds its bank");
                }
                state_.textures[index * 16 + tile] = static_cast<std::uint32_t>(entry.offset + texture_pixels_offset + offset);
                state_.filters[index * 16 + tile] = entry.offset + texture_header_bytes;
            }
        }
        if (first_model) {
            throw std::runtime_error("Level has no models");
        }
        const auto& palette = member(11003);
        if (palette.size < palette_bytes + 8192) {
            throw std::runtime_error("Missing level palette translation table");
        }
        state_.line_colour = bytes_.at(palette.offset + palette_bytes + 0x107f);
    }

    EntityState& entity(std::int32_t slot) {
        return state_.entities.at(entity_indices_.at(slot));
    }

    EntityState& allocate(std::int32_t slot) {
        entity_indices_.emplace(slot, state_.entities.size());
        state_.entities.push_back({slot, {}});
        return state_.entities.back();
    }

    EntityState& add(const std::array<std::int32_t, 16>& command, std::int32_t flags,
                     std::int32_t model, std::int32_t altitude, Behavior behavior) {
        if (state_.free_slot < 0) {
            throw std::runtime_error("Mission object pool exhausted");
        }
        auto& result = allocate(state_.free_slot--);
        reference(result, command, flags, model, altitude, behavior);
        return result;
    }

    void reference(EntityState& result, const std::array<std::int32_t, 16>& command,
                   std::int32_t flags, std::int32_t model, std::int32_t altitude, Behavior behavior) {
        if (model < 0 || static_cast<std::size_t>(model) >= state_.models.size()) {
            throw std::runtime_error("Invalid model reference");
        }
        auto& w = result.words;
        w[1] = flags;
        w[2] = command[6];
        w[5] = 10;
        w[7] = command[1] * fixed_scale;
        w[9] = command[2] * fixed_scale;
        const auto ground = height(w[7], w[9]);
        w[8] = ground + altitude * fixed_scale;
        w[7] &= 0x7fffff;
        w[9] &= 0x7fffff;
        w[13] = command[3];
        w[16] = -1;
        w[19] = model;
        w[22] = result.slot;
        w[23] = result.slot;
        w[26] = static_cast<std::int32_t>(behavior);
        link(result);
    }

    void link(EntityState& result) {
        auto& head = state_.grid[cell(result)];
        result.words[0] = head;
        head = result.slot;
    }

    std::int32_t model_floor(std::int32_t model) const {
        if (model < 0 || static_cast<std::size_t>(model) >= state_.models.size()) {
            throw std::runtime_error("Invalid model floor reference");
        }
        return i32(bytes_, state_.models[model] + 16);
    }

    std::int32_t spawn(std::int32_t parent, std::int32_t flags, std::int32_t model,
                       std::int32_t energy, Behavior behavior) {
        const auto source = entity(parent);
        auto& child = allocate(state_.free_slot--);
        child.words = source.words;
        child.words[1] = flags;
        child.words[2] = energy;
        child.words[6] = 0;
        child.words[16] = -1;
        child.words[19] = model;
        child.words[20] = 0;
        child.words[21] = 0;
        child.words[23] = parent;
        child.words[26] = static_cast<std::int32_t>(behavior);
        link(child);
        return child.slot;
    }

    void objective(EntityState& result, std::int32_t value) {
        if (value >= 5000 && value < 5010) {
            result.words[1] |= 0x2000;
            ++state_.objectives;
            const ObjectiveState objective{result.slot, value - 5000};
            auto position = state_.objective_targets.begin();
            if (position != state_.objective_targets.end() && position->priority >= objective.priority) {
                ++position;
                while (position != state_.objective_targets.end() && position->priority > objective.priority) {
                    ++position;
                }
            }
            state_.objective_targets.insert(position, objective);
        }
    }

    void setup_player(const std::array<std::int32_t, 16>& command) {
        auto& player = allocate(player_slot);
        reference(player, command, 0x217, 496, 80, Behavior::helicopter);
        player.words[2] = static_cast<std::int32_t>(u32(profile(), 48));
        player.words[19] = 990;
        state_.weapons.primary_level = i32(profile(), 52);
        const auto payload = profile();
        for (std::size_t i = 0; i < state_.weapons.ammunition.size(); ++i) {
            const int value = payload.at(i * 2) | payload.at(i * 2 + 1) << 8;
            state_.weapons.ammunition[i] = value < 32768 ? value : value - 65536;
        }
        state_.flight.previous_heading = command[3];
        state_.player = player_slot;
        if (command[0] == demo_player_command) {
            state_.flight.attitude = {player.words[13] * 256, player.words[14] * 256, player.words[15] * 128};
            player.words[1] = 0x40;
            player.words[26] = static_cast<int>(Behavior::demo_helicopter);
            state_.flight.control = PlayerControl::disabled;
            return;
        }
        detail::Combat initialization(state_,
            [this](int x, int z) { return height(x, z); },
            [this](std::uint32_t id) { return resource(id); }, {},
            [this](int x, int z) { return surface_height(x, z); },
            static_cast<CraftKind>(u32(profile(), 44)));
        for (std::size_t i = 0; i < state_.entities.size(); ++i) {
            entity_indices_[state_.entities[i].slot] = i;
        }
    }

    void setup(const std::vector<std::uint8_t>& configuration) {
        // Reserve once: spawn/reference retain parent references while allocating.
        state_.entities.reserve(object_slots + 1);
        const auto& commands = member(105);
        bool found_player = false;
        bool ended = false;
        for (std::size_t offset = 0; offset + command_bytes <= commands.size; offset += command_bytes) {
            std::array<std::int32_t, 16> command{};
            for (std::size_t index = 0; index < command.size(); ++index) {
                command[index] = i32(bytes_, commands.offset + offset + index * 4);
            }
            if (command[0] == -1) {
                ended = true;
                break;
            }
            if (command[0] == player_command || command[0] == demo_player_command) {
                if (found_player) {
                    continue;
                }
                setup_player(command);
                found_player = true;
                continue;
            }
            if (command[0] == 7900) {
                state_.flight.maximum_height = height(command[1] * fixed_scale,command[2] * fixed_scale);
                continue;
            }
            if (command[0] == 7800 || command[0] == 7801) {
                auto& actor = add(command,0x841,command[4],-model_floor(command[4]),Behavior::teleport);
                actor.words[20] = command[0] == 7800 ? 0 : 10;
                actor.words[21] = command[7]; actor.words[5] = command[3];
                continue;
            }
            if (command[0] == 3200 || command[0] == 8030) {
                const bool eye = command[0] == 3200;
                auto& actor = add(command,eye ? 1 : 0x40,command[4],-model_floor(command[4]),
                    eye ? Behavior::pyramid_eye : Behavior::mars_face);
                actor.words[25] = static_cast<int>(offset);
                if (eye) {
                    objective(actor,command[7]); actor.words[21] = command[7];
                    if (command[7] >= 1000 && command[7] < 2000) { ++state_.secrets; }
                }
                continue;
            }
            if (command[0] == 8010 || command[0] == 8020) {
                const bool pyramid = command[0] == 8020;
                auto& core = add(command,0x40,command[4],-model_floor(command[4]),pyramid ? Behavior::pyramid : Behavior::boss2_core);
                core.words[2] = pyramid ? 800 : 500; core.words[25] = static_cast<int>(offset);
                objective(core,command[7]);
                if (!pyramid) {
                    state_.boss_destroyed = 0;
                    for (int i = 0; i < 2; ++i) {
                        const auto slot = spawn(core.slot,0x1001,command[11 + i],250,Behavior::boss2_top);
                        entity(slot).words[20] = i; entity(slot).words[22] = core.slot;
                    }
                } else {
                    for (int i = 0; i < 4; ++i) {
                        auto placement = command;
                        const int heading = core.words[13] + i * 128 + 64;
                        placement[1] += detail::combat_math::shift(detail::combat_math::sine(heading) * 400,8);
                        placement[2] += detail::combat_math::shift(detail::combat_math::cosine(heading) * 400,8);
                        auto& base = add(placement,0x1001,command[11],-model_floor(command[11]),Behavior::stationary_tank);
                        base.words[2] = 300; base.words[23] = core.slot; base.words[13] = heading;
                        objective(base,command[7]);
                        const auto top = spawn(base.slot,0x1001,command[12],300,Behavior::stationary_top);
                        entity(top).words[21] = 109; entity(top).words[22] = core.slot;
                    }
                }
                continue;
            }
            if (command[0] == 7020) {
                auto& actor = add(command,1,command[4],-model_floor(command[4]),Behavior::ground_exit);
                actor.words[20] = 5;
                objective(actor,command[7]);
                continue;
            }
            if (command[0] == 3100 || command[0] == 5004) {
                const bool trailer = command[0] == 5004;
                auto& base = add(command,trailer ? 0x1003 : 0x1001,command[4],-model_floor(command[4]),
                    trailer ? Behavior::tank : Behavior::stationary_tank);
                if (trailer) { base.words[3] = command[8]; }
                const auto child = spawn(base.slot,trailer ? 0x1003 : 0x1001,command[5],command[6],
                    trailer ? Behavior::trailer : Behavior::stationary_top);
                entity(child).words[22] = child;
                if (trailer) {
                    entity(child).words[3] = command[8]; entity(child).words[5] = 230;
                    for (int index = 0; index < 2; ++index) {
                        const auto missile = spawn(child,0x40,command[11],70,Behavior::trailer_missile);
                        entity(missile).words[20] = index;
                    }
                } else { entity(child).words[21] = command[7]; }
                continue;
            }
            if (command[0] == 4100 || command[0] == 4101 || command[0] == 5010) {
                const bool grav = command[0] == 5010, slug = command[0] == 4101;
                auto& actor = add(command,grav ? 0x1003 : 0x1001,command[4],-model_floor(command[4]),
                    grav ? Behavior::grav_tank : slug ? Behavior::slug_drone : Behavior::drone);
                actor.words[3] = command[8];
                if (slug) { actor.words[21] = command[7]; }
                else {
                    const auto slot = spawn(actor.slot,grav ? 0x1003 : 0x1001,command[5],command[6],
                        grav ? Behavior::grav_top : Behavior::drone_gun);
                    entity(slot).words[21] = command[7]; entity(slot).words[22] = slot;
                }
                continue;
            }
            if (command[0] == 3002 || command[0] == 6200) {
                const bool carried = command[0] == 6200;
                auto& actor = add(command,carried ? 0x1001 : 0x801,command[4],-model_floor(command[4]),
                    carried ? Behavior::carryall : Behavior::hover_turret_base);
                int parent = actor.slot;
                if (carried) {
                    actor.words[20] = 10;
                    parent = spawn(parent,0x40,command[11],command[13],Behavior::hover_turret_base);
                }
                const auto slot = spawn(parent,carried ? 0x40 : 0x1001,command[carried ? 12 : 5],
                    command[carried ? 13 : 6],Behavior::hover_turret_top);
                entity(slot).words[21] = command[carried ? 14 : 7];
                entity(slot).words[3] = command[carried ? 15 : 8]; entity(slot).words[22] = slot;
                continue;
            }
            if (command[0] == 8000 || command[0] == 8001) {
                auto& core = add(command,0x40,command[4],-model_floor(command[4]),Behavior::boss_core);
                core.words[2] = 600; core.words[25] = static_cast<int>(offset);
                objective(core,command[7]);
                state_.boss_destroyed = 0;
                const int turn = command[0] == 8000 ? 205 : 102;
                for (int part = 0; part < 5; ++part) {
                    auto& base = add(command,0x40,command[11],-model_floor(command[11]),Behavior::boss_base);
                    base.words[23] = core.slot; base.words[20] = part * 256;
                    base.words[7] += detail::combat_math::sine(base.words[13]) * 300;
                    base.words[9] += detail::combat_math::cosine(base.words[13]) * 300;
                    const auto slot = spawn(base.slot,0x40,command[12],400,Behavior::boss_top);
                    auto& top = entity(slot).words;
                    top[21] = command[7]; top[3] = command[8]; top[22] = slot;
                    command[3] += turn;
                }
                for (unsigned byte = 0; byte < 4; ++byte) {
                    bytes_.at(commands.offset + offset + 12 + byte) = static_cast<std::uint8_t>(command[3] >> (byte * 8));
                }
                continue;
            }
            if (command[0] == 2000 || command[0] == 2002 || command[0] == 2003 || command[0] == 2004) {
                const auto flags = command[0] == 2000 || command[0] == 2004 ? 1 : (command[0] == 2002 ? 0x4081 : 0x4001);
                auto& result = add(command, flags, command[4], -model_floor(command[4]), Behavior::pose);
                if (command[0] != 2000 && command[0] != 2004) {
                    objective(result, command[7]);
                    continue;
                }
                objective(result, command[7]);
                result.words[21] = command[7];
                result.words[25] = static_cast<std::int32_t>(offset);
                if (command[7] >= 1000 && command[7] < 2000) {
                    ++state_.secrets;
                }
                if (command[0] == 2004) {
                    const auto slot = spawn(result.slot,0x40,command[5],command[6],Behavior::radar);
                    entity(slot).words[20] = command[7] == 6001 ? 1 : command[7] == 6002 ? 2 : 3;
                }
                continue;
            }
            if (command[0] == 7000) {
                add(command, 0x841, command[4], -model_floor(command[4]), Behavior::pose);
                auto& result = add(command, 0x41, command[5], -model_floor(command[5]), Behavior::pickup);
                result.words[21] = command[7];
                objective(result, command[7]);
                ++state_.pickups;
                continue;
            }
            if (command[0] == 4001 || command[0] == 4002) {
                auto& actor = add(command, 0x1001, command[4], -model_floor(command[4]),
                                  command[0] == 4001 ? Behavior::aircraft : Behavior::air_patrol);
                if (command[0] == 4002) { actor.words[21] = command[7]; }
                continue;
            }
            if (command[0] == 3001) {
                auto& actor = add(command, 0x1001, command[4], -model_floor(command[4]), Behavior::pillbox);
                actor.words[21] = command[7];
                actor.words[3] = command[8];
                continue;
            }
            if (command[0] == 6000 || command[0] == 6100 || command[0] == 6101 || command[0] == 6110) {
                auto& actor = add(command, 1, command[4], -model_floor(command[4]),
                                  command[0] == 6000 ? Behavior::tank_factory :
                                  command[0] == 6100 ? Behavior::strafe_factory :
                                  command[0] == 6101 ? Behavior::burst_factory : Behavior::plane_factory);
                actor.words[25] = static_cast<std::int32_t>(offset);
                objective(actor, command[7]);
                if (command[0] != 6000) {
                    const auto child = spawn(actor.slot, 0x40, command[5], command[6], Behavior::radar);
                    entity(child).words[20] = command[0] == 6100 ? 1 : command[0] == 6101 ? 2 : 3;
                }
                continue;
            }
            if (command[0] == 6300 || command[0] == 7001 || command[0] == 7002) {
                const bool spawner = command[0] == 6300;
                auto& actor = add(command, spawner ? 0x40 : 0x841, spawner ? 0 : command[4],
                    spawner ? -model_floor(0) : 0,
                    spawner ? Behavior::intruder_factory : command[0] == 7001 ? Behavior::completion_pickup : Behavior::conditional_pickup);
                if (spawner) { actor.words[2] = 100; }
                actor.words[25] = static_cast<std::int32_t>(offset);
                if (!spawner) { ++state_.pickups; }
                continue;
            }
            if (command[0] == 5500) {
                auto& actor = add(command, 0x1003, command[4], -model_floor(command[4]), Behavior::boat);
                actor.words[3] = command[8];
                const auto child = spawn(actor.slot, 0x1003, command[5], command[6], Behavior::tank_top);
                entity(child).words[21] = command[7];
                entity(child).words[22] = child;
                continue;
            }
            if (command[0] == 5501) {
                auto& actor = add(command,0x1003,command[4],-model_floor(command[4]),Behavior::submarine);
                actor.words[3] = command[8]; actor.words[21] = command[7];
                continue;
            }
            if (command[0] >= 7010 && command[0] <= 7013) {
                const bool ski = command[0] >= 7012;
                auto& actor = add(command, 1, command[4], -model_floor(command[4]), ski ? Behavior::ski_station : Behavior::change_station);
                if ((command[0] & 1) != 0) {
                    actor.words[20] = 5;
                    const auto child = spawn(actor.slot, 0x40, command[5], command[6], Behavior::parked_helicopter);
                    auto& w = entity(child).words;
                    w[19] = 990;
                    w[13] += 64; w[8] += 10000;
                    w[7] -= detail::combat_math::sine(w[13]) * 32;
                    w[9] -= detail::combat_math::cosine(w[13]) * 32;
                }
                continue;
            }
            if (command[0] == 3000 || command[0] == 5000 || command[0] == 5001 || command[0] == 5002 || command[0] == 5003) {
                const bool tank = command[0] != 3000;
                auto& result = add(command, tank ? 0x1003 : 0x4801, command[4], -model_floor(command[4]),
                                   command[0] == 5001 || command[0] == 5003 ? Behavior::amphibious_tank : tank ? Behavior::tank : Behavior::pose);
                const auto parent = result.slot;
                if (tank) {
                    result.words[3] = command[8];
                }
                if (command[0] == 5003) { continue; }
                if (command[0] == 5002) {
                    for (int wheel = 0; wheel < 4; ++wheel) {
                        const auto child = spawn(parent, 0x40, 984, command[6], Behavior::apc_wheel);
                        entity(child).words[20] = wheel;
                    }
                }
                const auto child = spawn(parent, tank ? 0x1003 : 0x1001, command[5], command[6],
                                         tank ? Behavior::tank_top : Behavior::turret_top);
                entity(child).words[21] = command[7];
                entity(child).words[22] = child;
                if (!tank) {
                    entity(child).words[3] = command[8];
                }
                continue;
            }
            throw std::runtime_error("Unsupported mission setup command: " + std::to_string(command[0]));
        }
        if (!ended || !found_player) {
            throw std::runtime_error("Missing mission player or setup terminator");
        }
        if (entity(state_.player).words[26] == static_cast<int>(Behavior::demo_helicopter)) { return; }
        configure_controls(configuration);
    }

    void ensure_combat() {
        if (!combat_) {
            combat_ = std::make_unique<detail::Combat>(state_,
                [this](int x, int z) { return height(x, z); },
                [this](std::uint32_t id) { return resource(id); },
                [this](const EntityState& actor, int size) { deform(actor, size); },
                [this](int x, int z) { return surface_height(x, z); }, std::nullopt,
                [this](int model,int bound) { set_model_bound(model,bound); });
        }
    }

    std::vector<std::uint8_t> profile() const {
        return {request_.mission_profile.begin(), request_.mission_profile.end()};
    }

    FrontendState request_;
    std::vector<std::uint8_t> bytes_;
    std::map<std::uint32_t, Resource> directory_;
    std::map<std::int32_t, std::size_t> entity_indices_;
    MissionState state_;
    MissionState initial_state_;
    std::vector<std::uint8_t> initial_bytes_;
    std::unique_ptr<detail::Combat> combat_;
    detail::Camera camera_;
    bool debug_markers_ = false;
    std::optional<int> mission_menu_yaw_;
    std::uint32_t outcome_tick_ = 0;
    bool results_active_ = false;
    bool frame_counter_reset_pending_ = false;
    FrontendPhase menu_phase_ = FrontendPhase::pilot_selection;
    int pilot_menu_yaw_ = 0, controller_menu_yaw_ = 0;
    int main_menu_yaw_ = 0, level_menu_yaw_ = 0;
    int submenu_yaw_ = 0;
    std::vector<std::uint8_t> damage_;
};

Mission::Mission(const Archive& level, const FrontendState& request, const std::vector<std::uint8_t>& configuration, const Archive* globals)
    : impl_(std::make_unique<Impl>(level, request, configuration, globals)) {}
Mission::~Mission() = default;
MissionState Mission::state() const { return impl_->state(); }
MissionResult Mission::result() const { return impl_->result(); }
std::vector<SoundEvent> Mission::take_sound_events() { return impl_->take_sound_events(); }
void Mission::begin_results() { impl_->begin_results(); }
void Mission::menu_header(const FrontendState& frontend) { impl_->menu_header(frontend); }
void Mission::end_menu() { impl_->end_menu(); }
void Mission::advance_menu() { impl_->advance_menu(); }
std::vector<std::uint8_t> Mission::resource(std::uint32_t id) const { return impl_->resource(id); }
std::int32_t Mission::height(std::int32_t x, std::int32_t z) const { return impl_->height(x, z); }
void Mission::advance_flight(const KeyboardState& keys) { impl_->advance_flight(keys); }
void Mission::reload_background() { impl_->reload_background(); }
void Mission::apply_cheat(int action) { impl_->apply_cheat(action); }
void Mission::configure_controls(const std::vector<std::uint8_t>& configuration) { impl_->configure_controls(configuration); }
void Mission::advance(const KeyboardState& keys, const DeviceInput& devices) { impl_->advance(keys,devices); }
void Mission::finish_event_loop() { impl_->finish_event_loop(); }
void Mission::restart() { impl_->restart(); }
MissionFrame Mission::render(const Archive& globals) { return impl_->render(globals, {}); }
MissionFrame Mission::render(const Archive& globals, RenderTiming timing) { return impl_->render(globals, timing); }
MissionFrame Mission::first_frame() const { return impl_->first_frame(); }
MissionFrame Mission::first_frame(const Archive& globals) const { return impl_->first_frame(globals); }

const char* behavior_name(Behavior behavior) {
    switch (behavior) {
    case Behavior::none: return "none";
    case Behavior::pose: return "OBJECT_pose3";
    case Behavior::helicopter: return "OBJECT_Heli1";
    case Behavior::demo_helicopter: return "OBJECT_Heli1_demo";
    case Behavior::turret_top: return "OBJECT_turret1";
    case Behavior::tank: return "OBJECT_tank1";
    case Behavior::tank_top: return "OBJECT_tank1_top";
    case Behavior::aircraft: return "OBJECT_air1";
    case Behavior::pickup: return "OBJECT_pickup_std";
    case Behavior::bullet: return "OBJECT_RocketT";
    case Behavior::enemy_bullet: return "OBJECT_Rocket1";
    case Behavior::bang: return "OBJECT_bang2";
    case Behavior::explosion: return "OBJECT_bang3";
    case Behavior::smoke: return "OBJECT_smoke1";
    case Behavior::smoker: return "OBJECT_smoker";
    case Behavior::wreck: return "OBJECT_debris1";
    case Behavior::dead_aircraft: return "OBJECT_Dead_Heli1";
    case Behavior::ground_explosion: return "OBJECT_bang1";
    case Behavior::debris_large: return "OBJECT_debris2";
    case Behavior::debris_small: return "OBJECT_debris3";
    case Behavior::aircraft_explosion: return "OBJECT_bang4";
    case Behavior::secret_pickup: return "OBJECT_pickup_sec";
    case Behavior::mounted_weapon: return "WEAPON_ob_std1";
    case Behavior::missile_single: return "WEAPON_ob_active0";
    case Behavior::missile_pair: return "WEAPON_ob_active1";
    case Behavior::missile_coast: return "WEAPON_ob_activeNULL";
    case Behavior::fire_trail: return "OBJECT_firetrail";
    case Behavior::shield: return "WEAPON_ob_shld";
    case Behavior::falling_bullet: return "OBJECT_RocketF";
    case Behavior::homing_bullet: return "OBJECT_RocketH";
    case Behavior::falling_casing: return "WEAPON_fall_off";
    case Behavior::cruise_missile: return "WEAPON_ob_active9";
    case Behavior::scatter_missile: return "WEAPON_ob_active4";
    case Behavior::shard_bomb: return "WEAPON_ob_active60";
    case Behavior::pulse_ring: return "WEAPON_ob_active70";
    case Behavior::pulse_wall: return "WEAPON_ob_active71";
    case Behavior::cluster_missile: return "WEAPON_ob_active7";
    case Behavior::cluster_child: return "WEAPON_ob_active8";
    case Behavior::bomber_missile: return "WEAPON_ob_active5";
    case Behavior::carpet_missile: return "WEAPON_ob_active15";
    case Behavior::bomblet: return "WEAPON_ob_active3";
    case Behavior::rocket_buddy: return "WEAPON_ob_buddy1";
    case Behavior::tracking_buddy: return "WEAPON_ob_buddy2";
    case Behavior::bomber_buddy: return "WEAPON_ob_buddy3";
    case Behavior::buddy_bomb: return "WEAPON_ob_active6";
    case Behavior::dummy_craft: return "OBJECT_dummycraft";
    case Behavior::apc_wheel: return "OBJECT_apcwheel";
    case Behavior::pillbox: return "OBJECT_pillbox1";
    case Behavior::tank_factory: return "OBJECT_spawn_tank1";
    case Behavior::plane_factory: return "OBJECT_spawn_plane2";
    case Behavior::strafe_factory: return "OBJECT_spawn_plane1";
    case Behavior::strafe_plane: return "OBJECT_plane1";
    case Behavior::boat: return "OBJECT_boat1";
    case Behavior::change_station: return "OBJECT_changeHB";
    case Behavior::parked_helicopter: return "OBJECT_dummyobj_h";
    case Behavior::burst_factory: return "OBJECT_spawn_plane11";
    case Behavior::intruder_factory: return "OBJECT_invspawn";
    case Behavior::intruder: return "OBJECT_is_heli";
    case Behavior::conditional_pickup: return "OBJECT_pickup_conditional1";
    case Behavior::completion_pickup: return "OBJECT_pickup_conditional";
    case Behavior::amphibious_tank: return "OBJECT_tank2";
    case Behavior::boss_core: return "OBJECT_boss1_c";
    case Behavior::boss_base: return "OBJECT_boss1_tb";
    case Behavior::boss_top: return "OBJECT_boss1_tt";
    case Behavior::boss_air: return "OBJECT_boss1_air";
    case Behavior::drone: return "OBJECT_drone1";
    case Behavior::drone_gun: return "OBJECT_drone1_gun";
    case Behavior::slug_drone: return "OBJECT_slugdrone";
    case Behavior::grav_tank: return "OBJECT_gravtank1";
    case Behavior::grav_top: return "OBJECT_gravtank1_top";
    case Behavior::carryall: return "OBJECT_carryall";
    case Behavior::hover_turret_base: return "OBJECT_turret_h1b";
    case Behavior::hover_turret_top: return "OBJECT_turret_h1t";
    case Behavior::ground_exit: return "OBJECT_finnB";
    case Behavior::buggy_exit: return "OBJECT_changeover_buggy2";
    case Behavior::stationary_tank: return "OBJECT_stank1";
    case Behavior::stationary_top: return "OBJECT_stank1_top";
    case Behavior::trailer: return "OBJECT_trailer";
    case Behavior::trailer_missile: return "OBJECT_trailer_mis";
    case Behavior::boss2_core: return "OBJECT_boss2_c";
    case Behavior::boss2_top: return "OBJECT_boss2_tt";
    case Behavior::pyramid: return "OBJECT_pmid1";
    case Behavior::pyramid_gun: return "OBJECT_pmid1gun";
    case Behavior::pyramid_eye: return "OBJECT_boss3eye";
    case Behavior::pyramid_shot: return "OBJECT_boss3_p";
    case Behavior::pyramid_dive: return "OBJECT_boss3_pd";
    case Behavior::mars_face: return "OBJECT_marsface";
    case Behavior::teleport: return "OBJECT_teleport";
    case Behavior::hover: return "OBJECT_Hover1";
    case Behavior::ski_station: return "OBJECT_changeHS";
    case Behavior::ski_change: return "OBJECT_changeover_ski";
    case Behavior::submarine: return "OBJECT_sub1";
    case Behavior::helicopter_change: return "OBJECT_changeover_heli_fly";
    case Behavior::buggy_change: return "OBJECT_changeover_buggy";
    case Behavior::ground_buggy: return "OBJECT_Ground_R1";
    case Behavior::ground_ski: return "OBJECT_Ground_R2";
    case Behavior::wheel: return "OBJECT_wheel";
    case Behavior::radar: return "OBJECT_radar_top";
    case Behavior::air_patrol: return "OBJECT_air2";
    case Behavior::attack_plane: return "OBJECT_plane2";
    case Behavior::enemy_missile: return "WEAPON_ob_active2";
    case Behavior::polaris_missile: return "WEAPON_ob_polaris1";
    }
    throw std::runtime_error("Unknown object behavior");
}

}
