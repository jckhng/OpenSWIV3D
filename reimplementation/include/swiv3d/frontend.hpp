#pragma once

#include "swiv3d/assets.hpp"
#include "swiv3d/controller.hpp"

#include <memory>
#include <string>

namespace swiv3d {

constexpr int initial_debug_counter = -123;
constexpr unsigned credit_page_count = 3;
constexpr unsigned pause_scan = 0x9d;

enum class FrontendPhase {
    pilot_selection,
    pilot_name,
    controller_type,
    controller_bindings,
    main_menu,
    briefing,
    mission_request,
    mission_results,
    level_selection,
    settings,
    pilot_deletion,
    key_capture,
    sound_volume,
    language_selection,
    gamma_selection,
    music_selection,
    axis_bindings,
    cheat_menu,
    opening_movies,
    credits,
    exit_confirmation,
    quit,
    mission_pause,
    mission_abort,
    mission_grab,
    grab_name
};

enum class KeyShift : std::uint32_t { released, pressed };
enum class GrabAction { none, increment, save };

enum class MissionOutcome { failed, completed };

struct MenuRow {
    std::string text;
    std::int32_t value = 0;
    std::int32_t colour = 0;
    std::int32_t kind = 0;
    std::int32_t option = 0;
    bool selection_bottom_border = true;
};

struct LandscapeClock {
    std::uint32_t rotor = 0;
    std::uint32_t water = 0;
};

struct MissionResult {
    MissionOutcome outcome;
    std::array<std::uint8_t, 112> profile{};
    std::uint32_t elapsed_updates = 0;
    LandscapeClock landscape_clock{};
    std::int32_t objective_heading = 0;
    std::uint32_t control_ticks = 0;
    std::uint32_t effects_phase = 0;
    std::int32_t engine_pitch = 0;
    std::int32_t weapon_progress = 0;
    std::array<int, 5> direction_steps{-150,-2,0,2,150};
    std::array<std::int32_t, 3> flight_attitude{};
    std::int32_t node_range = 0;
    std::int32_t level_score = 0;
    std::array<std::int32_t, 3> ratios{};
    std::uint32_t game_counter = 0;
};

struct FrontendState {
    FrontendPhase phase = FrontendPhase::pilot_selection;
    std::int32_t selection = -1;
    std::int32_t width = 0;
    std::int32_t player = -1;
    std::uint32_t language = 0;
    std::uint32_t music_type = 0;
    std::uint32_t level = 0;
    std::uint32_t water = 3;
    std::uint32_t craft = 0;
    LandscapeClock landscape_clock;
    std::int32_t objective_heading = 0;
    std::uint32_t control_ticks = 0;
    std::uint32_t effects_phase = 0;
    std::int32_t engine_pitch = 0;
    std::int32_t weapon_progress = 0;
    std::array<int, 5> direction_steps{-150,-2,0,2,150};
    std::array<std::int32_t, 3> flight_attitude{};
    std::int32_t node_range = 0;
    std::size_t sequence_offset = 0;
    std::string movie;
    std::string level_file;
    std::array<std::uint32_t, 6> tracks{9, 3, 2, 2, 2, 2};
    std::array<std::uint8_t, 112> mission_profile{};
    std::uint32_t gamma = 0;
    std::array<int, 3> volumes{-1,-1,255};
    int screen_size = 255;
    int detail = 240;
    bool debug_controls = false;
    bool cheats_unlocked = false;
    bool cheat_mode = false;
    bool mission_menu = false;
    int debug_counter = initial_debug_counter;
    unsigned credit_page = 0;
    std::uint32_t game_counter = 0;
    bool grab_statistics = true;
    std::string grab_name;
};

// Semantic frontend with retained landscape animation clocks. The caller supplies
// completed landscape updates; device scheduling and menu drawing remain separate.
class Frontend {
public:
    Frontend(const Archive& globals, const std::vector<std::uint8_t>& configuration);
    ~Frontend();
    // Consume one key through the next logical editor/menu boundary.
    void press(std::uint32_t scan, KeyShift shift = KeyShift::released);
    // Count completed menu draws before delivering live queued keys.
    void presented();
    bool accepts_queued_key(std::uint32_t scan) const;
    void devices(DeviceCapabilities capabilities);
    int take_cheat();
    bool take_abort();
    GrabAction take_grab();
    std::vector<int> take_menu_sounds();
    // Original active event-loop ticks, excluding movies and credit-page waits.
    void advance_clock(std::uint32_t updates);
    void finish_presentation();
    void advance_landscape_clock(std::uint32_t updates);
    void begin_results(const MissionResult& result);
    void advance_results(std::uint32_t updates);
    // Return the configuration file written at mission exit, before dispatching
    // the successor sequence. The live configuration may then gain a checkpoint.
    std::vector<std::uint8_t> finish_mission(const MissionResult& result);
    FrontendState state() const;
    std::vector<MenuRow> rows() const;
    std::vector<std::uint8_t> configuration() const;
    std::vector<std::uint8_t> configuration_file() const;

private:
    class Impl;
    std::unique_ptr<Impl> impl_;
};

const char* phase_name(FrontendPhase phase);

}
