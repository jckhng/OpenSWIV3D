#include "checkpoint_fixture.hpp"

#include <iostream>
#include <chrono>
#include "../src/profile_file.hpp"

namespace {

void require(bool condition, const char* message) {
    if (!condition) { throw std::runtime_error(message); }
}

void complete_movie(swiv3d::detail::MissionSession& session) {
    require(session.cutscene != nullptr, "No active presentation");
    const auto count = session.cutscene->frames();
    const auto duration = session.cutscene->frame_duration();
    const auto updates = (static_cast<std::uint64_t>(count) * duration + 24999) / 25000;
    for (std::uint64_t tick = 0; tick < updates; ++tick) {
        require(session.cutscene != nullptr, "Presentation exited before its final hold");
        const auto image = swiv3d::detail::render_session(session);
        require(image.pixels.size() == swiv3d::image_width * swiv3d::image_height, "Incomplete presentation frame");
        swiv3d::detail::mix_session(session, tick % 2 == 0 ? 1102 : 1103);
        swiv3d::detail::advance_session(session, {});
    }
    require(!session.cutscene, "Presentation retained after natural EOF");
}

void queue_ending(swiv3d::detail::MissionSession& session) {
    auto request = scenario::campaign_checkpoint(session, 16);
    auto level = session.game / request.level_file;
    level.replace_extension(".SLV");
    session.mission = std::make_unique<swiv3d::Mission>(swiv3d::Archive::open(level), request,
                                                     session.frontend->configuration(), &session.globals);
    swiv3d::MissionResult completed{swiv3d::MissionOutcome::completed};
    completed.profile = request.mission_profile;
    session.frontend->begin_results(completed);
    session.frontend->finish_mission(completed);
    require(session.frontend->state().phase == swiv3d::FrontendPhase::briefing && session.frontend->state().movie == "OUTRO",
            "Final results bypassed OUTRO");
    swiv3d::detail::frontend_key(session, 0);
}

void startup(const std::filesystem::path& game, const std::filesystem::path& script) {
    using namespace swiv3d;
    const auto profile = std::filesystem::temp_directory_path() /
        ("swiv3d-menu-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()) + ".CFG");
    auto closed = detail::start_frontend(game,profile);
    detail::abort_mission(closed);
    auto legacy = detail::start_mission(game,script,profile);
    require(legacy.mission != nullptr, "Scripted startup rejected a saved configuration without a pilot");
    std::filesystem::remove(profile);
    auto session = detail::start_frontend(game);
    require(session.opening_movie == 1 && !session.mission, "Opening startup bypassed INTRO1");
    const auto updates = (static_cast<std::uint64_t>(session.cutscene->frames()) * session.cutscene->frame_duration() + 24999) / 25000;
    for (std::uint64_t tick = 0; tick < updates; ++tick) { detail::advance_session(session,{}); }
    require(session.opening_movie == 2 && !session.mission && session.cutscene->frames() == 957,
            "Natural INTRO1 completion bypassed INTRO2");
    complete_movie(session);
    require(session.opening_movie == 0 && session.frontend->state().phase == FrontendPhase::pilot_selection &&
            session.mission && session.mission->state().tick == 0, "Opening sequence lost first demo entry");
    for (unsigned tick = 0; tick < 50; ++tick) { detail::advance_session(session,{}); detail::render_session(session); }
    const auto demo = session.mission->state();
    require(demo.entities.front().words[26] == static_cast<int>(Behavior::demo_helicopter) &&
            demo.entities.front().words[7] != 4031232 && demo.landscape_clock.water == 50, "Menu demo stopped moving");
    detail::frontend_key(session,0x1c);
    for (unsigned tick = 0; tick < 16; ++tick) { detail::advance_session(session,{}); detail::render_session(session); }
    require(session.mission->state().tick == demo.tick && session.mission->state().landscape_clock.water == 50 &&
            session.mission->state().entities.front().words[7] == demo.entities.front().words[7] &&
            session.frontend->state().landscape_clock.water == 50, "Modal name entry advanced the demo world");
    for (unsigned scan : {0x14U,0x12U,0x1fU,0x14U,0x1cU}) { detail::frontend_key(session,scan); }
    require(session.frontend->state().phase == FrontendPhase::controller_type, "Live pilot creation lost controller setup");
    detail::frontend_key(session,0x1c);
    for (unsigned scan : {0xd0U,0xd0U,0xd0U,0x1cU,0x11U}) { detail::frontend_key(session,scan); }
    require(session.frontend->state().phase == FrontendPhase::controller_bindings && session.frontend->state().selection == 24,
            "Live key prompt did not return to the forward binding");
    detail::frontend_key(session,0x01);
    require(session.frontend->state().phase == FrontendPhase::main_menu, "Live keyboard setup failed to return to main menu");
    session.profile = profile;
    detail::abort_mission(session);
    require(detail::load_profile(profile) == session.frontend->configuration_file(), "Closing menu lost pilot configuration");
    std::filesystem::remove(profile);
    session.profile.clear();
    detail::frontend_key(session,0x1c);
    require(session.cutscene && session.frontend->state().movie == "BR_TER0", "Main menu bypassed mission briefing");
    complete_movie(session);
    require(session.mission->state().tick == 0 && session.frontend->state().phase == FrontendPhase::mission_request,
            "Live briefing failed to construct first mission");
    require(session.mission->state().flight.bindings[2] == 0x11,"Mission entry lost remapped forward control");
    KeyboardState forward{};
    forward[0x11] = true;
    detail::advance_session(session,forward);
    require(session.mission->state().flight.attitude[1] < 0,"Remapped control failed through live mission dispatch");
    for (auto end : {detail::CutsceneEnd::skipped,detail::CutsceneEnd::finished}) {
        auto skipped = detail::start_frontend(game);
        if (end == detail::CutsceneEnd::finished) { detail::complete_cutscene(skipped,end); }
        KeyboardState key{};
        key[0x01] = true;
        detail::advance_session(skipped,key);
        require(skipped.opening_movie == 0 && !skipped.cutscene && skipped.mission,
                "Opening key skip failed to bypass remaining movies");
    }
}

}

int main(int argc, char** argv) {
    try {
        require(argc == 3, "Expected GAME FRONTEND");
        using namespace swiv3d;
        startup(argv[1],argv[2]);
        auto natural = detail::start_mission(argv[1], argv[2], {}, detail::MoviePlayback::play);
        require(natural.cutscene && !natural.mission && natural.frontend->state().movie == "BR_TER0", "Initial briefing was skipped");
        const auto clock = natural.frontend->state().landscape_clock;
        const auto cd_timer = natural.cd_timer;
        complete_movie(natural);
        require(natural.mission && natural.mission->state().tick == 0, "Briefing advanced the mission");
        const auto entered = natural.frontend->state();
        require(entered.landscape_clock.rotor == clock.rotor && entered.landscape_clock.water == clock.water,
                "Briefing advanced the background landscape clock");
        require(natural.cd_timer == cd_timer, "Briefing advanced the landscape CD poll counter");
        auto skipped = detail::start_mission(argv[1], argv[2], {}, detail::MoviePlayback::play);
        KeyboardState keys{};
        keys[0x1d] = true;
        detail::advance_session(skipped, keys);
        require(!skipped.cutscene && skipped.mission && skipped.mission->state().tick == 0, "Any-key skip failed");
        require(skipped.frontend->state().mission_profile == entered.mission_profile &&
                skipped.frontend->state().sequence_offset == entered.sequence_offset, "Skip changed campaign entry");
        queue_ending(natural);
        const auto before = natural.frontend->configuration();
        require(natural.cutscene != nullptr, "OUTRO was skipped on session dispatch");
        complete_movie(natural);
        require(natural.frontend->state().phase == FrontendPhase::main_menu && natural.frontend->state().selection == 3,
                "OUTRO failed to return to completed-pilot menu");
        require(natural.frontend->configuration() != before, "Natural OUTRO completion lost completed-pilot marker");
        queue_ending(skipped);
        keys.fill(false);
        keys[0x01] = true;
        detail::advance_session(skipped, keys);
        require(!skipped.cutscene && skipped.frontend->state().phase == FrontendPhase::main_menu &&
                skipped.frontend->state().selection == 3, "OUTRO Escape failed to return to menu");
        require(skipped.frontend->configuration() == natural.frontend->configuration(), "OUTRO skip changed completed profile");
        std::cout << "Opening movies, live demo and modal menus, profile save, briefing/OUTRO natural completion and skips passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
