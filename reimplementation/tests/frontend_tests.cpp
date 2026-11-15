#include "swiv3d/frontend.hpp"
#include "../src/input_script.hpp"

#include <algorithm>
#include <iostream>
#include <fstream>
#include <map>
#include <sstream>
#include <stdexcept>

namespace {

void require(bool condition, const char* message) {
    if (!condition) {
        throw std::runtime_error(message);
    }
}

void put(std::vector<std::uint8_t>& data, std::size_t offset, std::uint32_t value) {
    for (std::size_t i = 0; i < sizeof(value); ++i) {
        data.at(offset + i) = static_cast<std::uint8_t>(value >> (i * 8));
    }
}

std::vector<std::uint8_t> strings() {
    using Group = std::map<std::uint32_t, std::string>;
    std::map<std::uint32_t, Group> groups{
        {0, {{1, "Pilot|Choose"}, {10, "New"}, {11, "Delete"}, {51, "Name"}, {52, "[%s_]"}, {61,"Choose deletion"},{100,"On"},{101,"Off"}}},
        {1, {{1, "Player|%s"}, {10, "Start"}, {11, "Continue"}, {12, "Select level"}, {13, "Setup"}, {14, "Intro"}, {15, "Credits"},
             {18, "Pilot"}, {19, "Exit"},{20,"Test levels"}}},
        {2, {{10,"Controls"},{11,"Volume"},{12,"Language"},{13,"Gamma"},{14,"Music"},{15,"Debug [%s]"},{16,"Screen"},{17,"Detail"}}},
        {3, {{1, "Controller|%s"}, {11, "Keyboard"}, {12, "Mouse"},{13,"Joystick"},{14,"Gamepad"},{20,"Bind axes"}, {34,"Press a key"},{101, "Choose|Controller"},
             {200,"Keyboard"},{201,"X"},{202,"Y"},{203,"Z"},{1001,"Bind motion"},{1010,"Turn [%s]"},{1011,"Move [%s]"},{1012,"Strafe [%s]"},{1013,"Clip [%s]"}}},
        {4, {{1, "Any key"}}},
        {7, {{0, "Level|Completed"}, {1, "Game Over"}, {10, "Level score %d"}, {11, "Score %d"},
             {12, "Level time %uh %um %us"}, {13, "Total time %uh %um %us"}, {14, "Objectives %d%%"},
             {15, "Pickups %d%%"}, {16, "Secrets %d%%"}}},
        {11, {{1,"Select|Mission"},{10,"Back"},{11,"Next"},{21,"Test|Mission"}}},
        {12, {{1,"Screen grab menu"},{10,"Grab Screen"},{11,"Increment Frame"},{12,"Toggle Stat elements"},
              {20,"Enter filename"},{21,"c:\\swivgrab\\%s.bmp"}}},
        {6, {{1,"Monitor"},{10,"None"},{11,"Low"},{12,"Medium"},{13,"High"}}},
        {8, {{1,"Volume"},{6,"Unavailable"},{10,"Master"},{11,"CD"},{12,"Effects"}}},
        {9, {{1,"Music"},{10,"SWIV"},{11,"Classical"},{12,"Misc"},{13,"#"}}},
        {13, {{1,"Abort mission?"},{10,"Yes"},{11,"No"},{30,"Exit?"},{31,"Exit one?"},{32,"Exit two?"},{33,"Exit three?"}}},
        {200, {{0, "Rank"}, {1, "Rank two"}}},
        {0x630000, {{0,"English"},{1,"#"}}},
        {0x630002, {{0, "0"}}}
    };
    for (int level = 2; level < 17; ++level) { groups[200][level] = "Mission " + std::to_string(level); }
    groups[200][17] = "#";
    for (auto id : {21, 22, 24, 25, 26, 27, 28, 29, 30, 31, 32}) {
        groups[3][id] = "Key [%s]";
    }
    for (auto id : {0xcb, 0xcd, 0xc8, 0xd0, 0x33, 0x34, 0x39, 0x32, 0xc9, 0xd1, 0x1d}) {
        groups[1000][id] = "Synthetic key";
    }
    groups[1000][0x01] = "#";
    groups[1000][0x1e] = "A";
    groups[1000][0x48] = "Keypad Up";
    for (auto scan : {0x90,0x91,0x93,0x94,0x95,0x96}) { groups[1000][scan] = "Device button"; }
    for (unsigned id = 0; id < 20; ++id) { groups[100][id] = "Weapon"; }
    for (unsigned id : {100U,101U,102U,105U,106U,107U,108U,113U,200U,201U,202U,203U,1001U,9999U}) { groups[100][id] = "Cheat"; }
    std::string keys(128, '_');
    keys[0x1e] = 'a';
    keys[0x30] = 'b';
    const std::string cheat_name = "SWIV!AndLetDie";
    std::copy(cheat_name.begin(),cheat_name.end(),keys.begin()+0x60);
    groups[1001][0] = keys;
    groups[1001][1] = keys;
    std::vector<std::uint8_t> data((groups.size() + 1) * 8);
    std::size_t root = 0;
    for (const auto& group : groups) {
        put(data, root, group.first);
        put(data, root + 4, static_cast<std::uint32_t>(data.size()));
        root += 8;
        const auto start = data.size();
        data.resize(start + (group.second.size() + 1) * 8);
        std::size_t cursor = start;
        for (const auto& item : group.second) {
            put(data, cursor, item.first);
            put(data, cursor + 4, static_cast<std::uint32_t>(data.size()));
            data.insert(data.end(), item.second.begin(), item.second.end());
            data.push_back(0);
            cursor += 8;
        }
        put(data, cursor, UINT32_MAX);
    }
    put(data, root, UINT32_MAX);
    return data;
}

enum class CampaignFixture { single_mission, two_missions, completed_campaign };

swiv3d::Archive globals(CampaignFixture fixture = CampaignFixture::single_mission) {
    std::map<std::uint32_t, std::vector<std::uint8_t>> members;
    for (auto id : {1000, 100, 200, 501, 502, 503, 504, 505, 506, 507, 508, 510, 511, 512, 600, 800, 700}) {
        members[id] = {0};
    }
    members[100] = strings();
    members[508].resize(384 * 4);
    for (std::size_t i = 0; i < 384; ++i) {
        put(members[508], i * 4, 1);
    }
    auto& sequence = members[600];
    sequence.resize(8 * 16);
    put(sequence, 0, 200);
    put(sequence, 16, 100); put(sequence, 20, 1); put(sequence, 24, 496);
    put(sequence, 32, 100); put(sequence, 36, 109); put(sequence, 40, 0xffff);
    put(sequence, 48, 101); put(sequence, 52, 109); put(sequence, 56, 0);
    put(sequence, 64, 103); put(sequence, 68, 119); put(sequence, 72, 0x10001);
    put(sequence, 80, 500);
    sequence[84] = 'C'; sequence[85] = 'U'; sequence[86] = 'T';
    put(sequence, 96, 400);
    for (std::size_t i = 0; i < 6; ++i) {
        sequence[100 + i * 2] = static_cast<std::uint8_t>(i + 1);
    }
    put(sequence, 112, 300);
    sequence[116] = 'T'; sequence[117] = 'E'; sequence[118] = 'S'; sequence[119] = 'T';
    if (fixture == CampaignFixture::two_missions) {
        sequence.resize(11 * 16);
        put(sequence, 128, 200); put(sequence, 132, 1);
        put(sequence, 144, 101); put(sequence, 148, 1); put(sequence, 152, 248);
        put(sequence, 160, 300);
        sequence[164] = 'N'; sequence[165] = 'E'; sequence[166] = 'X'; sequence[167] = 'T';
    }
    if (fixture == CampaignFixture::completed_campaign) {
        sequence.resize(12 * 16);
        put(sequence,128,500);
        sequence[132] = 'E'; sequence[133] = 'N'; sequence[134] = 'D';
        put(sequence,144,200); put(sequence,148,999);
        put(sequence,160,UINT32_MAX - 1);
        put(sequence,176,UINT32_MAX);
    }
    std::vector<std::uint8_t> archive;
    std::vector<swiv3d::Resource> directory;
    for (const auto& member : members) {
        directory.push_back({static_cast<std::uint32_t>(archive.size()),
                             static_cast<std::uint32_t>(member.second.size()), member.first});
        archive.insert(archive.end(), member.second.begin(), member.second.end());
    }
    directory.push_back({static_cast<std::uint32_t>(directory.size() + 1), 0, 0});
    for (const auto& entry : directory) {
        const auto offset = archive.size();
        archive.resize(offset + 12);
        put(archive, offset, entry.offset); put(archive, offset + 4, entry.size); put(archive, offset + 8, entry.id);
    }
    return swiv3d::Archive(std::move(archive));
}

void test_retained_route() {
    const auto archive = globals();
    swiv3d::Frontend frontend(archive, {});
    require(frontend.state().phase == swiv3d::FrontendPhase::pilot_selection, "Pilot startup missing");
    frontend.advance_landscape_clock(7);
    frontend.press(0x1c);
    frontend.press(0x1c);
    require(frontend.state().phase == swiv3d::FrontendPhase::pilot_name, "Empty name accepted");
    for (std::size_t i = 0; i < 15; ++i) {
        frontend.press(0x1e);
    }
    frontend.press(0x30);
    require(frontend.rows()[7].text == "[aaaaaaaaaaaaaaa_]", "Name limit or shared name storage changed");
    frontend.press(0x0e);
    frontend.press(0x1c);
    require(frontend.state().phase == swiv3d::FrontendPhase::controller_type, "Controller setup skipped");
    frontend.advance_landscape_clock(9);
    frontend.press(0x1c);
    require(frontend.state().phase == swiv3d::FrontendPhase::controller_bindings, "Keyboard defaults not retained");
    frontend.press(0x01);
    require(frontend.state().phase == swiv3d::FrontendPhase::main_menu, "Bindings did not return to main menu");
    frontend.press(0x1c);
    require(frontend.state().phase == swiv3d::FrontendPhase::briefing, "Sequence did not yield at briefing");
    require(frontend.state().movie == "CUT", "Wrong sequence movie identity");
    frontend.press(0x01);
    const auto mission = frontend.state();
    require(mission.phase == swiv3d::FrontendPhase::mission_request && mission.level_file == "TEST.slv",
            "Sequence did not retain its mission successor");
    require(mission.landscape_clock.rotor == 160 && mission.landscape_clock.water == 16,
            "Menu or sequence transition reset the landscape clock");
    require(mission.game_counter == 16, "Menu dispatch count did not reach the mission");
    frontend.advance_landscape_clock(UINT32_MAX - 15);
    require(frontend.state().landscape_clock.rotor == 0 && frontend.state().landscape_clock.water == 0,
            "Landscape clocks lost 32-bit wrap semantics");
    require(mission.tracks == std::array<std::uint32_t, 6>{1, 2, 3, 4, 5, 6}, "Wrong packed CD track order");
    require(mission.mission_profile[0x3c - 12] == 0xf0 && mission.mission_profile[0x3d - 12] == 1,
            "Campaign assignment did not survive briefing");
    require(mission.mission_profile[0x1e - 12] == 0 && mission.mission_profile[0x1f - 12] == 0,
            "Short campaign comparison lost signedness");
    require(mission.mission_profile[0x32 - 12] == 1 && mission.mission_profile[0x33 - 12] == 0,
            "Short campaign addition lost truncation");
    const auto retained = frontend.configuration();
    const auto name_offset = 2 * 128 + 12;
    require(retained.at(name_offset) == 'a' && retained.at(name_offset + 14) == 0,
            "Pilot bytes did not survive mission dispatch");
}

void test_input_line_endings() {
    const auto archive = globals();
    swiv3d::Frontend frontend(archive, {});
    std::istringstream startup("swiv3d-frontend-input-v2\r\npilot-selection landscape-updates 3\r\n");
    swiv3d::detail::replay_frontend(frontend, startup);
    require(frontend.state().landscape_clock.rotor == 30, "Windows frontend movie lost clock input on another host");
    std::istringstream flight("swiv3d-flight-input-v1\r\n2 0xc8+0xcd\r\n1 -\r\n");
    const auto segments = swiv3d::detail::read_flight(flight);
    require(segments.size() == 2 && segments[0].updates == 2 && segments[0].keys[0xc8] && segments[0].keys[0xcd] &&
            std::none_of(segments[1].keys.begin(), segments[1].keys.end(), [](bool value) { return value; }),
            "Windows flight movie lost held/released controls on another host");
}

void test_mission_pause_and_abort() {
    swiv3d::Frontend frontend(globals(), {});
    for (auto scan : {0x1c, 0x1e, 0x1c, 0x1c, 0x01, 0x1c, 0x01}) { frontend.press(scan); }
    const auto configuration = frontend.configuration();
    frontend.press(0x01);
    require(std::string(swiv3d::phase_name(frontend.state().phase)) == "mission-abort" &&
            frontend.state().mission_menu && frontend.state().selection == 2,
            "Mission Escape did not open the default-No abort confirmation");
    require(frontend.rows().at(2).text == "Abort mission?", "Mission abort lost the original prompt");
    frontend.press(0x1c);
    require(frontend.state().phase == swiv3d::FrontendPhase::mission_request && !frontend.state().mission_menu,
            "No did not resume the retained mission");
    frontend.press(0x01);
    frontend.press(0x01);
    require(frontend.state().phase == swiv3d::FrontendPhase::mission_request,
            "Escape did not cancel mission abort");
    frontend.press(0x9d);
    require(std::string(swiv3d::phase_name(frontend.state().phase)) == "mission-pause" &&
            frontend.state().mission_menu,
            "Pause did not enter the original modal owner");
    frontend.press(0x39);
    require(frontend.state().phase == swiv3d::FrontendPhase::mission_request && !frontend.state().mission_menu,
            "A fresh key did not resume the paused mission");
    require(frontend.configuration() == configuration, "Pause or cancelled abort changed pilot records");
    frontend.press(0x01);
    frontend.press(0xc8);
    frontend.press(0x1c);
    require(frontend.take_abort() && !frontend.take_abort(), "Confirmed abort was lost or consumed twice");
}

void test_screen_grab_entry() {
    swiv3d::Frontend locked(globals(), {});
    for (auto scan : {0x1c, 0x1e, 0x1c, 0x1c, 0x01, 0x1c, 0x01}) { locked.press(scan); }
    locked.press(0x57);
    require(locked.state().phase == swiv3d::FrontendPhase::mission_request, "Locked pilot opened screen grab");

    swiv3d::Frontend frontend(globals(), {});
    frontend.press(0x1c);
    for (unsigned i = 0; i < std::string("SWIV!AndLetDie").size(); ++i) { frontend.press(0x60+i); }
    for (auto scan : {0x1c, 0x1e, 0x1c, 0x1c, 0x01, 0x1c, 0x01}) { frontend.press(scan); }
    require(!frontend.state().cheat_mode, "Screen grab fixture entered Test Levels");
    frontend.press(0x57);
    require(std::string(swiv3d::phase_name(frontend.state().phase)) == "mission-grab" &&
            frontend.state().mission_menu && frontend.state().selection == 10,
            "Unlocked F11 did not open the original screen-grab owner");
    const auto configuration = frontend.configuration();
    frontend.press(0xd0); frontend.press(0x1c);
    require(frontend.take_grab() == swiv3d::GrabAction::increment && frontend.take_grab() == swiv3d::GrabAction::none,
            "Single-frame increment was lost or dispatched twice");
    frontend.press(0xd0); frontend.press(0x1c);
    require(!frontend.state().grab_statistics, "Grab statistics toggle was lost");
    frontend.press(0x1c);
    require(frontend.state().grab_statistics, "Grab statistics did not toggle back");
    frontend.press(0x01); frontend.press(0x57); frontend.press(0x1c);
    require(std::string(swiv3d::phase_name(frontend.state().phase)) == "grab-name", "Grab filename editor is unreachable");
    frontend.press(0x1c);
    require(frontend.state().phase == swiv3d::FrontendPhase::grab_name, "Empty grab filename was accepted");
    for (int i = 0; i < 27; ++i) { frontend.press(0x1e); }
    require(frontend.state().grab_name == std::string(25,'a'), "Grab filename lost its original limit");
    frontend.press(0x0e); frontend.press(0x30); frontend.press(0x1c);
    require(frontend.state().grab_name == std::string(24,'a') + 'b' &&
            frontend.take_grab() == swiv3d::GrabAction::save && frontend.take_grab() == swiv3d::GrabAction::none &&
            frontend.state().selection == 10, "Grab filename editing or save ownership changed");
    frontend.press(0x1c); frontend.press(0x01);
    require(frontend.state().phase == swiv3d::FrontendPhase::mission_grab && frontend.take_grab() == swiv3d::GrabAction::none,
            "Cancelling the filename editor requested a save");
    frontend.press(0x01);
    require(frontend.state().phase == swiv3d::FrontendPhase::mission_request && frontend.configuration() == configuration,
            "Grab return changed the pilot profile or lost the mission");
}

void test_death_results_headings() {
    swiv3d::Frontend frontend(globals(CampaignFixture::two_missions), {});
    for (auto scan : {0x1c, 0x1e, 0x1c, 0x1c, 0x01, 0x1c, 0x01}) { frontend.press(scan); }
    swiv3d::MissionResult completed{swiv3d::MissionOutcome::completed};
    completed.profile = frontend.state().mission_profile;
    frontend.begin_results(completed);
    frontend.finish_mission(completed);
    swiv3d::MissionResult result{swiv3d::MissionOutcome::failed};
    result.profile = frontend.state().mission_profile;
    frontend.begin_results(result);
    std::vector<std::string> headings;
    for (const auto& row : frontend.rows()) {
        if (row.kind == 1) { headings.push_back(row.text); }
    }
    require(headings == std::vector<std::string>{"Game Over", "Rank two"},
            "Death results lost the original outcome-before-mission heading order");
    require(!frontend.rows().back().selection_bottom_border,
            "Death results ignored the retained successor of the final selected row");
}

void test_result_carry_and_profile_reload() {
    const auto archive = globals(CampaignFixture::two_missions);
    swiv3d::Frontend frontend(archive, {});
    for (auto scan : {0x1c, 0x1e, 0x1c, 0x1c, 0x01, 0x1c, 0x01}) { frontend.press(scan); }
    swiv3d::MissionResult result{swiv3d::MissionOutcome::completed};
    result.profile = frontend.state().mission_profile;
    result.profile[0] = 7;
    result.profile[40] = 144; result.profile[41] = 1;
    result.profile[44] = 2;
    result.profile[48] = 120; result.profile[49] = 0;
    result.profile[52] = 3;
    result.elapsed_updates = 1232;
    result.level_score = 400;
    result.ratios = {33,0,0};
    result.landscape_clock = {12810, 1281};
    result.game_counter = 60;
    result.control_ticks = 1201;
    result.effects_phase = 20;
    result.engine_pitch = 9691;
    result.weapon_progress = 40;
    result.flight_attitude = {18432,-900,120};
    frontend.begin_results(result);
    require(frontend.rows().at(11).text == "Level score 0", "Results did not begin at the original count-up boundary");
    frontend.advance_results(4);
    require(frontend.rows().at(11).text == "Level score 284", "Results lost the original 71-point counter step");
    const auto file = frontend.finish_mission(result);
    const auto next = frontend.state();
    require(next.phase == swiv3d::FrontendPhase::mission_request && next.level_file == "NEXT.slv" && next.level == 1,
            "Completion failed to dispatch the retained campaign successor");
    require(next.mission_profile[0] == 7 && next.mission_profile[40] == 144 && next.mission_profile[41] == 1 &&
            next.mission_profile[44] == 2 && next.mission_profile[48] == 248 && next.mission_profile[49] == 0 && next.mission_profile[52] == 3,
            "Successor lost downloaded inventory/score or the campaign health floor");
    require(next.landscape_clock.water == 1281, "Campaign transition reset landscape clocks");
    require(next.game_counter == result.game_counter, "Campaign transition lost the independent frame counter");
    require(next.control_ticks == 1201, "Campaign transition reset the retained firing phase");
    require(next.weapon_progress == result.weapon_progress,"Campaign transition reset retained launcher progress");
    require(next.effects_phase == 20 && next.engine_pitch == 9691, "Campaign transition reset retained effect/sound state");
    require(next.flight_attitude == result.flight_attitude,"Campaign transition lost ground-craft attitude accumulators");
    const auto word = [](const auto& bytes, std::size_t offset) {
        return std::uint32_t(bytes.at(offset)) | std::uint32_t(bytes.at(offset + 1)) << 8 |
               std::uint32_t(bytes.at(offset + 2)) << 16 | std::uint32_t(bytes.at(offset + 3)) << 24;
    };
    bool elapsed = false, premature_checkpoint = false;
    for (std::size_t offset = 0; offset < file.size(); offset += 128) {
        if (word(file, offset) != 20 || word(file, offset + 4) != 0) { continue; }
        if (word(file, offset + 8) == 1) { elapsed = word(file, offset + 0x2c) == 30 && word(file, offset + 0x20) == 0; }
        premature_checkpoint |= word(file, offset + 8) == 1001;
    }
    require(elapsed && !premature_checkpoint, "Saved profile moved the original persistence boundary");
    result.outcome = swiv3d::MissionOutcome::failed;
    result.elapsed_updates = 81;
    result.profile.fill(0);
    const auto failed_file = frontend.finish_mission(result);
    require(frontend.state().phase == swiv3d::FrontendPhase::main_menu, "Death did not return to the pilot menu");
    frontend.press(0xd0); frontend.press(0x1c);
    const auto unlocked = frontend.rows();
    require(frontend.state().phase == swiv3d::FrontendPhase::level_selection &&
            std::any_of(unlocked.begin(),unlocked.end(),[](const auto& row) { return row.kind == 0 && row.value == 1; }) &&
            std::none_of(unlocked.begin(),unlocked.end(),[](const auto& row) { return row.kind == 0 && (row.value == 2 || row.value == 100); }),
            "Retry menu exposed missions beyond the saved checkpoint");
    frontend.press(0x01);
    swiv3d::Frontend loaded(archive, failed_file);
    loaded.press(0xd0); loaded.press(0xd0); loaded.press(0x1c); loaded.press(0x1c);
    require(loaded.state().control_ticks == 0, "Profile reload persisted transient controller state");
    require(loaded.state().level == 1 && loaded.state().mission_profile[0] == 7 && loaded.state().mission_profile[52] == 3 &&
            loaded.state().mission_profile[44] == 2 && loaded.state().mission_profile[48] == 248,
            "Reload/continue lost the saved ground checkpoint or downloaded the death loadout");
}

void test_campaign_completion() {
    const auto archive = globals(CampaignFixture::completed_campaign);
    swiv3d::Frontend frontend(archive,{});
    for (auto scan : {0x1c,0x1e,0x1c,0x1c,0x01,0x1c,0x01}) { frontend.press(scan); }
    swiv3d::MissionResult result{swiv3d::MissionOutcome::completed};
    result.profile = frontend.state().mission_profile;
    frontend.finish_mission(result);
    require(frontend.state().phase == swiv3d::FrontendPhase::briefing && frontend.state().movie == "END",
            "Final mission skipped the campaign outro");
    frontend.press(0x01);
    require(frontend.state().phase == swiv3d::FrontendPhase::main_menu,
            "Campaign terminator did not return to the completed-pilot menu");
    const auto rows = frontend.rows();
    require(std::none_of(rows.begin(),rows.end(),[](const auto& row) { return row.kind == 0 && row.value == 2; }) &&
            std::any_of(rows.begin(),rows.end(),[](const auto& row) { return row.kind == 0 && row.value == 3; }),
            "Completed pilot offered Continue instead of the original level-selection action");
    const auto configuration = frontend.configuration();
    bool completed = false;
    for (std::size_t offset = 0; offset < configuration.size(); offset += 128) {
        if (configuration[offset] != 20 || configuration[offset + 4] != frontend.state().player || configuration[offset + 8] != 1) { continue; }
        completed = configuration[offset + 0x20] == 0x69 && configuration[offset + 0x21] == 0x4c && configuration[offset + 0x22] == 0x0b;
    }
    require(completed,"Campaign completion did not retain the original pilot marker");
    frontend.press(0x1c);
    require(frontend.state().phase == swiv3d::FrontendPhase::level_selection && frontend.state().selection == 0,
            "Completed pilot did not open its unlocked level selector");
    while (frontend.state().selection != 100) { frontend.press(0xd0); }
    frontend.press(0x1c);
    const auto page_rows = frontend.rows();
    require(frontend.state().selection == -100 &&
            std::any_of(page_rows.begin(),page_rows.end(),[](const auto& row) { return row.kind == 0 && row.value == 13; }),
            "Level selector lost its six-level overlapping page step");
    while (frontend.state().selection != 100) { frontend.press(0xd0); }
    frontend.press(0x1c);
    const auto last_page = frontend.rows();
    require(std::any_of(last_page.begin(),last_page.end(),[](const auto& row) { return row.kind == 0 && row.value == 16; }) &&
            std::none_of(last_page.begin(),last_page.end(),[](const auto& row) { return row.kind == 0 && row.value == 100; }),
            "Retry menu lost its last campaign page or allowed paging beyond it");
    frontend.press(0x01);
    frontend.press(0x1c);
    frontend.press(0x1c);
    require(frontend.state().phase == swiv3d::FrontendPhase::briefing && frontend.state().movie == "CUT",
            "Completed pilot could not retry the first campaign checkpoint");
}

void test_menu_action_feedback() {
    using swiv3d::FrontendPhase;
    swiv3d::Frontend frontend(globals(),{});
    frontend.press(0x01);
    require(frontend.take_menu_sounds().empty(),"Pilot selection played feedback for ignored Escape");
    frontend.press(0x1c);
    require(frontend.take_menu_sounds() == std::vector<int>{6003},"Menu confirmation omitted its original action sound");
    frontend.press(0x1e); frontend.press(0x0e); frontend.press(0x1e); frontend.press(0x1c);
    require(frontend.take_menu_sounds().empty(),"Pilot name editing emitted menu action feedback");
    frontend.press(0x01);
    require(frontend.state().phase == FrontendPhase::controller_type &&
            frontend.take_menu_sounds().empty(),"Controller selection played feedback for ignored Escape");
    frontend.press(0x1c); frontend.press(0x01);
    require(frontend.take_menu_sounds() == std::vector<int>{6003,6003},"Controller menu confirmation/cancel feedback lost order");
    frontend.press(0xd0);
    require(frontend.state().selection == 4,"Main menu test did not reach Setup");
    frontend.take_menu_sounds();
    frontend.press(0x3b);
    require(frontend.state().phase == FrontendPhase::main_menu && frontend.state().selection == 4 &&
            frontend.take_menu_sounds().empty(),"Main menu changed selection or played feedback for ignored F1");
    frontend.press(0x1c); frontend.press(0x1c); frontend.press(0xd0); frontend.press(0x1c);
    require(frontend.state().phase == FrontendPhase::key_capture,"Menu feedback test did not enter remapping");
    frontend.take_menu_sounds();
    frontend.press(0x01);
    require(frontend.take_menu_sounds().empty(),"Rejected remapping key emitted acceptance feedback");
    frontend.press(0x1e);
    require(frontend.state().phase == FrontendPhase::controller_bindings &&
            frontend.take_menu_sounds() == std::vector<int>{6003} && frontend.take_menu_sounds().empty(),
            "Accepted remapping key omitted one-shot action feedback");
}

void test_raw_editor_keys() {
    using swiv3d::FrontendPhase;
    swiv3d::Frontend frontend(globals(), {});
    frontend.press(0x1c);
    frontend.press(0x1e);
    frontend.press(0x9c);
    require(frontend.state().phase == FrontendPhase::pilot_name,
            "Pilot editor normalized keypad Enter before consuming the raw key");
    frontend.press(0x1c);
    frontend.press(0x1c);
    frontend.press(0xd0);
    frontend.press(0x1c);
    require(frontend.state().phase == FrontendPhase::key_capture,
            "Raw-key fixture did not enter controller remapping");
    frontend.take_menu_sounds();
    const auto before = frontend.configuration();
    for (auto scan : {0, 0x100, 0x01}) {
        frontend.press(scan);
    }
    require(frontend.state().phase == FrontendPhase::key_capture && frontend.configuration() == before &&
            frontend.take_menu_sounds().empty(), "Rejected raw binding changed the editor or profile");
    frontend.press(0x48);
    require(frontend.state().phase == FrontendPhase::controller_bindings && frontend.state().selection == 21 &&
            frontend.take_menu_sounds() == std::vector<int>{6003},
            "Raw keypad binding lost its selected action or acceptance sound");
    const auto configuration = frontend.configuration();
    bool found = false;
    for (std::size_t offset = 0; offset < configuration.size(); offset += 128) {
        if (configuration[offset] != 20 || configuration[offset + 8] != 2) {
            continue;
        }
        found = true;
        require(configuration[offset + 16] == 0x48,
                "Controller remapping normalized a raw keypad scan code");
    }
    require(found, "Raw keypad binding lost its controller record");
}

void test_cancel_confirmation_age() {
    using swiv3d::FrontendPhase;
    constexpr unsigned confirmation_draws = 4;
    swiv3d::Frontend frontend(globals(), {});
    for (unsigned draw = 0; draw < confirmation_draws; ++draw) {
        require(!frontend.accepts_queued_key(0x1c) && !frontend.accepts_queued_key(0x9c),
                "Menu confirmation became available before four completed draws");
        frontend.presented();
    }
    frontend.press(0x01);
    require(frontend.accepts_queued_key(0x1c) && frontend.take_menu_sounds().empty(),
            "Ignored pilot cancellation reset confirmation age or emitted feedback");
    for (auto scan : {0x1c, 0x1e, 0x1c}) {
        frontend.press(scan);
    }
    for (unsigned draw = 0; draw < confirmation_draws; ++draw) {
        frontend.presented();
    }
    frontend.press(0x01);
    require(frontend.state().phase == FrontendPhase::controller_type && frontend.accepts_queued_key(0x9c),
            "Ignored controller cancellation reset confirmation age");
    for (auto scan : {0x1c, 0x01, 0xd0, 0x1c}) {
        frontend.press(scan);
    }
    require(frontend.state().phase == FrontendPhase::settings, "Cancellation fixture did not reach settings");
    for (unsigned draw = 0; draw < confirmation_draws; ++draw) {
        frontend.presented();
    }
    frontend.take_menu_sounds();
    frontend.press(0x3b);
    require(frontend.state().phase == FrontendPhase::main_menu && frontend.state().selection == 4 &&
            !frontend.accepts_queued_key(0x1c) && frontend.take_menu_sounds() == std::vector<int>{6003},
            "F1 cancellation lost menu selection, confirmation reset or action feedback");
}

void test_unlocked_key_remapping() {
    swiv3d::Frontend frontend(globals(),{});
    frontend.press(0x1c);
    for (unsigned index = 0; index < std::string("SWIV!AndLetDie").size(); ++index) { frontend.press(0x60 + index); }
    frontend.press(0x1c);
    frontend.press(0x1e); frontend.press(0x1c); frontend.press(0x1c);
    frontend.press(0xd0); frontend.press(0x1c);
    require(frontend.state().phase == swiv3d::FrontendPhase::key_capture,"Unlocked remapping test did not reach key capture");
    frontend.press(0x01);
    require(frontend.state().phase == swiv3d::FrontendPhase::controller_bindings,
            "Unlocked pilot could not bind the original reserved key");
    const auto configuration = frontend.configuration();
    bool found = false;
    for (std::size_t offset = 0; offset < configuration.size(); offset += 128) {
        if (configuration[offset] != 20 || configuration[offset + 8] != 2) { continue; }
        found = true;
        require(configuration[offset + 16] == 1,"Unlocked remapping did not persist the reserved scan");
    }
    require(found,"Unlocked remapping lost its controller record");
}

void test_settings_entry() {
    using swiv3d::FrontendPhase;
    swiv3d::Frontend frontend(globals(),{});
    for (auto scan : {0x1c,0x1e,0x1c,0x1c,0x01,0xd0,0x1c}) { frontend.press(scan); }
    require(std::string(swiv3d::phase_name(frontend.state().phase)) == "settings", "Setup did not enter settings");
    frontend.press(0x1c); frontend.press(0xd0); frontend.press(0x1c);
    require(frontend.state().phase == FrontendPhase::key_capture, "Multiline controller row trapped navigation");
    frontend.press(0);
    frontend.press(0x01);
    require(frontend.state().phase == FrontendPhase::key_capture, "Reserved Escape was accepted as a binding");
    frontend.press(0x1e);
    require(frontend.state().phase == FrontendPhase::controller_bindings && frontend.state().selection == 21,
            "Remapping lost its selected action");
    for (int action : {21,22,24,25,26,27,28,29,30,31,32}) {
        while (frontend.state().selection != action) { frontend.press(0xd0); }
        frontend.press(0x1c); frontend.press(0x1e);
        require(frontend.state().phase == FrontendPhase::controller_bindings && frontend.state().selection == action,
                "A keyboard action failed to return from remapping");
    }
    const auto remapped = frontend.configuration();
    bool controls_found = false;
    for (std::size_t offset = 0; offset < remapped.size(); offset += 128) {
        if (remapped[offset] != 20 || remapped[offset+4] != 0 || remapped[offset+8] != 2) { continue; }
        controls_found = true;
        for (std::size_t key = 0; key < 11; ++key) {
            require(remapped[offset+16+key*4] == 0x1e, "Remapping wrote the wrong controller field");
        }
    }
    require(controls_found,"Remapping lost its controller record");
    frontend.press(0x3b); frontend.press(0x50); frontend.press(0x9c);
    require(frontend.state().phase == FrontendPhase::sound_volume && frontend.state().selection == 3,
            "Volume selected an unavailable mixer control");
    frontend.press(0xcb);
    require(frontend.state().volumes[2] == 253, "Effects slider did not change its retained value");
    frontend.press(0x4b);
    require(frontend.state().volumes[2] == 251,"Keypad Left did not adjust the effects slider");
    frontend.press(0x4d);
    require(frontend.state().volumes[2] == 253,"Keypad Right did not adjust the effects slider");
    frontend.take_menu_sounds();
    frontend.press(0xcb); frontend.press(0xcd); frontend.press(0xd0);
    require(frontend.take_menu_sounds() == std::vector<int>{6004,6004,6002} && frontend.take_menu_sounds().empty(),
            "Menu feedback lost its resource order or one-shot consumption");
    frontend.press(0x01); frontend.press(0xd0); frontend.press(0x1c); frontend.press(0x1c);
    frontend.press(0xd0); frontend.press(0x1c); frontend.press(0xd0); frontend.press(0x1c);
    require(frontend.state().gamma == 1, "Gamma did not update the pilot");
    frontend.press(0x01); frontend.press(0xd0); frontend.press(0x1c); frontend.press(0xd0); frontend.press(0x1c);
    require(frontend.state().music_type == 1, "Music preference did not update the pilot");
    const auto saved = frontend.configuration_file();
    swiv3d::Frontend loaded(globals(),saved);
    loaded.press(0xd0); loaded.press(0xd0); loaded.press(0x1c);
    require(loaded.state().gamma == 1 && loaded.state().music_type == 1,
            "Reload lost presentation preferences");
    frontend.press(0x01);
    while (frontend.state().selection != 7) { frontend.press(0xd0); }
    frontend.press(0x1c); frontend.press(0xd0); frontend.press(0x1c);
    require(frontend.state().phase == FrontendPhase::pilot_deletion, "Pilot deletion was not reachable");
    const auto before = frontend.configuration();
    frontend.press(0x01);
    require(frontend.configuration() == before, "Cancelling deletion changed the profile store");
    frontend.press(0x1c); frontend.press(0x1c);
    require(frontend.state().phase == FrontendPhase::pilot_selection && frontend.state().selection == 0,
            "Deleted pilot remained selectable");
    for (auto scan : {0x1c,0x30,0x1c}) { frontend.press(scan); }
    require(frontend.state().phase == FrontendPhase::controller_type && frontend.state().gamma == 0,
            "Reused pilot inherited deleted controls or presentation settings");
}

void test_pilot_record_deletion() {
    swiv3d::Frontend empty(globals(),{});
    auto records = empty.configuration();
    const auto seed = [&](std::uint32_t player, std::uint32_t part, std::uint8_t marker) {
        for (std::size_t offset = 0; offset < records.size(); offset += 128) {
            if (records[offset] != 255) { continue; }
            std::fill_n(records.begin()+offset,128,0);
            put(records,offset,20); put(records,offset+4,player); put(records,offset+8,part);
            records[offset+12] = marker;
            return;
        }
        throw std::runtime_error("Synthetic profile capacity exhausted");
    };
    seed(0,2,1); seed(0,1000,77); seed(0,1016,88);
    seed(1,2,1); seed(1,1000,99);
    for (std::size_t offset = 0; offset < records.size(); offset += 128) {
        if (records[offset] == 20 && records[offset+8] == 1 && records[offset+4] < 2) {
            records[offset+12] = records[offset+4] == 0 ? 'a' : 'b';
        }
    }
    swiv3d::Frontend frontend(globals(),records);
    frontend.press(0xd0); frontend.press(0x1c); frontend.press(0x1c);
    const auto deleted = frontend.configuration();
    for (std::size_t offset = 0; offset < deleted.size(); offset += 128) {
        require(!(deleted[offset] == 20 && deleted[offset+4] == 0 && deleted[offset+8] != 1),
                "Pilot deletion retained controls or checkpoints");
        if (records[offset] == 20 && records[offset+4] == 1) {
            require(std::equal(records.begin()+offset,records.begin()+offset+128,deleted.begin()+offset),
                    "Pilot deletion modified another pilot's records");
        }
    }
}

void select(swiv3d::Frontend& frontend,int action) {
    for (int i = 0; i < 40 && frontend.state().selection != action; ++i) { frontend.press(0xd0); }
    require(frontend.state().selection == action,"Menu action is unreachable");
    frontend.press(0x1c);
}

void test_device_and_cheat_routes() {
    swiv3d::Frontend frontend(globals(),{});
    frontend.devices({true,true});
    frontend.press(0x1c);
    for (unsigned i = 0; i < std::string("SWIV!AndLetDie").size(); ++i) { frontend.press(0x60+i); }
    frontend.press(0x1c);
    require(frontend.state().phase == swiv3d::FrontendPhase::pilot_name,"Cheat name became a real pilot name");
    frontend.press(0x1e); frontend.press(0x1c);
    select(frontend,2);
    select(frontend,20);
    require(frontend.state().phase == swiv3d::FrontendPhase::axis_bindings,"Mouse axis setup is unreachable");
    select(frontend,1); select(frontend,1); // X -> Y -> keyboard.
    frontend.press(0x01);
    require(frontend.rows().at(3).text == "Mouse","Controller identity was lost");
    frontend.press(0x01);
    select(frontend,4); select(frontend,15);
    require(frontend.state().debug_controls,"Debug setting has no retained consumer state");
    frontend.press(0x01);
    select(frontend,9);
    require(frontend.state().phase == swiv3d::FrontendPhase::level_selection,"Unlocked test-level route is unreachable");
    select(frontend,0); frontend.press(0x01);
    require(frontend.state().cheat_mode && frontend.state().phase == swiv3d::FrontendPhase::mission_request,
            "Test level did not retain cheat mode through briefing");
    frontend.press(0x58);
    require(frontend.state().phase == swiv3d::FrontendPhase::cheat_menu,"F12 did not open the test-level cheat inventory");
    select(frontend,0);
    require(frontend.take_cheat() == 0 && frontend.take_cheat() == -1,"Cheat action was lost or dispatched twice");
    select(frontend,-9); select(frontend,-9); select(frontend,200);
    require(frontend.take_cheat() == 200,"Craft change action did not reach the mission boundary");
    frontend.press(0x01);
    frontend.press(0x3b); frontend.press(0x01);
    require(frontend.state().phase == swiv3d::FrontendPhase::mission_request && !frontend.state().mission_menu,
            "In-mission setup did not resume the retained mission");
}

}

void test_main_menu_presentations() {
    swiv3d::Frontend frontend(globals(),{});
    for (auto scan : {0x1c,0x1e,0x1c,0x1c,0x01}) { frontend.press(scan); }
    const auto profile = frontend.configuration_file();
    select(frontend,8);
    require(std::string(swiv3d::phase_name(frontend.state().phase)) == "exit-confirmation",
            "Exit did not open confirmation");
    require(frontend.state().selection == 2,"Exit confirmation did not default to No");
    frontend.press(0x01);
    require(frontend.state().selection == 8,"Cancelling exit lost main-menu selection");
    select(frontend,6);
    require(std::string(swiv3d::phase_name(frontend.state().phase)) == "credits","Credits are unreachable");
    for (int page = 0; page < 3; ++page) { frontend.press(0x39); }
    require(frontend.state().phase == swiv3d::FrontendPhase::main_menu && frontend.state().selection == 6,
            "Credits did not return to the selected menu action");
    select(frontend,5);
    require(std::string(swiv3d::phase_name(frontend.state().phase)) == "opening-movies","Replay intro is unreachable");
    frontend.finish_presentation();
    require(frontend.state().phase == swiv3d::FrontendPhase::main_menu && frontend.state().selection == 5,
            "Replayed intro lost the selected pilot/menu action");
    for (unsigned clock = 0; clock < 4; ++clock) {
        select(frontend,8);
        require(frontend.rows().at(2).text == std::array<const char*,4>{"Exit?","Exit one?","Exit two?","Exit three?"}[clock],
                "Exit prompt ignored the retained timer");
        frontend.press(0x1c);
        require(frontend.state().phase == swiv3d::FrontendPhase::main_menu,"No did not cancel exit");
        frontend.advance_clock(1);
    }
    require(frontend.configuration_file() == profile,"Menu presentations changed the pilot profile");
    select(frontend,8); select(frontend,1);
    require(std::string(swiv3d::phase_name(frontend.state().phase)) == "quit","Yes did not request clean shutdown");
    frontend.press(0x1c);
    require(std::string(swiv3d::phase_name(frontend.state().phase)) == "quit","Shutdown accepted more menu input");
}

void test_cheat_playtime() {
    for (auto outcome : {swiv3d::MissionOutcome::completed,swiv3d::MissionOutcome::failed}) {
        for (bool results : {false,true}) {
            swiv3d::Frontend frontend(globals(CampaignFixture::two_missions),{});
            frontend.press(0x1c);
            for (unsigned i = 0; i < std::string("SWIV!AndLetDie").size(); ++i) { frontend.press(0x60+i); }
            frontend.press(0x1c); frontend.press(0x1e); frontend.press(0x1c); frontend.press(0x1c); frontend.press(0x01);
            select(frontend,9); select(frontend,0); frontend.press(0x01);
            require(frontend.state().cheat_mode,"Playtime test lost Test Levels mode");
            swiv3d::MissionResult result{outcome};
            result.profile = frontend.state().mission_profile;
            result.elapsed_updates = 1232;
            if (results) { frontend.begin_results(result); }
            const auto file = frontend.finish_mission(result);
            bool accounted = false;
            for (std::size_t offset = 0; offset < file.size(); offset += 128) {
                if (file[offset] == 20 && file[offset+4] == 0 && file[offset+8] == 1) {
                    accounted = file[offset+0x2c] == 30 && file[offset+0x2d] == 0;
                }
                require(!(file[offset] == 20 && file[offset+4] == 0 && file[offset+8] == 233 && file[offset+9] == 3),
                        "Test Levels created an earned successor checkpoint");
            }
            require(accounted,"Test Levels omitted original pilot playtime accounting");
        }
    }
}

void test_viewport_shortcuts() {
    swiv3d::Frontend frontend(globals(), {});
    for (auto scan : {0x1c,0x1e,0x1c,0x1c,0x01,0x1c,0x01}) { frontend.press(scan); }
    require(frontend.state().phase == swiv3d::FrontendPhase::mission_request,"Viewport test did not enter a mission");
    frontend.press(0x4a);
    require(frontend.state().screen_size == 250,"Keypad minus did not shrink the mission viewport");
    for (unsigned i = 0; i < 51; ++i) { frontend.press(0x4a); }
    require(frontend.state().screen_size == 0,"Viewport shortcut did not clamp at zero");
    frontend.press(0x4a); frontend.press(0x4e);
    require(frontend.state().screen_size == 5,"Simultaneous viewport keys lost subtraction-first clamping");
    for (unsigned i = 0; i < 51; ++i) { frontend.press(0x4e); }
    require(frontend.state().screen_size == 255,"Viewport shortcut did not clamp at full size");
}

void viewport_cases(const char* fixture) {
    std::ifstream input(fixture);
    const auto archive = globals();
    unsigned initial, mask;
    while (input >> initial >> mask) {
        require(initial <= 255 && mask < 4,"Invalid viewport fixture");
        swiv3d::Frontend frontend(archive, {});
        for (auto scan : {0x1c,0x1e,0x1c,0x1c,0x01,0x1c,0x01,0x3b}) { frontend.press(scan); }
        while (frontend.state().selection != 17) { frontend.press(0xd0); }
        if (initial % 2 == 0) {
            for (unsigned i = 0; i < 128; ++i) { frontend.press(0xcb); }
            while (frontend.state().screen_size < int(initial)) { frontend.press(0xcd); }
        } else {
            while (frontend.state().screen_size > int(initial)) { frontend.press(0xcb); }
        }
        frontend.press(0x01);
        if ((mask & 1) != 0) { frontend.press(0x4a); }
        if ((mask & 2) != 0) { frontend.press(0x4e); }
        std::cout << frontend.state().screen_size << '\n';
    }
    require(input.eof(),"Truncated viewport fixture");
}

void playtime_cases(const char* fixture) {
    std::ifstream input(fixture);
    unsigned outcome, mode, updates, prior, results;
    const auto archive = globals(CampaignFixture::two_missions);
    while (input >> outcome >> mode >> updates >> prior >> results) {
        require(outcome < 3 && mode < 2 && results < 2,"Invalid playtime fixture");
        swiv3d::Frontend setup(archive,{});
        setup.press(0x1c);
        for (unsigned i = 0; i < std::string("SWIV!AndLetDie").size(); ++i) { setup.press(0x60+i); }
        setup.press(0x1c); setup.press(0x1e); setup.press(0x1c); setup.press(0x1c); setup.press(0x01);
        auto config = setup.configuration();
        for (std::size_t offset = 0; offset < config.size(); offset += 128) {
            if (config[offset] == 20 && config[offset+4] == 0 && config[offset+8] == 1) { put(config,offset+0x2c,prior); }
        }
        swiv3d::Frontend frontend(archive,config);
        select(frontend,100);
        if (mode != 0) { select(frontend,9); select(frontend,0); }
        else { select(frontend,1); }
        frontend.press(0x01);
        swiv3d::MissionResult result{outcome == 0 ? swiv3d::MissionOutcome::completed : swiv3d::MissionOutcome::failed};
        result.profile = frontend.state().mission_profile;
        result.elapsed_updates = updates;
        if (results != 0) { frontend.begin_results(result); }
        const auto file = frontend.finish_mission(result);
        for (std::size_t offset = 0; offset < file.size(); offset += 128) {
            if (file[offset] == 20 && file[offset+4] == 0 && file[offset+8] == 1) {
                const auto time = offset + 0x2c;
                std::cout << (std::uint32_t(file[time]) | std::uint32_t(file[time+1]) << 8 |
                              std::uint32_t(file[time+2]) << 16 | std::uint32_t(file[time+3]) << 24) << '\n';
            }
        }
    }
    require(input.eof(),"Truncated playtime fixture");
}

int main(int argc, char** argv) {
    try {
        if (argc == 3 && std::string(argv[1]) == "--viewport-cases") { viewport_cases(argv[2]); return 0; }
        if (argc == 3 && std::string(argv[1]) == "--playtime-cases") { playtime_cases(argv[2]); return 0; }
        require(argc == 1,"Expected no arguments or a contract fixture option");
        test_retained_route();
        test_input_line_endings();
        test_viewport_shortcuts();
        test_cheat_playtime();
        test_mission_pause_and_abort();
        test_screen_grab_entry();
        test_result_carry_and_profile_reload();
        test_death_results_headings();
        test_campaign_completion();
        test_unlocked_key_remapping();
        test_raw_editor_keys();
        test_cancel_confirmation_age();
        test_menu_action_feedback();
        test_settings_entry();
        test_pilot_record_deletion();
        test_device_and_cheat_routes();
        test_main_menu_presentations();
        std::cout << "Retained pilot, keyboard and campaign route passed\n";
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
