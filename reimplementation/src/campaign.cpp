#include "input_script.hpp"
#include "menu_render.hpp"
#include "clock.hpp"

#include <algorithm>
#include <stdexcept>

namespace swiv3d::detail {

void advance_session(MissionSession& session, const KeyboardState& keyboard, const DeviceInput& devices) {
    auto keys = keyboard;
    for (std::size_t i = 0; i < devices.mouse_buttons.size(); ++i) { keys[0x90+i] = keys[0x90+i] || devices.mouse_buttons[i]; }
    for (std::size_t i = 0; i < devices.joystick_buttons.size(); ++i) { keys[0x93+i] = keys[0x93+i] || (devices.joystick_connected && devices.joystick_buttons[i]); }
    const auto phase = session.frontend->state().phase;
    const bool grab_mode = phase == FrontendPhase::mission_grab || phase == FrontendPhase::grab_name;
    if (!session.cutscene && !grab_mode && phase != FrontendPhase::credits && phase != FrontendPhase::quit) { session.frontend->advance_clock(1); }
    if (phase == FrontendPhase::quit) { session.previous_keys = keys; return; }
    if (session.cutscene) {
        bool pressed = false;
        for (std::size_t scan = 0; scan < keys.size(); ++scan) {
            if (keys[scan] && !session.previous_keys[scan]) { pressed = true; break; }
        }
        if (pressed) { complete_cutscene(session,CutsceneEnd::skipped); }
        else if (session.movie_timing == MovieTiming::updates && session.movie_preparation_work != 0) {
            constexpr auto work_per_update = CompatibilityClock::cpu_frequency / mission_tick_rate;
            session.movie_preparation.elapse(work_per_update,work_per_update);
            session.movie_preparation_work = session.movie_preparation.remaining();
            if (session.movie_preparation_work == 0) {
                if (session.audio) { session.cutscene->attach_audio(*session.audio); }
                session.movie_preparation_frame = {};
            }
        }
        else if (session.movie_timing == MovieTiming::updates && session.cutscene->advance(1000000 / mission_tick_rate)) { complete_cutscene(session,CutsceneEnd::finished); }
        session.previous_keys = keys;
        return;
    } else if (phase == FrontendPhase::credits) {
        for (std::size_t scan = 0; scan < keys.size(); ++scan) {
            if (keys[scan] && !session.previous_keys[scan]) { frontend_key(session,static_cast<std::uint32_t>(scan)); break; }
        }
        session.previous_keys = keys;
        return;
    } else if (phase == FrontendPhase::mission_request) {
        if (session.mission->state().phase == MissionPhase::playing) {
            for (const unsigned scan : {0x01U,pause_scan,0x3bU,0x57U,0x58U}) {
                if ((scan == pause_scan || scan == 0x57) && session.mission->state().finish != FinishKind::none) { continue; }
                if (keys[scan] && !session.previous_keys[scan]) {
                    frontend_key(session,scan,KeyShift::released,keys);
                    if (session.frontend->state().mission_menu) { break; }
                }
            }
        }
        if (session.frontend->state().mission_menu) { session.previous_keys = keys; return; }
        session.mission->advance(keys,devices);
    } else {
        const bool modal = grab_mode || phase == FrontendPhase::mission_pause || phase == FrontendPhase::mission_abort ||
            (session.frontend_playback == FrontendPlayback::interactive &&
            (phase == FrontendPhase::pilot_name || phase == FrontendPhase::controller_type || phase == FrontendPhase::controller_bindings ||
             phase == FrontendPhase::settings || phase == FrontendPhase::key_capture ||
             phase == FrontendPhase::sound_volume || phase == FrontendPhase::language_selection ||
             phase == FrontendPhase::gamma_selection || phase == FrontendPhase::music_selection ||
             phase == FrontendPhase::axis_bindings || phase == FrontendPhase::cheat_menu));
        if (modal) { if (!grab_mode) { session.mission->advance_menu(); } }
        else { session.mission->advance({}); }
        if (phase == FrontendPhase::mission_results) {
            session.frontend->advance_results(1);
            constexpr int results_input_delay = 110;
            bool pressed = false;
            for (std::size_t scan = 0; scan < keys.size(); ++scan) {
                if (keys[scan] && !session.previous_keys[scan]) { pressed = true; break; }
            }
            if (session.mission->state().finish_counter >= results_input_delay && pressed) { finish_mission(session); }
        } else {
            if (!modal) { session.frontend->advance_landscape_clock(1); }
            ++session.menu_updates;
            constexpr unsigned keypad_left = 0x4b, keypad_right = 0x4d;
            constexpr unsigned menu_left = 0xcb, menu_right = 0xcd;
            const bool slider_menu = phase == FrontendPhase::settings || phase == FrontendPhase::sound_volume;
            if (slider_menu) {
                // rendermenu_ polls both held directions before queued keys.
                const bool left_held = keys[keypad_left] || keys[menu_left];
                if (left_held) { frontend_key(session,menu_left); }
                // The volume menu returns immediately after its first adjustment.
                if ((keys[keypad_right] || keys[menu_right]) && (!left_held || phase != FrontendPhase::sound_volume)) {
                    frontend_key(session,menu_right);
                }
            }
            for (std::size_t scan = 0; scan < keys.size(); ++scan) {
                if (slider_menu && (scan == keypad_left || scan == menu_left || scan == keypad_right || scan == menu_right)) { continue; }
                if (keys[scan] && !session.previous_keys[scan] &&
                    (session.frontend_playback == FrontendPlayback::scripted || session.frontend->accepts_queued_key(static_cast<std::uint32_t>(scan)))) {
                    frontend_key(session,static_cast<std::uint32_t>(scan),keys[0x2a] || keys[0x36] ? KeyShift::pressed : KeyShift::released,keys);
                    break;
                }
            }
        }
    }
    if (++session.cd_timer > 120) {
        session.cd_timer = 0;
        if (session.audio) { session.audio->poll_music(); }
    }
    session.previous_keys = keys;
}

MissionFrame render_session(MissionSession& session, RenderTiming timing, const KeyboardState& shortcut_keys) {
    session.render_rate = timing.frames_per_second;
    if (session.cutscene) {
        return session.movie_preparation_work != 0 || session.movie_video_work != 0 ? session.movie_preparation_frame : session.cutscene->render();
    }
    if (session.frontend->state().phase == FrontendPhase::credits) {
        constexpr std::array<unsigned,credit_page_count> credit_images{1003,1002,1001};
        const auto image = session.globals.load(credit_images.at(session.frontend->state().credit_page));
        Planes planes{};
        copy_image(image,planes);
        MissionFrame frame;
        frame.pixels = indexed_pixels(planes);
        std::copy_n(image.begin(),frame.palette.size(),frame.palette.begin());
        return frame;
    }
    if (session.frontend->state().phase == FrontendPhase::mission_request &&
        session.mission->state().phase != MissionPhase::playing) { begin_results(session); }
    timing.screen_size = session.frontend->state().screen_size;
    timing.detail = session.frontend->state().detail;
    timing.debug_controls = session.frontend->state().debug_controls;
    timing.debug_counter = session.frontend->state().debug_counter;
    timing.cheats_unlocked = session.frontend->state().cheats_unlocked;
    timing.shortcut_keys = shortcut_keys;
    const auto phase = session.frontend->state().phase;
    const bool menu = phase != FrontendPhase::mission_request;
    if (menu) {
        const bool grab = phase == FrontendPhase::mission_grab || phase == FrontendPhase::grab_name;
        timing.overlays = grab && session.frontend->state().grab_statistics ? OverlayMode::statistics : OverlayMode::none;
        timing.counter_reset = FrameCounterReset::render_end;
    }
    auto frame = session.mission->render(session.globals, timing);
    // The original polls viewport keys after drawing the current camera.
    if (!menu && timing.overlays == OverlayMode::mission && session.mission->state().finish == FinishKind::none) {
        constexpr unsigned keypad_minus = 0x4a, keypad_plus = 0x4e;
        for (const unsigned scan : {keypad_minus,keypad_plus}) {
            if (shortcut_keys[scan]) { frontend_key(session,scan); }
        }
    }
    apply_gamma(frame,session.frontend->state().gamma);
    present_audio(session, frame);
    if (menu) {
        draw_menu(frame, session.globals, session.mission->resource(108), *session.frontend);
        session.frontend->presented();
    }
    return frame;
}

std::vector<std::int16_t> mix_session(MissionSession& session, std::size_t frames) {
    if (session.cutscene) { return session.cutscene->mix(frames); }
    if (session.audio) { session.audio->effects_volume(session.frontend->state().volumes[2]); }
    return session.audio ? session.audio->mix(frames) : std::vector<std::int16_t>(frames * 2);
}

std::vector<std::int16_t> mix_session_device(MissionSession& session, std::size_t frames) {
    if (session.cutscene && !session.audio) { return session.cutscene->mix_device(frames); }
    if (!session.audio) { return std::vector<std::int16_t>(frames * 2); }
    session.audio->effects_volume(session.frontend->state().volumes[2]);
    if (session.movie_timing == MovieTiming::updates || !session.cutscene) { return session.audio->mix_device(frames); }
    std::vector<std::int16_t> output;
    output.reserve(frames * 2);
    constexpr auto frames_per_millisecond = device_audio_rate / 1000;
    constexpr auto work_per_frame = CompatibilityClock::cpu_frequency / device_audio_rate;
    while (frames != 0) {
        auto count = std::min<std::size_t>(frames,frames_per_millisecond);
        const bool preparing = session.movie_preparation_work != 0;
        if (preparing) { count = 1; }
        if (session.movie_video_work != 0) {
            count = std::min<std::size_t>(count,(session.movie_video_work + work_per_frame - 1) / work_per_frame);
        }
        if (!preparing) { count = std::min(count,session.cutscene->device_frames_until_poll()); }
        if (preparing) {
            count = std::min<std::size_t>(count,(session.movie_preparation_work + work_per_frame - 1) / work_per_frame);
        }
        const auto samples = session.audio->mix_device(count);
        const auto cpu_work = session.audio->cpu_work();
        output.insert(output.end(),samples.begin(),samples.end());
        frames -= count;
        if (preparing) {
            const auto before = session.movie_preparation_work;
            session.movie_preparation.elapse_device(count * work_per_frame,cpu_work,
                session.audio->device_cycles() - count * work_per_frame,
                [audio = session.audio.get()](auto interrupt) { return audio->irq_work(interrupt); });
            session.movie_preparation_work = session.movie_preparation.remaining();
            session.cutscene->prepare_audio_clock(*session.audio,before,session.movie_preparation_work,cpu_work);
            if (session.movie_preparation_work == 0) {
                session.cutscene->attach_audio(*session.audio);
                session.movie_video_work = session.cutscene->first_video_work();
                if (session.movie_video_work == 0) { session.movie_preparation_frame = {}; }
            }
        } else if (session.cutscene->advance_device(count,session.movie_video_work == 0 ? MoviePolling::enabled : MoviePolling::suspended)) {
            complete_cutscene(session,CutsceneEnd::finished);
        }
        if (session.movie_video_work != 0 && !preparing) {
            session.movie_video_work -= std::min(session.movie_video_work,cpu_work);
            if (session.movie_video_work == 0) { session.movie_preparation_frame = {}; }
        }
        if (!session.cutscene) {
            const auto tail = session.audio->mix_device(frames);
            output.insert(output.end(),tail.begin(),tail.end());
            break;
        }
    }
    return output;
}

void present_audio(MissionSession& session, const MissionFrame& frame) {
    if (session.audio) {
        SoundListener listener;
        std::copy_n(frame.view_transform.begin(), 3, listener.position.begin());
        std::copy_n(frame.view_transform.begin() + 9, 3, listener.forward.begin());
        listener.right = frame.sound_right;
        const auto world = session.mission->state();
        session.audio->present(session.mission->take_sound_events(), listener, [&](int slot) {
            const auto actor = std::find_if(world.entities.begin(), world.entities.end(), [slot](const auto& entity) { return entity.slot == slot; });
            if (actor == world.entities.end()) { throw std::runtime_error("Sound references missing actor"); }
            SoundSource source;
            for (int axis = 0; axis < 3; ++axis) { source.position[axis] = actor->words[7 + axis] >> 8; }
            source.rate = slot == world.player ? world.flight.engine_pitch : 11025;
            return source;
        });
    }
}

}
