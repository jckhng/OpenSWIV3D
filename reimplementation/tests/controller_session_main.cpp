#include "../src/input_script.hpp"
#include "../src/profile_file.hpp"

#include <algorithm>
#include <fstream>
#include <iostream>
#include <stdexcept>

namespace {
constexpr unsigned menu_confirmation_draws = 4;

void require(bool value,const char* message) {
    if (!value) { throw std::runtime_error(message); }
}
void select(swiv3d::detail::MissionSession& session,int action) {
    for (int i = 0; i < 40 && session.frontend->state().selection != action; ++i) { swiv3d::detail::frontend_key(session,0xd0); }
    require(session.frontend->state().selection == action,"Menu action is unreachable");
    swiv3d::detail::frontend_key(session,0x1c);
}
void save_frame(const swiv3d::MissionFrame& frame,const std::filesystem::path& output,const std::string& name) {
    if (output.empty()) { return; }
    std::filesystem::create_directories(output);
    std::ofstream pixels(output / (name + ".bin"),std::ios::binary);
    pixels.write(reinterpret_cast<const char*>(frame.pixels.data()),frame.pixels.size());
    std::ofstream palette(output / (name + "-dac.bin"),std::ios::binary);
    palette.write(reinterpret_cast<const char*>(frame.palette.data()),frame.palette.size());
    if (!pixels || !palette) { throw std::runtime_error("Cannot write presentation fixture"); }
}
void finish_movie(swiv3d::detail::MissionSession& session) {
    const auto duration = static_cast<std::uint64_t>(session.cutscene->frames()) * session.cutscene->frame_duration();
    constexpr unsigned update_us = 1000000 / swiv3d::mission_tick_rate;
    const auto updates = (duration + update_us - 1) / update_us;
    for (std::uint64_t tick = 0; tick < updates; ++tick) {
        swiv3d::detail::mix_session_device(session,swiv3d::detail::device_audio_rate / swiv3d::mission_tick_rate);
        swiv3d::detail::advance_session(session,{});
    }
    while (session.movie_preparation_work != 0) {
        swiv3d::detail::mix_session_device(session,swiv3d::detail::device_audio_rate / swiv3d::mission_tick_rate);
        swiv3d::detail::advance_session(session,{});
    }
}

void test_menu_input_delay(const char* game) {
    using namespace swiv3d;
    for (unsigned enter : {0x1cU,0x9cU}) {
        auto session = detail::start_frontend(game);
        detail::complete_cutscene(session,detail::CutsceneEnd::skipped);
        session.movies = detail::MoviePlayback::skip;
        const auto press = [&](unsigned scan) {
            KeyboardState keys{}; keys[scan] = true;
            detail::advance_session(session,keys);
            detail::advance_session(session,{});
        };
        for (unsigned draws = 1; draws <= menu_confirmation_draws; ++draws) {
            detail::render_session(session);
            press(enter);
            require(session.frontend->state().phase == (draws < menu_confirmation_draws ? FrontendPhase::pilot_selection : FrontendPhase::pilot_name),
                    "Queued Enter bypassed the original four-draw menu guard");
        }
        press(0x1e); press(0x1c);
        require(session.frontend->state().phase == FrontendPhase::controller_type,"Name editor inherited the menu Enter delay");
        press(enter);
        require(session.frontend->state().phase == FrontendPhase::controller_type,"New controller menu retained the previous input age");
        for (unsigned draws = 0; draws < menu_confirmation_draws; ++draws) { detail::render_session(session); }
        press(enter); press(0x01);
        require(session.frontend->state().phase == FrontendPhase::main_menu,"Menu cancellation inherited the Enter delay");
        KeyboardState keys{}; keys[enter] = true; keys[0xd0] = true;
        detail::advance_session(session,keys);
        require(session.frontend->state().phase == FrontendPhase::main_menu && session.frontend->state().selection == 4,
                "Ignored early Enter consumed the following menu navigation");
    }
}

void test_viewport_shortcuts(const char* game) {
    using namespace swiv3d;
    const auto start = [&] {
        auto session = detail::start_frontend(game);
        detail::complete_cutscene(session,detail::CutsceneEnd::skipped);
        session.movies = detail::MoviePlayback::skip;
        for (unsigned scan : {0x1cU,0x1eU,0x1cU,0x1cU,0x01U,0x1cU}) { detail::frontend_key(session,scan); }
        require(session.frontend->state().phase == FrontendPhase::mission_request,"Viewport fixture did not enter a mission");
        return session;
    };
    auto session = start();
    auto control = start();
    KeyboardState keys{}; keys[0x4a] = true;
    const auto drawn = detail::render_session(session,{},keys);
    const auto reference = detail::render_session(control);
    require(session.frontend->state().screen_size == 250 && drawn.pixels == reference.pixels,
            "Viewport shortcut changed the camera that was already drawn");
    require(detail::render_session(session,{},keys).pixels != detail::render_session(control).pixels &&
            session.frontend->state().screen_size == 245,"Held viewport shortcut did not resize the following frame");
    auto hidden = start();
    RenderTiming hidden_timing;
    hidden_timing.overlays = OverlayMode::none;
    detail::render_session(hidden,hidden_timing,keys);
    require(hidden.frontend->state().screen_size == 255,"Overlay-free render consumed a gameplay viewport shortcut");
    const auto size = session.frontend->state().screen_size;
    for (unsigned i = 0; i < 5; ++i) { detail::advance_session(session,keys); }
    require(session.frontend->state().screen_size == size,"Viewport shortcut repeated at update frequency");
    detail::frontend_key(session,pause_scan);
    detail::render_session(session,{},keys);
    require(session.frontend->state().screen_size == size,"Paused viewport consumed a gameplay shortcut");
}

void menu_poll_cases(const char* game, const char* fixture) {
    using namespace swiv3d;
    std::ifstream input(fixture);
    unsigned context, draws, scan;
    while (input >> context >> draws >> scan) {
        require(context <= 1 && draws <= 6 && scan < 256,"Invalid queued menu fixture");
        auto session = detail::start_frontend(game);
        detail::complete_cutscene(session,detail::CutsceneEnd::skipped);
        session.movies = detail::MoviePlayback::skip;
        if (context == 1) {
            for (unsigned key : {0x1cU,0x1eU,0x1cU,0x1cU}) { detail::frontend_key(session,key); }
        }
        for (unsigned draw = 0; draw < draws; ++draw) { detail::render_session(session); }
        session.audio->stop();
        KeyboardState keys{}; keys[scan] = true;
        detail::advance_session(session,keys);
        const bool returned = std::any_of(session.audio->voices().begin(),session.audio->voices().end(),[](const auto& voice) {
            return voice.sound == 6003 && (voice.flags & 1) != 0;
        });
        std::cout << context << ' ' << draws << ' ' << scan << ' ' << int(returned) << '\n';
    }
    require(input.eof(),"Invalid queued menu fixture");
}

void test_held_sliders(const char* game) {
    using namespace swiv3d;
    auto session = detail::start_frontend(game);
    detail::complete_cutscene(session,detail::CutsceneEnd::skipped);
    session.movies = detail::MoviePlayback::skip;
    for (auto scan : {0x1c,0x1e,0x1c,0x1c,0x01}) { detail::frontend_key(session,scan); }
    select(session,4);
    select(session,17);
    KeyboardState keys{};
    keys[0x4b] = true;
    for (int tick = 0; tick < 4; ++tick) { detail::advance_session(session,keys); }
    require(session.frontend->state().screen_size == 247,"Held keypad slider did not repeat");
    keys[0xcb] = true;
    detail::advance_session(session,keys);
    require(session.frontend->state().screen_size == 245,"Slider aliases adjusted the value twice");
    for (int tick = 0; tick < 130; ++tick) { detail::advance_session(session,keys); }
    keys[0xcd] = true;
    detail::advance_session(session,keys);
    require(session.frontend->state().screen_size == 2,"Opposing slider keys lost left-before-right clamping");
    keys = {}; keys[0xcb] = true; keys[0x01] = true;
    detail::advance_session(session,keys);
    require(session.frontend->state().screen_size == 0 && session.frontend->state().phase == FrontendPhase::main_menu,
            "Queued menu exit bypassed the held slider update");
    detail::advance_session(session,{});
    select(session,4);
    select(session,18);
    keys = {}; keys[0xcb] = true; keys[0xcd] = true;
    detail::advance_session(session,keys);
    require(session.frontend->state().detail == 240,"Opposing detail keys lost their ordered update");
    keys[0xcd] = false;
    for (int tick = 0; tick < 4; ++tick) { detail::advance_session(session,keys); }
    keys[0x4d] = true;
    detail::advance_session(session,keys);
    require(session.frontend->state().detail == 90,"Detail lower-bound clamping lost the following right input");
    detail::frontend_key(session,0x01);
    select(session,4);
    select(session,11);
    keys = {}; keys[0x4b] = true; keys[0xcd] = true;
    detail::advance_session(session,keys);
    require(session.frontend->state().volumes[2] == 253,"Volume menu lost its return-after-adjustment rule");
    require(std::any_of(session.audio->voices().begin(),session.audio->voices().end(),[](const auto& voice) {
                return voice.sound == 6004 && (voice.flags & 1) != 0;
            }),"Menu slider did not play its original feedback sound");
    detail::advance_session(session,{});
    session.audio->stop();
    keys = {}; keys[0x4d] = true;
    detail::advance_session(session,keys);
    detail::Audio reference(Archive::open(std::filesystem::path(game) / "SOUNDS.BD"));
    reference.present({{0,no_entity,6004}}, {}, [](int) { return detail::SoundSource{}; });
    const auto feedback = session.audio->mix(4096);
    require(feedback == reference.mix(4096) &&
            std::any_of(feedback.begin(),feedback.end(),[](auto sample) { return sample != 0; }),
            "Retained slider feedback differed from the controlled effect mixer");
    session.audio->stop();
    reference.stop();
    detail::frontend_key(session,0x01);
    require(std::any_of(session.audio->voices().begin(),session.audio->voices().end(),[](const auto& voice) {
                return voice.sound == 6003 && (voice.flags & 1) != 0;
            }),"Menu cancellation omitted its original action feedback");
    reference.present({{0,no_entity,6003}}, {}, [](int) { return detail::SoundSource{}; });
    const auto action_feedback = session.audio->mix(4096);
    require(action_feedback == reference.mix(4096) &&
            std::any_of(action_feedback.begin(),action_feedback.end(),[](auto sample) { return sample != 0; }),
            "Retained menu action feedback differed from the controlled effect mixer");
}

void menu_feedback(const char* game, const char* fixture) {
    const auto directory = std::filesystem::path(game);
    swiv3d::Frontend frontend(swiv3d::Archive::open(directory / "GLOBAL.BD"),
                             swiv3d::detail::load_profile(directory / "SWIV.CFG"));
    std::ifstream input(fixture);
    unsigned scan, shift;
    while (input >> scan >> shift) {
        if (scan >= 256 || shift > 1) { throw std::runtime_error("Invalid menu feedback key"); }
        frontend.press(scan,shift == 0 ? swiv3d::KeyShift::released : swiv3d::KeyShift::pressed);
        for (const auto sound : frontend.take_menu_sounds()) { std::cout << sound << '\n'; }
    }
    require(input.eof(),"Invalid menu feedback fixture");
}

void slider_cases(const char* game, const char* fixture) {
    using namespace swiv3d;
    auto session = detail::start_frontend(game);
    detail::complete_cutscene(session,detail::CutsceneEnd::skipped);
    for (auto scan : {0x1c,0x1e,0x1c,0x1c,0x01}) { detail::frontend_key(session,scan); }
    std::ifstream input(fixture);
    int selection, initial;
    unsigned mask;
    while (input >> selection >> initial >> mask) {
        while (session.frontend->state().phase != FrontendPhase::main_menu) { detail::frontend_key(session,0x01); }
        select(session,4);
        if (selection == 3) { select(session,11); }
        else { select(session,selection); }
        const auto value = [&] {
            const auto state = session.frontend->state();
            return selection == 3 ? state.volumes[2] : selection == 17 ? state.screen_size : state.detail;
        };
        if (selection != 18) {
            const auto endpoint = initial % 2 == 0 ? 0 : 255;
            while (value() != endpoint) { detail::frontend_key(session,endpoint == 0 ? 0xcb : 0xcd); }
        }
        while (value() != initial) { detail::frontend_key(session,value() > initial ? 0xcb : 0xcd); }
        session.audio->stop();
        session.previous_keys = {};
        KeyboardState keys{};
        constexpr std::array<unsigned,4> horizontal{0x4b,0xcb,0x4d,0xcd};
        for (unsigned bit = 0; bit < horizontal.size(); ++bit) { keys[horizontal[bit]] = (mask & (1U << bit)) != 0; }
        detail::advance_session(session,keys);
        std::cout << value();
        for (const auto& voice : session.audio->voices()) {
            if ((voice.flags & 1) != 0) { std::cout << ' ' << voice.sound; }
        }
        std::cout << '\n';
    }
    require(input.eof(),"Invalid slider contract fixture");
}
int run_session_tests(int argc,char** argv) {
    try {
        if (argc != 2 && argc != 3) { throw std::runtime_error("Expected GAME [PRESENTATION_OUTPUT]"); }
        test_menu_input_delay(argv[1]);
        test_viewport_shortcuts(argv[1]);
        const auto output = argc == 3 ? std::filesystem::path(argv[2]) : std::filesystem::path{};
        if (!output.empty() && std::filesystem::exists(output)) { throw std::runtime_error("Output must be fresh"); }
        using namespace swiv3d;
        auto session = detail::start_frontend(argv[1]);
        const auto sounds = Archive::open(std::filesystem::path(argv[1]) / "SOUNDS.BD");
        const int clock_sound = sounds.resources().front().id / 100;
        session.audio->present({{0,no_entity,clock_sound,SoundAction::loop}}, {},
                               [](int) { return detail::SoundSource{}; });
        detail::mix_session_device(session,12000);
        require(session.audio->voices()[0].position != 0,"Movie playback froze the retained digital driver clock");
        session.audio->stop();
        auto loading = detail::start_frontend(argv[1]);
        loading.cutscene->advance((loading.cutscene->frames() - 1) * loading.cutscene->frame_duration());
        detail::complete_cutscene(loading,detail::CutsceneEnd::finished);
        for (unsigned tick = 0; tick < 15; ++tick) { detail::advance_session(loading,{}); }
        require(loading.cutscene && loading.cutscene->frame() == 1,"Movie initialization work was skipped");
        require(loading.movie_preparation_work != 0,"Movie loading omitted original file-transfer latency");
        detail::complete_cutscene(session,detail::CutsceneEnd::skipped);
        const auto demo = session.mission->state();
        const auto demo_player = std::find_if(demo.entities.begin(),demo.entities.end(),
            [&](const auto& actor) { return actor.slot == demo.player; });
        require(demo.flight.attitude[0] == demo_player->words[13] * 256,
                "Demo setup lost the inherited ground flight accumulator");
        session.movies = detail::MoviePlayback::skip;
        session.frontend->devices({true,true});
        detail::frontend_key(session,0x1c);
        const std::array<unsigned,14> name{0x1f,0x11,0x17,0x2f,0x02,0x1e,0x31,0x20,0x26,0x12,0x14,0x20,0x17,0x12};
        const std::string characters = "SWIV!AndLetDie";
        for (std::size_t i = 0; i < name.size(); ++i) {
            const bool shift = characters[i] == '!' || (characters[i] >= 'A' && characters[i] <= 'Z');
            detail::frontend_key(session,name[i],shift ? KeyShift::pressed : KeyShift::released);
        }
        detail::frontend_key(session,0x1c);
        require(session.frontend->state().phase == FrontendPhase::pilot_name,"Original character table did not unlock cheats");
        detail::frontend_key(session,0x14); detail::frontend_key(session,0x1c);
        select(session,3); detail::frontend_key(session,0x01);
        const auto profile_before_media = session.frontend->configuration_file();
        for (int i = 0; i < 40 && session.frontend->state().selection != 5; ++i) { detail::frontend_key(session,0xd0); }
        require(session.frontend->state().selection == 5,"Replay intro action is unreachable");
        for (unsigned draws = 0; draws < menu_confirmation_draws; ++draws) { detail::render_session(session); }
        KeyboardState media_keys{};
        media_keys[0x1c] = true;
        detail::advance_session(session,media_keys);
        require(session.cutscene && session.opening_movie == 1,"Replay intro did not own INTRO1");
        save_frame(detail::render_session(session),output,"intro1");
        detail::advance_session(session,media_keys);
        require(session.cutscene != nullptr,"Held menu Enter skipped replayed intro");
        detail::advance_session(session,{});
        media_keys = {}; media_keys[0x01] = true;
        detail::advance_session(session,media_keys);
        require(!session.cutscene && session.frontend->state().selection == 5,"Skipping replay lost the main-menu return");
        select(session,5);
        finish_movie(session);
        require(session.cutscene && session.opening_movie == 2,"Replay intro did not hand off to INTRO2");
        save_frame(detail::render_session(session),output,"intro2");
        finish_movie(session);
        require(!session.cutscene && session.frontend->state().selection == 5,"INTRO2 completion lost the retained menu");
        detail::advance_session(session,{});
        for (int i = 0; i < 40 && session.frontend->state().selection != 6; ++i) { detail::frontend_key(session,0xd0); }
        require(session.frontend->state().selection == 6,"Credits action is unreachable");
        for (unsigned draws = 0; draws < menu_confirmation_draws; ++draws) { detail::render_session(session); }
        media_keys = {}; media_keys[0x1c] = true;
        detail::advance_session(session,media_keys);
        const auto credits_tick = session.mission->state().tick;
        for (unsigned page = 0; page < 3; ++page) {
            const auto frame = detail::render_session(session);
            require(frame.pixels.size() == image_width * image_height && session.frontend->state().credit_page == page,
                    "Credits did not render the active original image");
            save_frame(frame,output,"credits-" + std::to_string(page));
            require(session.mission->state().tick == credits_tick,"Credits advanced the world");
            detail::advance_session(session,media_keys);
            require(session.frontend->state().credit_page == page,"Held input skipped a credit page");
            detail::advance_session(session,{});
            detail::advance_session(session,media_keys);
        }
        require(session.frontend->state().phase == FrontendPhase::main_menu && session.frontend->state().selection == 6,
                "Credits failed to reload the menu background");
        require(session.frontend->configuration_file() == profile_before_media,"Media changed the pilot profile");
        select(session,8);
        const auto exit_tick = session.mission->state().tick;
        for (int i = 0; i < 4; ++i) { detail::advance_session(session,{}); }
        require(session.mission->state().tick == exit_tick + 4,"Exit confirmation froze the original demo simulation");
        detail::frontend_key(session,0x1c);
        require(session.frontend->state().selection == 8,"No failed to cancel exit");
        select(session,8); detail::frontend_key(session,0x01);
        require(session.frontend->state().selection == 8,"Escape failed to cancel exit");
        select(session,9);
        for (int i = 0; i < 12; ++i) { detail::advance_session(session,{}); }
        const auto background_tick = session.mission->state().tick;
        detail::frontend_key(session,0x01);
        const auto background = session.mission->state();
        const auto background_player = std::find_if(background.entities.begin(),background.entities.end(),
            [&](const auto& actor) { return actor.slot == background.player; });
        require(background.tick == background_tick && background_player->words[6] == 0 &&
                session.frontend->state().selection == 9,"Cancelling test-level selection did not reload DEMO with retained clocks");
        select(session,9); select(session,0);
        require(session.frontend->state().cheat_mode && session.frontend->state().phase == FrontendPhase::mission_request,
                "Test-level dispatch failed");
        auto* retained = session.mission.get();
        const auto modal_tick = session.mission->state().tick;
        KeyboardState modal_keys{};
        modal_keys[pause_scan] = true;
        detail::advance_session(session,modal_keys);
        for (int update = 0; update < 6; ++update) { detail::advance_session(session,modal_keys); }
        require(session.frontend->state().phase == FrontendPhase::mission_pause &&
                session.mission.get() == retained && session.mission->state().tick == modal_tick,
                "Held Pause resumed or advanced the mission");
        const auto pause_sounds = session.mission->take_sound_events();
        require(std::count_if(pause_sounds.begin(),pause_sounds.end(),[](const auto& sound) {
                    return sound.sound == 6000 && sound.actor == no_entity;
                }) == 1, "Pause did not emit the original non-positional entry sound once");
        detail::advance_session(session,{});
        modal_keys = {}; modal_keys[0x39] = true;
        detail::advance_session(session,modal_keys);
        require(session.frontend->state().phase == FrontendPhase::mission_request &&
                session.mission.get() == retained && session.mission->state().tick == modal_tick,
                "Pause resume recreated or advanced the retained world");
        const auto resume_sounds = session.mission->take_sound_events();
        require(std::count_if(resume_sounds.begin(),resume_sounds.end(),[](const auto& sound) {
                    return sound.sound == 6001 && sound.actor == no_entity;
                }) == 1, "Pause resume lost the original return sound");
        detail::advance_session(session,{});
        modal_keys = {}; modal_keys[0x01] = true;
        detail::advance_session(session,modal_keys);
        require(session.frontend->state().phase == FrontendPhase::mission_abort && session.frontend->state().selection == 2,
                "Mission Escape skipped the default-No confirmation");
        detail::advance_session(session,{});
        detail::advance_session(session,modal_keys);
        require(session.frontend->state().phase == FrontendPhase::mission_request && session.mission.get() == retained,
                "Cancelling abort lost the live world");
        detail::advance_session(session,{});
        require(session.frontend->state().cheats_unlocked,"Cheat identity did not reach render-time controls");
        KeyboardState camera_keys{};
        const auto camera = detail::render_session(session).camera_controls;
        camera_keys[0x3f] = true;
        detail::render_session(session,{},camera_keys);
        const auto orbit = detail::render_session(session,{},camera_keys).camera_controls;
        require(orbit[1] == camera[1]+12,"Held F5 did not repeat at render boundaries");
        camera_keys = {};
        require(detail::render_session(session,{},camera_keys).camera_controls[1] == orbit[1],"Released F5 continued orbiting");
        camera_keys[0x40] = true;
        detail::render_session(session,{},camera_keys);
        require(detail::render_session(session,{},camera_keys).camera_controls[1] == camera[1],"F6 did not reverse F5");
        camera_keys = {}; camera_keys[0x41] = true;
        require(detail::render_session(session,{},camera_keys).camera_controls[2] == camera[2]+6,"F7 did not change elevation");
        camera_keys = {}; camera_keys[0x42] = true;
        require(detail::render_session(session,{},camera_keys).camera_controls[2] == camera[2],"F8 did not reverse F7");
        camera_keys = {}; camera_keys[0x3e] = true;
        require(detail::render_session(session,{},camera_keys).camera_controls[0] == 1,"F4 did not retain the camera position");
        RenderTiming locked;
        locked.shortcut_keys[0x3f] = locked.shortcut_keys[0x41] = true;
        const auto before_locked = session.mission->render(session.globals).camera_controls;
        const auto after_locked = session.mission->render(session.globals,locked).camera_controls;
        require(before_locked[1] == after_locked[1] && before_locked[2] == after_locked[2],"Locked pilot accepted debug camera input");
        KeyboardState grab_keys{};
        grab_keys[0x57] = true;
        const auto grab_tick = session.mission->state().tick;
        detail::advance_session(session,grab_keys);
        const auto grab_camera = detail::render_session(session).camera_controls;
        for (int i = 0; i < 6; ++i) { detail::advance_session(session,grab_keys); }
        require(session.frontend->state().phase == FrontendPhase::mission_grab &&
                session.mission->state().tick == grab_tick && session.mission.get() == retained &&
                detail::render_session(session).camera_controls == grab_camera,
                "Screen grab failed to freeze the retained world and camera");
        detail::advance_session(session,{});
        select(session,11);
        require(session.mission->state().tick == grab_tick+1 && session.frontend->state().phase == FrontendPhase::mission_grab,
                "Grab increment did not advance exactly one world update");
        select(session,12);
        require(detail::render_session(session).stats_pixels.empty(), "Disabled grab statistics still rendered");
        select(session,12);
        require(!detail::render_session(session).stats_pixels.empty(), "Grab statistics failed to render after re-enabling");
        if (!output.empty()) {
            const auto profile = session.profile;
            session.profile = output / "grab-profile.CFG";
            select(session,10);
            detail::frontend_key(session,0x1e); detail::frontend_key(session,0x1c);
            require(std::filesystem::file_size(output / "swivgrab/a.bmp") == 129078,
                    "Grab filename confirmation did not save the original-sized bitmap");
            session.profile = profile;
        }
        const auto grab_yaw = detail::render_session(session).camera_controls[1];
        detail::frontend_key(session,0x01);
        require(session.frontend->state().phase == FrontendPhase::mission_request && session.mission.get() == retained &&
                detail::render_session(session).camera_controls[1] == grab_yaw,
                "Grab return lost the mission or restored an unrelated modal yaw");
        DeviceInput input;
        input.joystick_connected = true; input.joystick = {128,128,0}; input.joystick_buttons[0] = true;
        const auto initial = session.mission->state();
        for (int i = 0; i < 12; ++i) { detail::advance_session(session,{},input); }
        auto world = session.mission->state();
        require(world.flight.controller == ControllerKind::joystick && world.flight.attitude[0] != initial.flight.attitude[0] &&
                world.flight.control_ticks > initial.flight.control_ticks && world.weapons.primary_held,
                "Retained joystick input did not steer and fire in the actual mission");
        const auto before_menu = detail::render_session(session).camera_controls;
        detail::frontend_key(session,0x58);
        const auto tick = world.tick;
        for (int i = 0; i < 4; ++i) { detail::advance_session(session,{}); }
        require(session.mission->state().tick == tick,"Cheat menu advanced mission simulation");
        camera_keys = {}; camera_keys[0x3f] = camera_keys[0x41] = true;
        detail::render_session(session,{},camera_keys);
        select(session,0);
        require(session.mission->state().weapons.ammunition[0] > world.weapons.ammunition[0],"Cheat menu did not grant ammunition");
        detail::frontend_key(session,0x01);
        const auto after_menu = detail::render_session(session).camera_controls;
        require(after_menu[1] == before_menu[1] && after_menu[2] == before_menu[2]+6,
                "Modal return restored elevation instead of retaining it with the original yaw");
        detail::frontend_key(session,0x3b);
        camera_keys = {}; camera_keys[0x42] = true;
        detail::render_session(session,{},camera_keys);
        select(session,10); select(session,1); select(session,2);
        detail::frontend_key(session,0x01); detail::frontend_key(session,0x01);
        require(session.mission.get() == retained && session.mission->state().tick == tick &&
                session.mission->state().flight.controller == ControllerKind::mouse,
                "Returning from setup recreated the mission or lost changed controls");
        const auto after_setup = detail::render_session(session).camera_controls;
        require(after_setup[1] == after_menu[1] && after_setup[2] == before_menu[2],
                "F1 setup return discarded the retained elevation change");
        input = {}; input.mouse = {-128,0,0}; input.mouse_buttons[0] = true;
        detail::advance_session(session,{},input);
        require(session.mission->state().flight.axes[0] == -128 && session.mission->state().weapons.primary_held,
                "Changed mouse controls did not reach the retained mission");
        auto invalid = session.frontend->configuration();
        for (std::size_t offset = 0; offset + 128 <= invalid.size(); offset += 128) {
            if (invalid[offset] != 20 || invalid[offset+8] != 2) { continue; }
            invalid[offset+12] = 3;
            invalid[offset+16+10*4] = 0;
            invalid[offset+16+10*4+1] = 1;
        }
        bool rejected = false;
        try { session.mission->configure_controls(invalid); } catch (const std::runtime_error&) { rejected = true; }
        require(rejected && session.mission->state().flight.controller == ControllerKind::mouse,
                "Rejected controller setup partially changed the live mission");
        detail::frontend_key(session,0x58);
        select(session,-9); select(session,-9);
        select(session,201);
        require(session.mission->state().flight.craft == CraftKind::buggy,"Cheat craft switch did not create ground vehicle");
        select(session,200);
        require(session.mission->state().flight.craft == CraftKind::helicopter,"Cheat craft switch did not restore helicopter");
        select(session,102);
        const auto healed = session.mission->state();
        const auto healed_player = std::find_if(healed.entities.begin(),healed.entities.end(),
            [&](const auto& actor) { return actor.slot == healed.player; });
        require(healed_player->words[2] > 496,"Cheat health grant did not reach the player");
        detail::frontend_key(session,0x01);
        detail::render_session(session);
        const auto device_pcm = detail::mix_session_device(session,9600);
        require(device_pcm.size() == 19200 && std::any_of(device_pcm.begin(),device_pcm.end(),[](auto value) { return value != 0; }),
                "Retained mission did not reach the device audio output path");
        auto live_mission = std::move(session.mission);
        auto dying = session.frontend->state();
        std::fill_n(dying.mission_profile.begin()+48,4,0);
        session.mission = std::make_unique<Mission>(Archive::open(session.game / "TERRAN00.SLV"),dying,
                                                  session.frontend->configuration(),&session.globals);
        detail::advance_session(session,{});
        require(session.mission->state().finish == FinishKind::death,"Death control fixture did not enter its finish sequence");
        grab_keys = {}; grab_keys[0x57] = true;
        detail::advance_session(session,grab_keys);
        require(session.frontend->state().phase == FrontendPhase::mission_request,
                "F11 interrupted the mission finish sequence");
        session.mission = std::move(live_mission);
        session.previous_keys = {};
        detail::frontend_key(session,0x01);
        require(session.frontend->state().phase == FrontendPhase::mission_abort,"Mission abort confirmation was unreachable");
        select(session,1);
        require(session.frontend->state().phase == FrontendPhase::main_menu && !session.frontend->state().mission_menu,
                "Confirmed mission abort failed to return to the pilot menu");
        select(session,8); select(session,1);
        require(session.frontend->state().phase == FrontendPhase::quit,"Confirmed exit did not reach the retained session");
        const auto saved_configuration = session.frontend->configuration_file();
        detail::advance_session(session,{});
        require(session.frontend->configuration_file() == saved_configuration,"Shutdown continued dispatching input");
        if (!output.empty()) { session.profile = output / "exit.cfg"; }
        detail::abort_mission(session);
        if (!output.empty()) {
            std::ifstream file(session.profile,std::ios::binary);
            const std::vector<std::uint8_t> saved((std::istreambuf_iterator<char>(file)),{});
            require(saved == saved_configuration,"Confirmed exit did not save compatible CFG bytes");
        }
        auto failed_movie = detail::start_frontend(argv[1]);
        failed_movie.game /= "missing-movie-fixture";
        require(!std::filesystem::exists(failed_movie.game),"Missing-movie fixture path already exists");
        bool movie_failed = false;
        try { detail::complete_cutscene(failed_movie,detail::CutsceneEnd::finished); }
        catch (const std::runtime_error&) { movie_failed = true; }
        require(movie_failed && !failed_movie.cutscene,"Movie handoff retained the old owner while opening its successor");
        detail::MissionSession results{session.globals};
        results.frontend = std::make_unique<Frontend>(results.globals,std::vector<std::uint8_t>{});
        for (auto scan : {0x1c,0x1e,0x1c,0x1c,0x01,0x1c,0x01}) { results.frontend->press(scan); }
        auto fatal_request = results.frontend->state();
        require(fatal_request.phase == FrontendPhase::mission_request,"Results fixture did not reach a mission request");
        std::fill_n(fatal_request.mission_profile.begin()+48,4,0);
        results.mission = std::make_unique<Mission>(Archive::open(session.game / "TERRAN00.SLV"),fatal_request,
                                                  results.frontend->configuration(),&results.globals);
        detail::render_session(results);
        for (int tick = 0; tick < 37; ++tick) { detail::advance_session(results,{}); }
        require(results.mission->state().phase == MissionPhase::failed &&
                results.frontend->state().phase == FrontendPhase::mission_request,
                "Results began before queued mission updates drained to the render boundary");
        const auto result_tick = results.mission->state().tick;
        detail::render_session(results);
        require(results.frontend->state().phase == FrontendPhase::mission_results &&
                results.mission->state().tick == result_tick,
                "Results entry advanced the world or missed its first render");
        std::cout << "Original assets, menu media, quit/save, cheat/test level, joystick steering/fire and modal/live mouse changes passed\n";
        return 0;
    } catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
}

int main(int argc,char** argv) {
    try {
        if (argc == 4 && std::string(argv[1]) == "--slider-cases") { slider_cases(argv[2],argv[3]); return 0; }
        if (argc == 4 && std::string(argv[1]) == "--menu-feedback") { menu_feedback(argv[2],argv[3]); return 0; }
        if (argc == 4 && std::string(argv[1]) == "--menu-poll-cases") { menu_poll_cases(argv[2],argv[3]); return 0; }
        if (argc != 2 && argc != 3) { throw std::runtime_error("Expected GAME [PRESENTATION_OUTPUT]"); }
        test_held_sliders(argv[1]);
        return run_session_tests(argc,argv);
    } catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
