#include "input_script.hpp"
#include "profile_file.hpp"
#include "screen_grab.hpp"
#include "menu_render.hpp"

#include <algorithm>
#include <fstream>
#include <iterator>
#include <sstream>
#include <stdexcept>

namespace swiv3d::detail {
namespace {

bool line(std::istream& input, std::string& value) {
    if (!std::getline(input, value)) {
        return false;
    }
    if (!value.empty() && value.back() == '\r') {
        value.pop_back();
    }
    return true;
}

std::uint32_t number(const std::string& text) {
    std::size_t end = 0;
    const auto value = std::stoull(text, &end, 0);
    if (end != text.size() || value > UINT32_MAX || text.front() == '-') {
        throw std::runtime_error("Invalid input number");
    }
    return static_cast<std::uint32_t>(value);
}

MissionSession make_session(const std::filesystem::path& game, const std::filesystem::path& profile, MoviePlayback movies) {
    if (!profile.empty() && std::filesystem::weakly_canonical(profile) == std::filesystem::weakly_canonical(game / "SWIV.CFG")) {
        throw std::runtime_error("Native profiles must be separate from the preserved specimen");
    }
    auto globals = Archive::open(game / "GLOBAL.BD");
    const auto bytes = load_profile(!profile.empty() && std::filesystem::exists(profile) ? profile : game / "SWIV.CFG");
    auto frontend = std::make_unique<Frontend>(globals,bytes);
    MissionSession session{std::move(globals),{},std::move(frontend),game,profile,{},{},0,movies,{},FrontendPlayback::scripted,0,0};
    session.audio = std::make_unique<Audio>(Archive::open(game / "SOUNDS.BD"));
    return session;
}

enum class DemoPlayback { initialize, reload };

MissionFrame grab_frame(MissionSession& session) {
    const auto state = session.frontend->state();
    RenderTiming timing;
    timing.frames_per_second = session.render_rate;
    timing.overlays = state.grab_statistics ? OverlayMode::statistics : OverlayMode::none;
    timing.screen_size = state.screen_size; timing.detail = state.detail;
    timing.debug_controls = state.debug_controls; timing.debug_counter = state.debug_counter;
    auto frame = session.mission->render(session.globals,timing);
    apply_gamma(frame,state.gamma);
    return frame;
}

void load_demo(MissionSession& session, DemoPlayback playback = DemoPlayback::initialize) {
    auto request = session.frontend->state();
    request.phase = FrontendPhase::mission_request;
    request.level_file = "DEMO.slv";
    request.player = 0;
    request.craft = 0;
    request.water = 3;
    request.mission_profile.fill(0);
    constexpr unsigned demo_health = 400;
    request.mission_profile[48] = demo_health & 255;
    request.mission_profile[49] = demo_health >> 8;
    if (playback == DemoPlayback::reload) { session.mission->reload_background(); }
    else {
        session.mission = std::make_unique<Mission>(Archive::open(session.game / "DEMO.SLV"),request,
                                                   session.frontend->configuration(),&session.globals);
    }
    session.mission->menu_header(session.frontend->state());
    if (session.audio) { session.audio->stop(); session.audio->music_track(request.tracks.at(request.music_type)); }
}

void load_mission(MissionSession& session) {
    session.cutscene.reset();
    while (session.frontend->state().phase == FrontendPhase::briefing) {
        if (session.movies == MoviePlayback::play) {
            const auto request = session.frontend->state();
            session.cutscene = std::make_unique<Cutscene>(session.game / "ANIMS", request.movie, session.globals, request);
            if (session.audio) { session.audio->stop(); }
            if (session.audio) { session.audio->stop_music(); session.cutscene->attach_audio(*session.audio); }
            return;
        }
        session.frontend->press(0x01);
    }
    auto request = session.frontend->state();
    if (request.phase != FrontendPhase::mission_request) { return; }
    const auto craft = static_cast<CraftKind>(request.mission_profile[44]);
    if (!session.mission && (craft == CraftKind::buggy || craft == CraftKind::ski)) { load_demo(session); }
    if (session.mission) { request.flight_attitude = session.mission->state().flight.attitude; }
    auto path = session.game / request.level_file;
    path.replace_extension(".SLV");
    session.mission = std::make_unique<Mission>(Archive::open(path), request, session.frontend->configuration(), &session.globals);
    if (session.audio) { session.audio->stop(); session.audio->music_track(request.tracks.at(request.music_type)); }
}

}

void replay_frontend(Frontend& frontend, std::istream& script, const std::function<void()>& checkpoint, MoviePlayback movies) {
    std::string header;
    line(script, header);
    const bool clock_events = header == "swiv3d-frontend-input-v2";
    if (!clock_events && header != "swiv3d-frontend-input-v1") {
        throw std::runtime_error("Unsupported frontend input script");
    }
    if (checkpoint) {
        checkpoint();
    }
    std::string record;
    while (line(script, record)) {
        if (movies == MoviePlayback::play && frontend.state().phase == FrontendPhase::briefing) { break; }
        if (record.empty() || record.front() == '#') {
            continue;
        }
        std::istringstream input(record);
        std::string phase, scan, value, extra;
        if (!(input >> phase >> scan >> value) || (input >> extra) || phase != phase_name(frontend.state().phase)) {
            throw std::runtime_error("Frontend input phase/record mismatch");
        }
        if (clock_events && scan == "landscape-updates") {
            frontend.advance_landscape_clock(number(value));
        } else {
            if (value != "0" && value != "1") {
                throw std::runtime_error("Invalid key shift state");
            }
            frontend.press(number(scan), value == "0" ? KeyShift::released : KeyShift::pressed);
        }
        if (checkpoint) {
            checkpoint();
        }
    }
}

std::vector<FlightSegment> read_flight(std::istream& input) {
    std::string record;
    line(input, record);
    if (record != "swiv3d-flight-input-v1") {
        throw std::runtime_error("Unsupported flight input script");
    }
    std::vector<FlightSegment> result;
    while (line(input, record)) {
        if (record.empty() || record.front() == '#') {
            continue;
        }
        std::istringstream row(record);
        std::string updates, held, extra;
        if (!(row >> updates >> held) || (row >> extra)) {
            throw std::runtime_error("Invalid flight input record");
        }
        FlightSegment segment{number(updates), {}};
        constexpr std::uint32_t maximum_segment_updates = 10000;
        if (segment.updates == 0 || segment.updates > maximum_segment_updates) {
            throw std::runtime_error("Flight input segment exceeds update bound");
        }
        if (held != "-") {
            std::istringstream codes(held);
            std::string code;
            while (std::getline(codes, code, '+')) {
                const auto scan = number(code);
                if (scan >= segment.keys.size() || segment.keys[scan]) {
                    throw std::runtime_error("Invalid or repeated flight scan code");
                }
                segment.keys[scan] = true;
            }
        }
        result.push_back(segment);
    }
    if (result.empty()) {
        throw std::runtime_error("Flight input script is empty");
    }
    return result;
}

MissionSession start_mission(const std::filesystem::path& game, const std::filesystem::path& input, const std::filesystem::path& profile, MoviePlayback movies) {
    auto session = make_session(game,profile,movies);
    const bool resume = !profile.empty() && std::filesystem::exists(profile);
    auto& frontend = session.frontend;
    const auto rows = frontend->rows();
    const auto pilot = std::find_if(rows.begin(),rows.end(),[](const auto& row) { return row.kind == 0 && row.value >= 100; });
    if (resume && pilot != rows.end()) {
        while (frontend->state().selection != pilot->value) { frontend->press(0xd0); }
        frontend->press(0x1c);
        if (frontend->state().phase != FrontendPhase::main_menu) { throw std::runtime_error("Saved pilot requires controller setup"); }
        frontend->press(0x1c);
        if (frontend->state().phase == FrontendPhase::level_selection) { frontend->press(0x1c); }
    } else {
        std::ifstream script(input);
        replay_frontend(*frontend, script, {}, movies);
    }
    frontend->take_menu_sounds();
    load_mission(session);
    if (!session.mission && !session.cutscene) {
        throw std::runtime_error("Frontend script did not reach a mission request");
    }
    return session;
}

MissionSession start_frontend(const std::filesystem::path& game, const std::filesystem::path& profile) {
    auto session = make_session(game,profile,MoviePlayback::play);
    session.frontend_playback = FrontendPlayback::interactive;
    session.opening_movie = 1;
    session.cutscene = std::make_unique<Cutscene>(game / "ANIMS","INTRO1",session.globals,session.frontend->state());
    if (session.audio) { session.audio->stop_music(); session.cutscene->initialize_audio(*session.audio); }
    return session;
}

void complete_cutscene(MissionSession& session, CutsceneEnd end) {
    session.movie_video_work = 0;
    if (session.opening_movie == 1 && end == CutsceneEnd::finished) {
        auto previous = session.cutscene->render();
        session.cutscene.reset();
        session.opening_movie = 2;
        session.cutscene = std::make_unique<Cutscene>(session.game / "ANIMS","INTRO2",session.globals,session.frontend->state());
        session.movie_preparation = session.cutscene->preparation();
        session.movie_preparation_work = session.movie_preparation.remaining();
        session.movie_preparation_frame = std::move(previous);
        if (session.audio) { session.audio->stop_music(); }
        return;
    }
    session.movie_preparation_work = 0;
    session.movie_preparation = {};
    session.movie_preparation_frame = {};
    if (session.opening_movie != 0) {
        session.opening_movie = 0;
        session.cutscene.reset();
        const bool replay = session.frontend->state().phase == FrontendPhase::opening_movies;
        if (replay) { session.frontend->finish_presentation(); }
        load_demo(session,replay ? DemoPlayback::reload : DemoPlayback::initialize);
        return;
    }
    frontend_key(session,0x01);
}

void begin_results(MissionSession& session) {
    session.frontend->begin_results(session.mission->result());
    session.mission->begin_results();
}

void finish_mission(MissionSession& session) {
    if (session.audio) { session.audio->stop(); }
    save_profile(session.profile, session.frontend->finish_mission(session.mission->result()));
    load_mission(session);
    const auto state = session.frontend->state();
    if (session.frontend_playback == FrontendPlayback::interactive && state.phase == FrontendPhase::main_menu) { load_demo(session); return; }
    if (session.audio && state.phase == FrontendPhase::main_menu) { session.audio->music_track(state.tracks.at(state.music_type)); }
}

void frontend_key(MissionSession& session, std::uint32_t scan, KeyShift shift, const KeyboardState& keyboard) {
    const bool movie = session.cutscene != nullptr;
    const bool retained_mission = session.frontend->state().mission_menu || session.frontend->state().phase == FrontendPhase::mission_request;
    const auto previous = session.frontend->state().phase;
    const auto previous_music = session.frontend->state().music_type;
    if (previous == FrontendPhase::mission_grab && (scan == 0x1c || scan == 0x9c || scan == 0x01 || scan == 0x3b)) {
        // rendermenu_ redraws the world once before returning a grab action.
        grab_frame(session);
    }
    session.frontend->press(scan,shift);
    const auto menu_sounds = session.frontend->take_menu_sounds();
    if (session.audio && !menu_sounds.empty()) {
        std::vector<SoundEvent> events;
        for (const auto sound : menu_sounds) { events.push_back({0,no_entity,sound,SoundAction::effect}); }
        session.audio->enqueue(events);
    }
    if (session.frontend->take_abort()) {
        abort_mission(session);
        load_demo(session);
        return;
    }
    const auto phase = session.frontend->state().phase;
    if (phase == FrontendPhase::opening_movies) {
        session.opening_movie = 1;
        session.cutscene.reset();
        session.cutscene = std::make_unique<Cutscene>(session.game / "ANIMS","INTRO1",session.globals,session.frontend->state());
        if (session.audio) { session.audio->stop(); session.audio->stop_music(); session.cutscene->attach_audio(*session.audio); }
        return;
    }
    if (phase == FrontendPhase::credits || phase == FrontendPhase::quit) {
        if (session.audio) { session.audio->stop(); session.audio->stop_music(); }
        return;
    }
    if (previous == FrontendPhase::credits) { load_demo(session,DemoPlayback::reload); return; }
    if (!retained_mission) { load_mission(session); }
    const auto state = session.frontend->state();
    const auto cheat = session.frontend->take_cheat();
    const auto grab = session.frontend->take_grab();
    if (grab == GrabAction::increment) { session.mission->advance(keyboard); }
    if (grab == GrabAction::save) {
        session.mission->end_menu();
        save_screen_grab(session.profile.parent_path() / "swivgrab" / (state.grab_name + ".bmp"),grab_frame(session));
    }
    if (session.frontend_playback == FrontendPlayback::interactive && previous == FrontendPhase::level_selection && state.phase == FrontendPhase::main_menu) {
        session.mission->reload_background();
        if (session.audio) { session.audio->stop(); session.audio->music_track(state.tracks.at(state.music_type)); }
    }
    if (cheat >= 0) { session.mission->apply_cheat(cheat); }
    if (retained_mission && previous != FrontendPhase::mission_request && state.phase == FrontendPhase::mission_request) {
        session.mission->configure_controls(session.frontend->configuration());
        session.mission->end_menu();
    }
    const bool mission_modal = state.phase == FrontendPhase::mission_pause || state.phase == FrontendPhase::mission_abort ||
        state.phase == FrontendPhase::mission_grab || state.phase == FrontendPhase::grab_name;
    if ((session.frontend_playback == FrontendPlayback::interactive || mission_modal) && (state.phase != previous ||
        (scan == 0x1c && (state.phase == FrontendPhase::axis_bindings || state.phase == FrontendPhase::level_selection ||
                         state.phase == FrontendPhase::pilot_name || (state.phase == FrontendPhase::settings && state.selection == 15)))) &&
        state.phase != FrontendPhase::briefing && state.phase != FrontendPhase::mission_request) {
        if (movie) { load_demo(session); return; }
        else { session.mission->menu_header(state); }
    }
    if (session.audio && state.music_type != previous_music) { session.audio->music_track(state.tracks.at(state.music_type)); }
    if (movie && session.audio && state.phase == FrontendPhase::main_menu) { session.audio->music_track(state.tracks.at(state.music_type)); }
}

void abort_mission(MissionSession& session) {
    if (session.frontend->state().phase != FrontendPhase::mission_request && !session.frontend->state().mission_menu) {
        if (session.frontend_playback == FrontendPlayback::interactive) { save_profile(session.profile,session.frontend->configuration_file()); }
        return;
    }
    const auto state = session.mission->state();
    MissionResult result{MissionOutcome::failed};
    result.elapsed_updates = state.tick;
    result.landscape_clock = state.landscape_clock;
    result.game_counter = state.game_counter;
    result.objective_heading = state.objective_heading;
    result.control_ticks = state.flight.control_ticks;
    result.effects_phase = state.effects_phase;
    result.engine_pitch = state.flight.engine_pitch;
    result.weapon_progress = state.weapons.progress;
    result.direction_steps = state.direction_steps;
    result.flight_attitude = state.flight.attitude;
    result.node_range = state.node_range;
    save_profile(session.profile, session.frontend->finish_mission(result));
}

}
