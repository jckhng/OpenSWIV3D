#pragma once

#include "swiv3d/frontend.hpp"
#include "swiv3d/mission.hpp"
#include "audio.hpp"
#include "cutscene.hpp"

#include <functional>
#include <istream>

namespace swiv3d::detail {

enum class MoviePlayback { skip, play };
enum class MovieTiming { updates, device };
enum class FrontendPlayback { scripted, interactive };
enum class CutsceneEnd { finished, skipped };

void replay_frontend(Frontend& frontend, std::istream& input, const std::function<void()>& checkpoint = {},
                     MoviePlayback movies = MoviePlayback::skip);

struct FlightSegment {
    std::uint32_t updates;
    KeyboardState keys{};
};

std::vector<FlightSegment> read_flight(std::istream& input);

struct MissionSession {
    Archive globals;
    std::unique_ptr<Mission> mission;
    std::unique_ptr<Frontend> frontend;
    std::filesystem::path game, profile;
    KeyboardState previous_keys{};
    std::unique_ptr<Audio> audio;
    unsigned cd_timer = 0;
    MoviePlayback movies = MoviePlayback::skip;
    std::unique_ptr<Cutscene> cutscene;
    FrontendPlayback frontend_playback = FrontendPlayback::scripted;
    unsigned opening_movie = 0;
    std::uint32_t menu_updates = 0;
    std::int32_t render_rate = mission_tick_rate;
    std::uint64_t movie_preparation_work = 0;
    MissionFrame movie_preparation_frame{};
    MovieTiming movie_timing = MovieTiming::updates;
    std::uint64_t movie_video_work = 0;
    MoviePreparation movie_preparation;
};

MissionSession start_mission(const std::filesystem::path& game, const std::filesystem::path& input,
                             const std::filesystem::path& profile = {}, MoviePlayback movies = MoviePlayback::skip);
MissionSession start_frontend(const std::filesystem::path& game, const std::filesystem::path& profile = {});
void complete_cutscene(MissionSession& session, CutsceneEnd end);
void begin_results(MissionSession& session);
void finish_mission(MissionSession& session);
void frontend_key(MissionSession& session, std::uint32_t scan, KeyShift shift = KeyShift::released,
                  const KeyboardState& keyboard = {});
void abort_mission(MissionSession& session);
void advance_session(MissionSession& session, const KeyboardState& keys, const DeviceInput& devices = {});
MissionFrame render_session(MissionSession& session, RenderTiming timing = {}, const KeyboardState& shortcut_keys = {});
void present_audio(MissionSession& session, const MissionFrame& frame);
std::vector<std::int16_t> mix_session(MissionSession& session, std::size_t frames);
std::vector<std::int16_t> mix_session_device(MissionSession& session, std::size_t frames);

}
