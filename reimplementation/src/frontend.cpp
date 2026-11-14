#include "swiv3d/frontend.hpp"

#include <algorithm>
#include <cstdio>
#include <map>
#include <stdexcept>
#include <utility>

namespace swiv3d {
namespace {

constexpr std::size_t record_bytes = 128;
constexpr std::size_t record_count = 300;
constexpr std::size_t record_payload = 12;
constexpr std::size_t payload_bytes = 112;
constexpr std::size_t pilot_count = 10;
constexpr std::size_t name_limit = 15;
constexpr std::uint32_t missing = UINT32_MAX;
constexpr std::uint32_t preferences_kind = 10;
constexpr std::uint32_t pilot_kind = 20;
constexpr std::uint32_t global_preferences = 2000;
constexpr std::uint32_t pilot_identity = 1;
constexpr std::uint32_t pilot_controls = 2;
constexpr std::uint32_t checkpoint_base = 1000;
constexpr std::uint32_t pilot_action_base = 100;
constexpr std::size_t preference_language_offset = 0x10;
constexpr std::size_t pilot_language_offset = 0x1c;
constexpr std::size_t pilot_level_offset = 0x20;
constexpr std::size_t pilot_music_offset = 0x24;
constexpr std::size_t pilot_gamma_offset = 0x30;
constexpr std::size_t pilot_cheat_offset = 0x28;
constexpr std::size_t controller_type_offset = 0x0c;
constexpr std::size_t controller_keys_offset = 0x10;
constexpr std::size_t controller_buttons_offset = 0x3c;
constexpr std::uint32_t completed_campaign = 0xb4c69;
constexpr int level_page_rows = 8;
constexpr int level_page_step = 6;
constexpr int previous_level_page = -100;
constexpr int next_level_page = 100;
constexpr std::uint32_t string_resource = 100;
constexpr std::uint32_t sequence_resource = 600;
constexpr std::uint32_t width_resource = 508;
constexpr std::uint32_t all_language_page = 99;
constexpr std::uint32_t character_page_group = 2;
constexpr std::uint32_t keyboard_group = 1001;
constexpr std::uint32_t key_labels_group = 1000;
constexpr std::int32_t background_colour = 0xa1;
constexpr std::int32_t bright_colour = 0xff;
constexpr std::int32_t prompt_colour = 0x76;
constexpr std::int32_t selectable_row = 0;
constexpr std::int32_t heading_row = 1;
constexpr std::int32_t disabled_row = 2;
constexpr std::int32_t line_row = 3;
constexpr std::int32_t slider_row = 4;
constexpr std::int32_t detail_row = 5;
constexpr std::uint32_t binding_actions[]{21,22,24,25,26,27,28,29,30,31,32};
constexpr std::size_t binding_offsets[]{0x10,0x14,0x18,0x1c,0x20,0x24,0x38,0x28,0x2c,0x30,0x34};
constexpr int continuous_slider_step = 2;
constexpr int viewport_shortcut_step = 5;
constexpr int slider_maximum = 255;
constexpr int detail_slider_step = 75;
constexpr int detail_minimum = 15;
constexpr int detail_maximum = 240;
constexpr std::size_t sequence_record_bytes = 16;
constexpr std::uint32_t short_sign_bit = 0x8000;
constexpr std::uint32_t short_modulus = 0x10000;
constexpr std::uint32_t sequence_short_bias = 0xbc;
constexpr std::uint32_t rotor_step = 10;
constexpr unsigned exit_prompt_count = 4;
constexpr unsigned first_exit_prompt = 30;
constexpr std::size_t grab_name_limit = 25;
constexpr int default_grab_zoom = 96;
constexpr int menu_move_sound = 6002, menu_action_sound = 6003, menu_slider_sound = 6004;
constexpr std::uint32_t menu_confirmation_draws = 4;

enum Scan : std::uint32_t {
    escape = 0x01, backspace = 0x0e, enter = 0x1c, control = 0x1d,
    special_fire = 0x32, comma = 0x33, period = 0x34, space = 0x39, f1 = 0x3b,
    keypad_up = 0x48, keypad_left = 0x4b, keypad_right = 0x4d, keypad_down = 0x50, keypad_enter = 0x9c,
    up = 0xc8, page_up = 0xc9, left = 0xcb, right = 0xcd,
    down = 0xd0, page_down = 0xd1
};

std::uint32_t read_word(const std::uint8_t* bytes) {
    return static_cast<std::uint32_t>(bytes[0]) | static_cast<std::uint32_t>(bytes[1]) << 8 |
           static_cast<std::uint32_t>(bytes[2]) << 16 | static_cast<std::uint32_t>(bytes[3]) << 24;
}

std::int64_t signed_word(std::uint32_t value) {
    return value <= INT32_MAX ? static_cast<std::int64_t>(value) :
                               static_cast<std::int64_t>(value) - (INT64_C(1) << 32);
}

void write_word(std::uint8_t* bytes, std::uint32_t value) {
    for (std::size_t i = 0; i < sizeof(value); ++i) {
        bytes[i] = static_cast<std::uint8_t>(value >> (i * 8));
    }
}

std::uint32_t word(const std::vector<std::uint8_t>& bytes, std::size_t offset) {
    if (offset > bytes.size() || bytes.size() - offset < sizeof(std::uint32_t)) {
        throw std::runtime_error("Truncated frontend resource");
    }
    return read_word(bytes.data() + offset);
}

std::string string_at(const std::vector<std::uint8_t>& bytes, std::size_t offset) {
    if (offset >= bytes.size()) {
        throw std::runtime_error("String offset exceeds resource");
    }
    const auto start = bytes.begin() + offset;
    const auto end = std::find(start, bytes.end(), 0);
    if (end == bytes.end()) {
        throw std::runtime_error("Unterminated frontend string");
    }
    return {start, end};
}

std::string substitute(std::string format, const std::string& value) {
    const auto offset = format.find("%s");
    if (offset == std::string::npos) {
        throw std::runtime_error("Unsupported frontend format string");
    }
    format.replace(offset, 2, value);
    return format;
}

class Record {
public:
    std::uint32_t get(std::size_t offset) const {
        if (offset > record_bytes - sizeof(std::uint32_t)) {
            throw std::runtime_error("Profile field exceeds record");
        }
        return read_word(data_.data() + offset);
    }

    void set(std::size_t offset, std::uint32_t value) {
        if (offset > record_bytes - sizeof(std::uint32_t)) {
            throw std::runtime_error("Profile field exceeds record");
        }
        write_word(data_.data() + offset, value);
    }

    std::string name() const {
        const auto start = data_.begin() + record_payload;
        return {start, std::find(start, start + name_limit + 1, 0)};
    }

    void set_name(const std::string& value) {
        if (value.size() > name_limit) {
            throw std::runtime_error("Pilot name exceeds original limit");
        }
        std::copy(value.begin(), value.end(), data_.begin() + record_payload);
        data_[record_payload + value.size()] = 0;
    }

    void copy_payload(const Record& source) {
        std::copy_n(source.data_.begin() + record_payload, payload_bytes, data_.begin() + record_payload);
    }

    void set_payload(const std::array<std::uint8_t, payload_bytes>& value) {
        std::copy(value.begin(), value.end(), data_.begin() + record_payload);
    }

    void reset() { data_.fill(0); }

    void load(const std::vector<std::uint8_t>& file, std::size_t offset) {
        const auto length = std::min(record_bytes, file.size() - offset);
        std::copy_n(file.begin() + offset, length, data_.begin());
    }

    void append(std::vector<std::uint8_t>& destination) const {
        destination.insert(destination.end(), data_.begin(), data_.end());
    }

    void set_buttons(const std::array<std::uint8_t, 4>& buttons) {
        std::copy(buttons.begin(), buttons.end(), data_.begin() + controller_buttons_offset);
    }

    std::array<std::uint8_t,4> buttons() const {
        std::array<std::uint8_t,4> result;
        std::copy_n(data_.begin() + controller_buttons_offset,4,result.begin());
        return result;
    }

    std::array<std::uint8_t, payload_bytes> payload() const {
        std::array<std::uint8_t, payload_bytes> result{};
        std::copy_n(data_.begin() + record_payload, payload_bytes, result.begin());
        return result;
    }

    std::int32_t short_value(std::size_t offset) const {
        if (offset > record_bytes - 2) {
            throw std::runtime_error("Short field exceeds profile");
        }
        const auto bits = data_[offset] | static_cast<std::uint32_t>(data_[offset + 1]) << 8;
        return bits < short_sign_bit ? static_cast<std::int32_t>(bits) :
                                      static_cast<std::int32_t>(bits) - static_cast<std::int32_t>(short_modulus);
    }

    void set_short(std::size_t offset, std::uint32_t value) {
        if (offset > record_bytes - 2) {
            throw std::runtime_error("Short field exceeds profile");
        }
        data_[offset] = static_cast<std::uint8_t>(value);
        data_[offset + 1] = static_cast<std::uint8_t>(value >> 8);
    }

private:
    std::array<std::uint8_t, record_bytes> data_{};
};

class Profiles {
public:
    explicit Profiles(const std::vector<std::uint8_t>& file) {
        for (auto& record : records_) {
            record.set(0, missing);
            record.set(4, missing);
            record.set(8, missing);
        }
        for (std::size_t i = 0; i < record_count && i * record_bytes < file.size(); ++i) {
            records_[i].load(file, i * record_bytes);
            if (records_[i].get(0) == missing) {
                break;
            }
        }
    }

    Record& preferences() { return get(preferences_kind, global_preferences, missing); }
    Record& pilot(std::uint32_t player) { return get(pilot_kind, player, pilot_identity); }
    Record& controls(std::uint32_t player) { return get(pilot_kind, player, pilot_controls); }
    bool cheats(std::uint32_t player) const {
        for (const auto& record : records_) {
            if (record.get(0) == pilot_kind && record.get(4) == player && record.get(8) == pilot_identity) {
                return record.get(pilot_cheat_offset) != 0;
            }
        }
        return false;
    }
    Record& checkpoint(std::uint32_t player, std::uint32_t level) {
        return get(pilot_kind, player, checkpoint_base + level);
    }

    void erase_pilot(std::uint32_t player) {
        for (auto& record : records_) {
            if (record.get(0) == pilot_kind && record.get(4) == player) {
                record.set(0,missing); record.set(4,missing); record.set(8,missing);
            }
        }
    }

    std::vector<std::uint8_t> snapshot() const {
        std::vector<std::uint8_t> result;
        result.reserve(record_bytes * record_count);
        for (const auto& record : records_) {
            record.append(result);
        }
        return result;
    }

    std::vector<std::uint8_t> file() const {
        std::vector<std::uint8_t> result;
        // CONFIG_deinit scans 150 records despite the 300-record memory store.
        for (std::size_t i = 0; i < record_count / 2; ++i) {
            if (records_[i].get(0) != missing) { records_[i].append(result); }
        }
        const auto terminator = result.size();
        result.resize(terminator + record_bytes);
        write_word(result.data() + terminator, missing);
        return result;
    }

private:
    Record& get(std::uint32_t kind, std::uint32_t id, std::uint32_t part) {
        for (auto& record : records_) {
            if (record.get(0) == kind && record.get(4) == id && record.get(8) == part) {
                return record;
            }
        }
        for (auto& record : records_) {
            if (record.get(0) != missing) {
                continue;
            }
            record.reset();
            record.set(0, kind);
            record.set(4, id);
            record.set(8, part);
            return record;
        }
        throw std::runtime_error("Original profile capacity exhausted");
    }

    std::array<Record, record_count> records_{};
};

class MenuResources {
public:
    explicit MenuResources(const Archive& archive) {
        // GLOBAL_init_ load order; string/font pointers remain resource-relative.
        constexpr std::uint32_t ids[]{1000, 100, 200, 501, 502, 503, 504, 505, 506,
                                      507, 508, 510, 511, 512, 600, 800, 700};
        for (auto id : ids) {
            members_.emplace(id, archive.load(id));
        }
    }

    const std::vector<std::uint8_t>& member(std::uint32_t id) const { return members_.at(id); }

    std::string text(std::uint32_t group, std::uint32_t id, std::uint32_t language) const {
        const auto& bytes = member(string_resource);
        const auto table = find(bytes, 0, group + (language << 16));
        return string_at(bytes, find(bytes, table, id));
    }

    std::int32_t width(const std::string& text_value, std::uint32_t language) const {
        const auto pages = text(character_page_group, 0, all_language_page);
        if (language >= pages.size() || pages[language] < '0' || pages[language] > '1') {
            throw std::runtime_error("Unsupported menu character page");
        }
        const auto& widths = member(width_resource);
        std::int32_t result = 0;
        for (unsigned char character : text_value) {
            auto index = static_cast<std::uint32_t>(character);
            if (character == 127) {
                index = 0; // GLOBAL_setupchartab_ deliberately leaves slot 127 zero.
            } else if (character >= 128) {
                index += static_cast<std::uint32_t>(pages[language] - '0') * 128;
            }
            result += static_cast<std::int32_t>(word(widths, index * sizeof(std::uint32_t)));
        }
        return result;
    }

private:
    static std::uint32_t find(const std::vector<std::uint8_t>& bytes, std::size_t table, std::uint32_t key) {
        for (auto offset = table; offset + 8 <= bytes.size(); offset += 8) {
            const auto candidate = word(bytes, offset);
            if (candidate == key) {
                return word(bytes, offset + 4);
            }
            if (candidate == missing) {
                break;
            }
        }
        throw std::runtime_error("Missing frontend string key");
    }

    std::map<std::uint32_t, std::vector<std::uint8_t>> members_;
};

class Menu {
public:
    void reset() { total_ = 0; width_ = 0; selected_ = -1; }

    void line(std::int32_t height, std::int32_t colour) {
        auto& row = next();
        row.value = height;
        row.colour = colour;
        row.kind = line_row;
        ++total_;
    }

    void add(const MenuResources& resources, const std::string& text, std::uint32_t language,
             std::int32_t value, std::int32_t kind, std::int32_t option = 0) {
        std::size_t start = 0;
        while (start < text.size()) {
            const auto separator = text.find('|', start);
            auto& row = next();
            row.text = text.substr(start, separator == std::string::npos ? separator : separator - start);
            row.value = value;
            row.kind = kind;
            row.option = option;
            width_ = std::max(width_, resources.width(row.text, language));
            ++total_;
            if (separator == std::string::npos) {
                break;
            }
            start = separator + 1;
        }
    }

    void select(std::int32_t value) {
        for (std::size_t i = 0; i < total_; ++i) {
            if (selectable(rows_[i]) && rows_[i].value == value) {
                selected_ = value;
                return;
            }
        }
    }

    void ensure_selection() {
        if (selected_ != -1) {
            return;
        }
        for (std::size_t i = 0; i < total_; ++i) {
            if (selectable(rows_[i])) {
                selected_ = rows_[i].value;
                return;
            }
        }
    }

    void move(std::int32_t direction) {
        std::vector<std::int32_t> values;
        for (std::size_t i = 0; i < total_; ++i) {
            if (selectable(rows_[i]) && (values.empty() || values.back() != rows_[i].value)) {
                values.push_back(rows_[i].value);
            }
        }
        const auto found = std::find(values.begin(), values.end(), selected_);
        if (found == values.end()) {
            return;
        }
        const auto count = static_cast<std::int32_t>(values.size());
        selected_ = values[(static_cast<std::int32_t>(found - values.begin()) + direction + count) % count];
    }

    std::vector<MenuRow> rows() const {
        std::vector<MenuRow> result(rows_.begin(), rows_.begin() + total_);
        for (std::size_t i = 0; i < result.size(); ++i) {
            // The original tests the retained successor even beyond the active menu.
            result[i].selection_bottom_border = i + 1 == rows_.size() ||
                rows_[i + 1].kind != selectable_row || rows_[i + 1].value != selected_;
        }
        return result;
    }
    std::int32_t width() const { return width_; }
    std::int32_t selection() const { return selected_; }

    void option(std::int32_t value, std::int32_t option) {
        for (std::size_t i = 0; i < total_; ++i) {
            if (rows_[i].value == value && (rows_[i].kind == slider_row || rows_[i].kind == detail_row)) { rows_[i].option = option; }
        }
    }

private:
    static bool selectable(const MenuRow& row) {
        return row.kind == selectable_row || row.kind == slider_row || row.kind == detail_row;
    }
    MenuRow& next() {
        if (total_ == rows_.size()) {
            throw std::runtime_error("Menu exceeds original storage");
        }
        return rows_[total_];
    }

    std::array<MenuRow, 40> rows_{};
    std::size_t total_ = 0;
    std::int32_t width_ = 0;
    std::int32_t selected_ = -1;
};

}

class Frontend::Impl {
public:
    Impl(const Archive& globals, const std::vector<std::uint8_t>& configuration)
        : profiles_(configuration), resources_(globals) {
        state_.language = profiles_.preferences().get(preference_language_offset);
        profiles_.controls(0); // GAME_Setup_ for the background DEMO.SLV precedes pilot selection.
        pilot_menu();
    }

    void press(std::uint32_t scan, KeyShift shift) {
        if (state_.phase == FrontendPhase::mission_request) {
            if (scan == escape || scan == pause_scan) {
                state_.mission_menu = true;
                if (scan == escape) { mission_abort_menu(); }
                else { mission_pause_menu(); }
            }
            if (scan == 0x4a) { state_.screen_size = std::max(0,state_.screen_size - viewport_shortcut_step); }
            if (scan == 0x4e) { state_.screen_size = std::min(slider_maximum,state_.screen_size + viewport_shortcut_step); }
            if (scan == 0x58) { state_.debug_counter = 1000000; }
            if (scan == f1) { state_.mission_menu = true; settings_menu(); }
            if (scan == 0x58 && state_.cheat_mode) { state_.mission_menu = true; cheat_page_ = 0; cheats_menu(); }
            if (scan == 0x57 && profiles_.cheats(state_.player)) { state_.mission_menu = true; grab_menu(); }
            return;
        }
        if (state_.phase == FrontendPhase::grab_name) { grab_name_key(scan,shift); return; }
        if (state_.phase == FrontendPhase::mission_pause) {
            if (scan != 0) { resume_mission(); }
            return;
        }
        if (state_.phase == FrontendPhase::key_capture) {
            if (scan == 0 || scan >= 256) { return; }
            if (resources_.text(key_labels_group,scan,state_.language).front() == '#' && !profiles_.cheats(state_.player)) { return; }
            const auto found = std::find(std::begin(binding_actions),std::end(binding_actions),binding_action_);
            profiles_.controls(state_.player).set(binding_offsets[found - std::begin(binding_actions)],scan);
            menu_sounds_.push_back(menu_action_sound);
            bindings_menu(static_cast<int>(binding_action_));
            return;
        }
        if (state_.phase == FrontendPhase::pilot_name) {
            name_key(scan, shift);
            return;
        }
        if (state_.phase == FrontendPhase::opening_movies || state_.phase == FrontendPhase::quit) { return; }
        if (state_.phase == FrontendPhase::credits) {
            if (scan != 0 && ++state_.credit_page == credit_page_count) { main_menu(); menu_.select(6); }
            return;
        }
        if (state_.phase == FrontendPhase::briefing) {
            if (scan == escape) {
                dispatch_sequence();
            }
            return;
        }
        if (scan == keypad_up) { scan = up; }
        if (scan == keypad_down) { scan = down; }
        if (scan == keypad_left) { scan = left; }
        if (scan == keypad_right) { scan = right; }
        if (scan == keypad_enter) { scan = enter; }
        if (scan == f1) { scan = escape; }
        if (scan == up || scan == down) {
            menu_sounds_.push_back(menu_move_sound);
            menu_.move(scan == up ? -1 : 1);
            return;
        }
        if (scan == left || scan == right) {
            adjust_slider(scan == left ? -1 : 1);
            return;
        }
        const bool ignore_cancel = state_.phase == FrontendPhase::pilot_selection || state_.phase == FrontendPhase::controller_type ||
            state_.phase == FrontendPhase::main_menu || state_.phase == FrontendPhase::mission_results;
        if (scan == enter || (scan == escape && !ignore_cancel)) {
            menu_polls_ = 0;
            menu_sounds_.push_back(menu_action_sound);
        }
        if (scan == escape && state_.phase == FrontendPhase::controller_bindings) {
            if (controller_return_ == FrontendPhase::settings) { settings_menu(10); }
            else { main_menu(); }
            return;
        }
        if (scan == escape) {
            switch (state_.phase) {
            case FrontendPhase::mission_grab: resume_mission(); return;
            case FrontendPhase::mission_abort: resume_mission(); return;
            case FrontendPhase::exit_confirmation: main_menu(); menu_.select(8); return;
            case FrontendPhase::pilot_deletion: pilot_menu(); menu_.select(1); return;
            case FrontendPhase::settings:
                if (state_.mission_menu) { state_.mission_menu = false; state_.phase = FrontendPhase::mission_request; return; }
                main_menu(); menu_.select(4); return;
            case FrontendPhase::axis_bindings: bindings_menu(20); return;
            case FrontendPhase::cheat_menu: state_.mission_menu = false; state_.phase = FrontendPhase::mission_request; return;
            case FrontendPhase::sound_volume: settings_menu(11); return;
            case FrontendPhase::language_selection: settings_menu(12); return;
            case FrontendPhase::gamma_selection: settings_menu(13); return;
            case FrontendPhase::music_selection: settings_menu(14); return;
            default: break;
            }
        }
        if (scan == escape && state_.phase == FrontendPhase::level_selection) {
            const int selection = test_levels_ ? 9 : 3;
            main_menu();
            menu_.select(selection);
            return;
        }
        if (scan != enter) {
            return;
        }
        if (state_.phase == FrontendPhase::mission_abort) {
            if (menu_.selection() == 1) { pending_abort_ = true; }
            else { resume_mission(); }
            return;
        }
        const auto selection = menu_.selection();
        switch (state_.phase) {
        case FrontendPhase::mission_grab:
            if (selection == 10) { state_.grab_name.clear(); grab_name_menu(); }
            if (selection == 11) { pending_grab_ = GrabAction::increment; }
            if (selection == 12) { state_.grab_statistics = !state_.grab_statistics; }
            return;
        case FrontendPhase::pilot_selection:
            if (selection == 0) {
                new_pilot();
            } else if (selection >= static_cast<std::int32_t>(pilot_action_base) && selection < static_cast<std::int32_t>(pilot_action_base) + static_cast<std::int32_t>(pilot_count)) {
                state_.player = selection - static_cast<std::int32_t>(pilot_action_base);
                selected_pilot();
            } else {
                deletion_menu();
            }
            return;
        case FrontendPhase::controller_type:
            if (profiles_.controls(state_.player).get(controller_type_offset) != static_cast<unsigned>(selection)) { controller_defaults(selection); }
            bindings_menu();
            return;
        case FrontendPhase::controller_bindings:
            if (selection == 1) { controller_menu(); return; }
            if (selection == 20) { axes_menu(); return; }
            binding_action_ = static_cast<std::uint32_t>(selection);
            key_menu();
            return;
        case FrontendPhase::pilot_deletion:
            if (selection >= static_cast<int>(pilot_action_base)) { profiles_.erase_pilot(selection - pilot_action_base); }
            pilot_menu(); menu_.select(1);
            return;
        case FrontendPhase::settings:
            switch (selection) {
            case 10: controller_return_ = FrontendPhase::settings; bindings_menu(); return;
            case 11: volume_menu(); return;
            case 12: language_menu(); return;
            case 13: gamma_menu(); return;
            case 14: music_menu(); return;
            case 15: state_.debug_controls = !state_.debug_controls; settings_menu(15); return;
            default: return;
            }
        case FrontendPhase::language_selection:
            state_.language = selection;
            profiles_.pilot(state_.player).set(pilot_language_offset,state_.language);
            settings_menu(12);
            return;
        case FrontendPhase::gamma_selection:
            state_.gamma = selection;
            profiles_.pilot(state_.player).set(pilot_gamma_offset,state_.gamma);
            gamma_menu();
            return;
        case FrontendPhase::music_selection:
            state_.music_type = selection;
            profiles_.pilot(state_.player).set(pilot_music_offset,state_.music_type);
            settings_menu(14);
            return;
        case FrontendPhase::sound_volume: return;
        case FrontendPhase::main_menu:
            if (selection == 5) { state_.phase = FrontendPhase::opening_movies; return; }
            if (selection == 6) { state_.credit_page = 0; state_.phase = FrontendPhase::credits; return; }
            if (selection == 8) { exit_menu(); return; }
            if (selection == 9) { test_levels_ = true; state_.cheat_mode = true; level_page_ = 0; level_menu(); return; }
            if (selection == 4) { settings_menu(); return; }
            if (selection == 7) { state_.player = -1; pilot_menu(); return; }
            if (selection == 3) {
                level_page_ = 0;
                level_menu();
                return;
            }
            if (selection != 1 && selection != 2) {
                throw std::runtime_error("This main-menu branch is not implemented");
            }
            state_.cheat_mode = false;
            start_sequence(selection);
            return;
        case FrontendPhase::level_selection:
            if (selection == previous_level_page || selection == next_level_page) {
                level_page_ = selection == previous_level_page ? std::max(0,level_page_ - level_page_step) :
                    std::min(level_count_ - 5,level_page_ + level_page_step);
                level_menu();
                if (test_levels_) { menu_.select(selection); menu_.ensure_selection(); }
            } else { state_.cheat_mode = test_levels_; start_sequence(2,static_cast<std::uint32_t>(selection)); }
            return;
        case FrontendPhase::exit_confirmation:
            if (selection == 1) { state_.phase = FrontendPhase::quit; }
            else { main_menu(); menu_.select(8); }
            return;
        case FrontendPhase::axis_bindings: {
            auto& record = profiles_.controls(state_.player);
            auto axes = record.buttons();
            if (selection >= 1 && selection <= 3) { axes[selection-1] = (axes[selection-1] + 1) % (axis_count(record) + 1); }
            if (selection == 4) { axes[3] ^= 1; }
            record.set_buttons(axes); axes_menu(selection); return;
        }
        case FrontendPhase::cheat_menu:
            if (selection == -9) { cheat_page_ = (cheat_page_ + 1) % 3; cheats_menu(); }
            else { pending_cheat_ = selection; }
            return;
        default:
            throw std::runtime_error("This frontend operation is not implemented");
        }
    }

    void presented() { ++menu_polls_; }

    bool accepts_queued_key(std::uint32_t scan) const {
        if (scan != enter && scan != keypad_enter) { return true; }
        switch (state_.phase) {
        case FrontendPhase::pilot_name: case FrontendPhase::grab_name: case FrontendPhase::key_capture:
        case FrontendPhase::mission_request: case FrontendPhase::mission_results: case FrontendPhase::mission_pause:
        case FrontendPhase::briefing: case FrontendPhase::opening_movies: case FrontendPhase::credits: case FrontendPhase::quit:
            return true;
        default:
            return menu_polls_ >= menu_confirmation_draws && menu_polls_ <= INT32_MAX;
        }
    }

    void devices(DeviceCapabilities capabilities) {
        if (capabilities.mouse == devices_.mouse && capabilities.joystick == devices_.joystick) { return; }
        devices_ = capabilities;
        if (state_.phase == FrontendPhase::controller_type) { controller_menu(); }
    }

    int take_cheat() { const int result = pending_cheat_; pending_cheat_ = -1; return result; }
    bool take_abort() { return std::exchange(pending_abort_,false); }
    GrabAction take_grab() { return std::exchange(pending_grab_,GrabAction::none); }
    std::vector<int> take_menu_sounds() { return std::exchange(menu_sounds_,{}); }

    void advance_clock(std::uint32_t updates) { clock_ += updates; }

    void finish_presentation() {
        if (state_.phase != FrontendPhase::opening_movies) { throw std::runtime_error("Opening movies are not active"); }
        main_menu();
        menu_.select(5);
    }

    void advance_landscape_clock(std::uint32_t updates) {
        state_.landscape_clock.rotor += updates * rotor_step;
        state_.landscape_clock.water += updates;
        state_.game_counter += updates;
    }

    void begin_results(const MissionResult& result) {
        if (state_.phase != FrontendPhase::mission_request) { throw std::runtime_error("No active mission for results"); }
        summary_ = result;
        summary_values_.fill(0);
        summary_values_[1] = static_cast<std::int32_t>(read_word(result.profile.data() + 40)) - result.level_score;
        summary_targets_ = {result.level_score, static_cast<std::int32_t>(read_word(result.profile.data() + 40)),
                            result.ratios[0], result.ratios[1], result.ratios[2], 0, 0, 0};
        account_time(result.elapsed_updates);
        state_.phase = FrontendPhase::mission_results;
        results_menu();
    }

    void advance_results(std::uint32_t updates) {
        if (state_.phase != FrontendPhase::mission_results) { throw std::runtime_error("Results are not active"); }
        for (std::uint32_t tick = 0; tick < updates; ++tick) {
            for (std::size_t i = 0; i < summary_values_.size(); ++i) {
                const auto delta = summary_targets_[i] - summary_values_[i];
                if (delta == 0) { continue; }
                summary_values_[i] += delta < 50 ? 1 : delta < 100 ? 3 : delta < 1000 ? 71 : delta < 10000 ? 12 : 673;
            }
        }
        results_menu();
    }

    std::vector<std::uint8_t> finish_mission(const MissionResult& result) {
        if (state_.phase != FrontendPhase::mission_request && state_.phase != FrontendPhase::mission_results && !state_.mission_menu) { throw std::runtime_error("No active campaign mission"); }
        if (state_.phase == FrontendPhase::mission_request || state_.mission_menu) { account_time(result.elapsed_updates); }
        state_.mission_menu = false;
        state_.landscape_clock = result.landscape_clock;
        state_.game_counter = result.game_counter;
        state_.objective_heading = result.objective_heading;
        state_.control_ticks = result.control_ticks;
        state_.effects_phase = result.effects_phase;
        state_.engine_pitch = result.engine_pitch;
        state_.weapon_progress = result.weapon_progress;
        state_.direction_steps = result.direction_steps;
        state_.flight_attitude = result.flight_attitude;
        state_.node_range = result.node_range;
        if (result.outcome == MissionOutcome::completed) { current_.set_payload(result.profile); }
        auto saved = profiles_.file();
        if (result.outcome == MissionOutcome::failed) {
            main_menu();
            return saved;
        }
        state_.sequence_offset += sequence_record_bytes;
        carry_result_ = true;
        dispatch_sequence();
        return saved;
    }

    FrontendState state() const {
        auto result = state_;
        result.cheats_unlocked = state_.player >= 0 && profiles_.cheats(state_.player);
        result.selection = menu_.selection();
        result.width = menu_.width();
        return result;
    }
    std::vector<MenuRow> rows() const { return menu_.rows(); }
    std::vector<std::uint8_t> configuration() const { return profiles_.snapshot(); }
    std::vector<std::uint8_t> configuration_file() const { return profiles_.file(); }

private:
    void grab_menu() {
        state_.phase = FrontendPhase::mission_grab;
        border_start(); text(12,1,-1,heading_row); separator(); line(3);
        text(12,10,10); text(12,11,11); text(12,12,12); slider(13,default_grab_zoom);
        border_end(); menu_.select(10);
    }

    void grab_name_menu() {
        state_.phase = FrontendPhase::grab_name;
        border_start(); line(3,prompt_colour); text(12,20,-1,heading_row);
        line(3,prompt_colour); separator(); line(3);
        add(substitute(resources_.text(0,52,state_.language),state_.grab_name),-1,disabled_row);
        border_end();
    }

    void grab_name_key(std::uint32_t scan, KeyShift shift) {
        if (scan == escape || (scan == enter && !state_.grab_name.empty())) {
            if (scan == enter) { pending_grab_ = GrabAction::save; }
            grab_menu();
            return;
        }
        if (scan < 128) {
            const auto keys = resources_.text(keyboard_group,static_cast<std::uint32_t>(shift),state_.language);
            if (scan >= keys.size()) { throw std::runtime_error("Truncated keyboard character table"); }
            if (keys[scan] != '_' && state_.grab_name.size() < grab_name_limit) { state_.grab_name.push_back(keys[scan]); }
            else if (scan == backspace && !state_.grab_name.empty()) { state_.grab_name.pop_back(); }
        }
        grab_name_menu();
    }

    void resume_mission() {
        state_.mission_menu = false;
        state_.phase = FrontendPhase::mission_request;
    }

    void mission_abort_menu() {
        state_.phase = FrontendPhase::mission_abort;
        border_start();
        text(13,1,-1,heading_row);
        separator(); line(3);
        text(13,10,1); text(13,11,2);
        border_end();
        menu_.select(2);
    }

    void mission_pause_menu() {
        state_.phase = FrontendPhase::mission_pause;
        menu_.reset();
        line(4,prompt_colour);
        text(4,1,1);
        line(4,prompt_colour);
        menu_.select(1);
    }

    void exit_menu() {
        state_.phase = FrontendPhase::exit_confirmation;
        border_start(); text(13,first_exit_prompt + clock_ % exit_prompt_count,-1,heading_row); separator(); line(3);
        text(13,10,1); text(13,11,2);
        border_end(); menu_.select(2);
    }

    void account_time(std::uint32_t updates) {
        constexpr std::size_t elapsed_time_offset = 0x2c;
        constexpr std::uint32_t updates_per_second = 40;
        auto& pilot = profiles_.pilot(state_.player);
        pilot.set(elapsed_time_offset, pilot.get(elapsed_time_offset) + updates / updates_per_second);
    }

    void results_menu() {
        menu_.reset();
        separator(); line(3, prompt_colour);
        if (summary_.outcome == MissionOutcome::completed) { text(200, state_.level, -1, heading_row); }
        else { text(7, 1, -1, heading_row); }
        line(3, prompt_colour); separator(); line(3, prompt_colour);
        if (summary_.outcome == MissionOutcome::completed) { text(7, 0, -1, heading_row); }
        else { text(200, state_.level, -1, heading_row); }
        line(3, prompt_colour); separator(); line(10);
        const auto statistic = [&](unsigned id, int value) {
            std::array<char, 128> buffer{};
            std::snprintf(buffer.data(), buffer.size(), resources_.text(7, id, state_.language).c_str(), value);
            add(buffer.data(), -1, disabled_row);
        };
        const auto duration = [&](unsigned id, std::uint32_t seconds) {
            std::array<char, 128> buffer{};
            std::snprintf(buffer.data(), buffer.size(), resources_.text(7, id, state_.language).c_str(),
                          seconds / 3600, (seconds / 60) % 60, seconds % 60);
            add(buffer.data(), -1, disabled_row);
        };
        statistic(10, summary_values_[0]);
        statistic(11, summary_values_[1]);
        duration(12, summary_.elapsed_updates / 40);
        duration(13, profiles_.pilot(state_.player).get(0x2c));
        statistic(14, summary_values_[2]);
        if (summary_.outcome == MissionOutcome::completed) {
            if (summary_targets_[3] != 0) { statistic(15, summary_values_[3]); }
            if (summary_targets_[4] != 0) { statistic(16, summary_values_[4]); }
        }
        line(12);
        text(4, 1, 99);
        menu_.ensure_selection();
    }
    void line(std::int32_t height, std::int32_t colour = background_colour) { menu_.line(height, colour); }
    void separator() { line(1, bright_colour); }
    void add(const std::string& text, std::int32_t value, std::int32_t kind = selectable_row) {
        menu_.add(resources_, text, state_.language, value, kind);
    }
    void text(std::uint32_t group, std::uint32_t id, std::int32_t value, std::int32_t kind = selectable_row) {
        add(resources_.text(group, id, state_.language), value, kind);
    }
    void border_start() { menu_polls_ = 0; menu_.reset(); line(3); separator(); }
    void border_end(std::int32_t height = 3) { line(height); separator(); line(3); }

    void pilot_menu() {
        state_.phase = FrontendPhase::pilot_selection;
        state_.language = profiles_.preferences().get(preference_language_offset);
        std::size_t used = 0;
        for (std::uint32_t player = 0; player < pilot_count; ++player) {
            used += !profiles_.pilot(player).name().empty();
        }
        border_start();
        text(0, 1, -1, heading_row);
        separator(); line(3);
        if (used < pilot_count) {
            text(0, 10, 0);
        }
        if (used > 0 && used < pilot_count) {
            line(3); separator(); line(3);
        }
        if (used > 0) {
            text(0, 11, 1);
        }
        line(3); separator(); line(3);
        for (std::uint32_t player = 0; player < pilot_count; ++player) {
            const auto name = profiles_.pilot(player).name();
            if (!name.empty()) {
                add(name, static_cast<std::int32_t>(pilot_action_base + player));
            }
        }
        for (auto i = used; i < 9; ++i) {
            add(". . . . .", -1, disabled_row);
        }
        border_end();
        menu_.select(static_cast<std::int32_t>(pilot_action_base) + state_.player);
        menu_.ensure_selection();
    }

    void new_pilot() {
        for (std::uint32_t player = 0; player < pilot_count; ++player) {
            auto& record = profiles_.pilot(player);
            if (!record.name().empty()) {
                continue;
            }
            state_.player = static_cast<std::int32_t>(player);
            state_.language = profiles_.preferences().get(preference_language_offset);
            record.set(pilot_language_offset, state_.language);
            record.set_name("");
            name_menu();
            return;
        }
        throw std::runtime_error("No free pilot slot");
    }

    void name_menu() {
        state_.phase = FrontendPhase::pilot_name;
        border_start();
        line(3, prompt_colour);
        text(0, 51, -1, heading_row);
        line(3, prompt_colour); separator(); line(3);
        add(substitute(resources_.text(0, 52, state_.language), profiles_.pilot(state_.player).name()), -1, disabled_row);
        border_end();
    }

    void name_key(std::uint32_t scan, KeyShift shift) {
        auto& record = profiles_.pilot(state_.player);
        auto name = record.name();
        if (scan == escape) {
            record.set_name(""); state_.player = -1; pilot_menu();
            return;
        }
        if (scan < 128) {
            const auto keys = resources_.text(keyboard_group, static_cast<std::uint32_t>(shift), state_.language);
            if (scan >= keys.size()) {
                throw std::runtime_error("Truncated keyboard character table");
            }
            if (keys[scan] != '_' && name.size() < name_limit) {
                name.push_back(keys[scan]);
            } else if (scan == backspace && !name.empty()) {
                name.pop_back();
            }
        }
        record.set_name(name);
        if (scan == enter && !name.empty()) {
            if (name == "SWIV!AndLetDie") {
                record.set(pilot_cheat_offset,1);
                record.set_name(""); name_menu(); return;
            }
            selected_pilot();
            return;
        }
        name_menu();
    }

    void selected_pilot() {
        state_.language = profiles_.pilot(state_.player).get(pilot_language_offset);
        state_.music_type = profiles_.pilot(state_.player).get(pilot_music_offset);
        state_.gamma = profiles_.pilot(state_.player).get(pilot_gamma_offset);
        controller_return_ = FrontendPhase::main_menu;
        auto& controls = profiles_.controls(state_.player);
        const auto type = controls.get(controller_type_offset);
        if (type == 0 || (type == 2 && !devices_.mouse) || (type >= 3 && (type > 4 || !devices_.joystick))) {
            controls.set(controller_type_offset, 0);
            controller_menu();
            return;
        }
        main_menu();
    }

    void controller_menu() {
        state_.phase = FrontendPhase::controller_type;
        border_start(); text(3, 101, -1, heading_row);
        separator(); line(3);
        text(3, 11, 1);
        if (devices_.mouse) { text(3,12,2); }
        if (devices_.joystick) { text(3,13,3); text(3,14,4); }
        border_end(2);
        menu_.select(1); menu_.ensure_selection();
    }

    void controller_defaults(int type) {
        auto& record = profiles_.controls(state_.player);
        std::uint32_t keys[]{left, right, up, down, comma, period, space,
                                       special_fire, page_up, page_down, control};
        std::array<std::uint8_t,4> axes{1,2,0,1};
        if (type > 1) { keys[4] = left; keys[5] = right; }
        if (type == 2) { keys[6] = 0x90; keys[7] = 0x91; axes[1] = 0; }
        if (type == 3 || type == 4) {
            keys[6] = 0x93; keys[7] = 0x94; keys[8] = type == 4 ? 0x96 : up; keys[9] = down; keys[10] = 0x95;
        }
        record.set(controller_type_offset, type);
        for (std::size_t i = 0; i < std::size(keys); ++i) {
            record.set(controller_keys_offset + i * sizeof(std::uint32_t), keys[i]);
        }
        record.set_buttons(axes);
    }

    void bindings_menu(std::int32_t selection = 1) {
        state_.phase = FrontendPhase::controller_bindings;
        const auto& record = profiles_.controls(state_.player);
        border_start();
        const auto type = record.get(controller_type_offset);
        const auto axes = record.buttons();
        add(substitute(resources_.text(3, 1, state_.language), resources_.text(3, 10+type, state_.language)), 1);
        line(3); separator();
        if (type > 1 && type != 6) { line(3); text(3,20,20); line(3); separator(); line(3); }
        for (std::size_t i = 0; i < std::size(binding_actions); ++i) {
            if (type > 1 && type != 6 && ((i < 2 && axes[0]) || (i >= 2 && i < 4 && axes[1]) || (i >= 4 && i < 6 && axes[2]))) { continue; }
            if (binding_actions[i] == 29) {
                separator();
            }
            add(substitute(resources_.text(3, binding_actions[i], state_.language),
                           resources_.text(key_labels_group, record.get(binding_offsets[i]), state_.language)),
                static_cast<std::int32_t>(binding_actions[i]));
        }
        line(4); separator(); line(4);
        menu_.select(selection); menu_.ensure_selection();
    }

    unsigned axis_count(const Record& record) const {
        const auto type = record.get(controller_type_offset);
        return type == 2 || type == 4 ? 2 : type == 3 || type == 5 ? 3 : 0;
    }

    void axes_menu(int selection = 1) {
        state_.phase = FrontendPhase::axis_bindings;
        auto& record = profiles_.controls(state_.player);
        auto axes = record.buttons();
        for (std::size_t i = 0; i < 3; ++i) { if (axes[i] > axis_count(record)) { axes[i] = 0; } }
        record.set_buttons(axes);
        border_start(); line(3); text(3,1001,-1,heading_row); line(3); separator(); line(3);
        for (unsigned i = 0; i < 3; ++i) {
            add(substitute(resources_.text(3,1010+i,state_.language),resources_.text(3,200+axes[i],state_.language)),i+1);
        }
        if (record.get(controller_type_offset) > 2) {
            add(substitute(resources_.text(3,1013,state_.language),resources_.text(0,100+axes[3],state_.language)),4);
        }
        border_end(); menu_.select(selection); menu_.ensure_selection();
    }

    void cheats_menu() {
        state_.phase = FrontendPhase::cheat_menu;
        border_start(); text(100,1001,-1,heading_row); line(3);
        text(100,9999,-9); line(3); separator(); line(3);
        if (cheat_page_ < 2) {
            for (unsigned i = cheat_page_ * 10; i < cheat_page_ * 10 + 10; ++i) { text(100,i,i); }
        } else {
            for (unsigned i : {100U,101U,102U,105U,106U,107U,108U,113U}) { text(100,i,i); }
            line(3); separator(); line(3);
            if (state_.craft == 0 || state_.craft == 1) { text(100,200,200); }
            if (state_.craft == 0 || state_.craft == 2 || state_.craft == 3) { text(100,201,201); }
            if (state_.craft == 1) { text(100,202,202); }
            if (state_.craft == 3) { text(100,203,203); }
        }
        border_end(); menu_.ensure_selection();
    }

    void deletion_menu() {
        state_.phase = FrontendPhase::pilot_deletion;
        border_start(); text(0,61,-1,heading_row); separator(); line(3);
        for (std::uint32_t player = 0; player < pilot_count; ++player) {
            const auto name = profiles_.pilot(player).name();
            if (!name.empty()) { add(name,static_cast<int>(pilot_action_base + player)); }
        }
        border_end(); menu_.ensure_selection();
    }

    void key_menu() {
        state_.phase = FrontendPhase::key_capture;
        menu_.reset(); separator(); line(3,prompt_colour);
        text(3,34,-1,heading_row);
        const auto found = std::find(std::begin(binding_actions),std::end(binding_actions),binding_action_);
        add(substitute(resources_.text(3,binding_action_,state_.language),
                       resources_.text(key_labels_group,profiles_.controls(state_.player).get(binding_offsets[found-std::begin(binding_actions)]),state_.language)),
            -1,heading_row);
        line(3,prompt_colour); separator();
    }

    void slider(int selection, int value, int kind = slider_row) {
        menu_.add(resources_," ",state_.language,selection,kind,value);
    }

    void settings_menu(int selection = 10) {
        state_.phase = FrontendPhase::settings;
        border_start(); line(3);
        for (unsigned id = 10; id <= 14; ++id) { text(2,id,id); }
        if (profiles_.pilot(state_.player).get(pilot_cheat_offset)) {
            line(3); separator(); line(3);
            add(substitute(resources_.text(2,15,state_.language),resources_.text(0,state_.debug_controls ? 100 : 101,state_.language)),15);
        }
        line(3); separator(); line(3);
        text(2,16,16,disabled_row); slider(17,state_.screen_size);
        line(3); line(3); separator(); line(3);
        text(2,17,16,disabled_row); slider(18,state_.detail,detail_row);
        border_end(); menu_.select(selection); menu_.ensure_selection();
    }

    void volume_menu() {
        state_.phase = FrontendPhase::sound_volume;
        border_start(); line(3); text(8,1,-1,heading_row); line(3); separator(); line(5);
        for (int index = 0; index < 3; ++index) {
            if (index != 0) { line(3); line(1,9); line(3); }
            text(8,10+index,-1,disabled_row);
            if (state_.volumes[index] < 0) { text(8,6,1,disabled_row); }
            else { slider(index+1,state_.volumes[index]); }
        }
        line(5); separator(); line(3); menu_.ensure_selection();
    }

    void language_menu() {
        state_.phase = FrontendPhase::language_selection;
        border_start(); line(3);
        const auto mask = profiles_.preferences().get(0x0c);
        for (unsigned language = 0; language < 15; ++language) {
            const auto label = resources_.text(0,language,all_language_page);
            if (label.front() == '#') { break; }
            if ((mask & (1U << language)) == 0) { add(label,language); }
        }
        border_end(); menu_.select(static_cast<int>(state_.language)); menu_.ensure_selection();
    }

    void gamma_menu() {
        state_.phase = FrontendPhase::gamma_selection;
        border_start(); line(3); text(6,1,-1,heading_row); line(3); separator(); line(3);
        for (unsigned gamma = 0; gamma < 4; ++gamma) { text(6,10+gamma,gamma); }
        border_end(); menu_.select(static_cast<int>(state_.gamma)); menu_.ensure_selection();
    }

    void music_menu() {
        state_.phase = FrontendPhase::music_selection;
        border_start(); text(9,1,-1,heading_row); separator(); line(3);
        for (unsigned music = 0; ; ++music) {
            if (resources_.text(9,10+music,state_.language).front() == '#') { break; }
            text(9,10+music,music);
        }
        border_end(); menu_.select(static_cast<int>(state_.music_type)); menu_.ensure_selection();
    }

    void adjust_slider(int direction) {
        const int selection = menu_.selection();
        int* value = nullptr;
        int step = continuous_slider_step, minimum = 0, maximum = slider_maximum;
        if (state_.phase == FrontendPhase::settings) {
            if (selection == 17) { value = &state_.screen_size; }
            if (selection == 18) { value = &state_.detail; step = detail_slider_step; minimum = detail_minimum; maximum = detail_maximum; }
        }
        if (state_.phase == FrontendPhase::sound_volume && selection >= 1 && selection <= 3 && state_.volumes[selection-1] >= 0) {
            value = &state_.volumes[selection-1];
        }
        if (!value) { return; }
        *value = std::clamp(*value + direction * step,minimum,maximum);
        menu_.option(selection,*value);
        if (state_.phase == FrontendPhase::sound_volume) { menu_polls_ = 0; }
        menu_sounds_.push_back(menu_slider_sound);
    }

    void main_menu() {
        test_levels_ = false;
        state_.cheat_mode = false;
        state_.tracks = {9,3,2,2,2,2};
        state_.phase = FrontendPhase::main_menu;
        const auto& record = profiles_.pilot(state_.player);
        border_start();
        add(substitute(resources_.text(1, 1, state_.language), record.name()), -1, heading_row);
        separator(); line(3);
        if (record.get(pilot_level_offset) == 0) {
            text(1, 10, 1);
        } else {
            if (record.get(pilot_level_offset) != completed_campaign) { text(1, 11, 2); }
            text(1, 12, 3);
        }
        text(1, 13, 4); text(1, 14, 5); text(1, 15, 6); text(1, 18, 7);
        line(2); separator(); line(2); text(1, 19, 8);
        if (record.get(pilot_cheat_offset) != 0) {
            text(1, 20, 9);
        }
        line(2); separator(); line(3);
        menu_.ensure_selection();
    }

    void level_menu() {
        state_.phase = FrontendPhase::level_selection;
        level_count_ = 0;
        while (resources_.text(200,level_count_,state_.language).front() != '#') { ++level_count_; }
        if (!test_levels_) { level_count_ = std::min(level_count_,static_cast<int>(profiles_.pilot(state_.player).get(pilot_level_offset)) + 1); }
        border_start(); text(11,test_levels_ ? 21 : 1,-1,heading_row); separator(); line(3);
        if (level_page_ == 0) { line(19); } else { text(11,10,previous_level_page); }
        line(3); separator(); line(3);
        for (int row = 0; row < level_page_rows; ++row) {
            const int level = level_page_ + row;
            if (level < level_count_) { text(200,level,level); } else { line(19); }
        }
        line(3); separator(); line(3);
        if (level_page_ < level_count_ - level_page_rows + 1) { text(11,11,next_level_page); } else { line(19); }
        line(3); separator(); line(2);
        menu_.ensure_selection();
    }

    void start_sequence(std::int32_t selection, std::uint32_t selected_level = missing) {
        carry_result_ = false;
        state_.sequence_offset = 0;
        current_ = Record{};
        if (selection == 2) {
            const auto level = selected_level == missing ? profiles_.pilot(state_.player).get(pilot_level_offset) : selected_level;
            const auto& sequence = resources_.member(sequence_resource);
            while (word(sequence, state_.sequence_offset) != 200 || word(sequence, state_.sequence_offset + 4) != level) {
                state_.sequence_offset += sequence_record_bytes;
            }
        }
        dispatch_sequence();
    }

    void dispatch_sequence() {
        const auto& sequence = resources_.member(sequence_resource);
        while (state_.sequence_offset + sequence_record_bytes <= sequence.size()) {
            const auto offset = state_.sequence_offset;
            const auto opcode = word(sequence, offset);
            const auto field = word(sequence, offset + 4);
            const auto value = word(sequence, offset + 8);
            state_.sequence_offset += sequence_record_bytes;
            if (opcode == 200) {
                state_.level = field;
                auto& pilot = profiles_.pilot(state_.player);
                if (state_.cheat_mode) { current_.reset(); current_.set(0x3c,400); continue; }
                pilot.set(pilot_level_offset, std::max(pilot.get(pilot_level_offset), field));
                auto& checkpoint = profiles_.checkpoint(state_.player, field);
                if (carry_result_) { checkpoint.copy_payload(current_); }
                current_.copy_payload(checkpoint);
            } else if (opcode == missing - 1) {
                profiles_.pilot(state_.player).set(pilot_level_offset,completed_campaign);
            } else if (opcode == missing) {
                main_menu();
                return;
            } else if (opcode >= 100 && opcode <= 103) {
                apply_sequence(opcode, field, value);
            } else if (opcode == 400) {
                for (std::size_t i = 0; i < state_.tracks.size(); ++i) {
                    const auto source = offset + 4 + i * 2;
                    state_.tracks[i] = sequence.at(source) | static_cast<std::uint32_t>(sequence.at(source + 1)) << 8;
                }
            } else if (opcode == 500) {
                state_.movie = string_at(sequence, offset + 4);
                state_.phase = FrontendPhase::briefing;
                return;
            } else if (opcode == 300) {
                state_.level_file = string_at(sequence, offset + 4) + ".slv";
                state_.mission_profile = current_.payload();
                state_.phase = FrontendPhase::mission_request;
                state_.sequence_offset = offset;
                return;
            } else {
                throw std::runtime_error("Unsupported campaign sequence opcode");
            }
        }
        throw std::runtime_error("Campaign sequence ended before a mission request");
    }

    void apply_sequence(std::uint32_t opcode, std::uint32_t field, std::uint32_t value) {
        auto apply = [opcode, value](std::uint32_t old) {
            const auto signed_old = signed_word(old);
            const auto signed_value = signed_word(value);
            switch (opcode) {
            case 100: return value;
            case 101: return signed_old < signed_value ? value : old;
            case 102: return signed_old > signed_value ? value : old;
            case 103: return old + value;
            default: throw std::runtime_error("Unsupported sequence assignment");
            }
        };
        if (field == 3) {
            state_.water = apply(state_.water);
            return;
        }
        if (field == 4) {
            state_.craft = apply(state_.craft);
            return;
        }
        if (field < 3) {
            constexpr std::size_t offsets[]{0x38, 0x3c, 0x34};
            current_.set(offsets[field], apply(current_.get(offsets[field])));
            return;
        }
        const auto offset = static_cast<std::int64_t>(field) * 2 - sequence_short_bias;
        if (offset < 0 || offset > static_cast<std::int64_t>(record_bytes - 2)) {
            throw std::runtime_error("Sequence field exceeds current profile");
        }
        current_.set_short(static_cast<std::size_t>(offset),
                           apply(static_cast<std::uint32_t>(current_.short_value(static_cast<std::size_t>(offset)))));
    }

    Profiles profiles_;
    MenuResources resources_;
    Menu menu_;
    DeviceCapabilities devices_;
    bool test_levels_ = false;
    unsigned cheat_page_ = 0;
    int pending_cheat_ = -1;
    bool pending_abort_ = false;
    GrabAction pending_grab_ = GrabAction::none;
    std::vector<int> menu_sounds_;
    std::uint32_t menu_polls_ = 0;
    std::uint32_t clock_ = 0;
    FrontendState state_;
    Record current_;
    bool carry_result_ = false;
    FrontendPhase controller_return_ = FrontendPhase::main_menu;
    std::uint32_t binding_action_ = 0;
    int level_page_ = 0, level_count_ = 0;
    MissionResult summary_{MissionOutcome::failed};
    std::array<std::int32_t, 8> summary_values_{}, summary_targets_{};
};

Frontend::Frontend(const Archive& globals, const std::vector<std::uint8_t>& configuration)
    : impl_(std::make_unique<Impl>(globals, configuration)) {}
Frontend::~Frontend() = default;
void Frontend::press(std::uint32_t scan, KeyShift shift) { impl_->press(scan, shift); }
void Frontend::presented() { impl_->presented(); }
bool Frontend::accepts_queued_key(std::uint32_t scan) const { return impl_->accepts_queued_key(scan); }
void Frontend::devices(DeviceCapabilities capabilities) { impl_->devices(capabilities); }
int Frontend::take_cheat() { return impl_->take_cheat(); }
bool Frontend::take_abort() { return impl_->take_abort(); }
GrabAction Frontend::take_grab() { return impl_->take_grab(); }
std::vector<int> Frontend::take_menu_sounds() { return impl_->take_menu_sounds(); }
void Frontend::advance_landscape_clock(std::uint32_t updates) { impl_->advance_landscape_clock(updates); }
void Frontend::begin_results(const MissionResult& result) { impl_->begin_results(result); }
void Frontend::advance_results(std::uint32_t updates) { impl_->advance_results(updates); }
std::vector<std::uint8_t> Frontend::finish_mission(const MissionResult& result) { return impl_->finish_mission(result); }
FrontendState Frontend::state() const { return impl_->state(); }
std::vector<MenuRow> Frontend::rows() const { return impl_->rows(); }
std::vector<std::uint8_t> Frontend::configuration() const { return impl_->configuration(); }
std::vector<std::uint8_t> Frontend::configuration_file() const { return impl_->configuration_file(); }
void Frontend::advance_clock(std::uint32_t updates) { impl_->advance_clock(updates); }
void Frontend::finish_presentation() { impl_->finish_presentation(); }

const char* phase_name(FrontendPhase phase) {
    switch (phase) {
    case FrontendPhase::pilot_selection: return "pilot-selection";
    case FrontendPhase::pilot_name: return "pilot-name";
    case FrontendPhase::controller_type: return "controller-type";
    case FrontendPhase::axis_bindings: return "axis-bindings";
    case FrontendPhase::cheat_menu: return "cheat-menu";
    case FrontendPhase::controller_bindings: return "controller-bindings";
    case FrontendPhase::main_menu: return "main-menu";
    case FrontendPhase::briefing: return "briefing";
    case FrontendPhase::mission_request: return "mission-request";
    case FrontendPhase::mission_results: return "mission-results";
    case FrontendPhase::level_selection: return "level-selection";
    case FrontendPhase::settings: return "settings";
    case FrontendPhase::pilot_deletion: return "pilot-deletion";
    case FrontendPhase::key_capture: return "key-capture";
    case FrontendPhase::sound_volume: return "volume";
    case FrontendPhase::language_selection: return "language";
    case FrontendPhase::gamma_selection: return "gamma";
    case FrontendPhase::music_selection: return "music";
    case FrontendPhase::opening_movies: return "opening-movies";
    case FrontendPhase::credits: return "credits";
    case FrontendPhase::exit_confirmation: return "exit-confirmation";
    case FrontendPhase::quit: return "quit";
    case FrontendPhase::mission_pause: return "mission-pause";
    case FrontendPhase::mission_abort: return "mission-abort";
    case FrontendPhase::mission_grab: return "mission-grab";
    case FrontendPhase::grab_name: return "grab-name";
    }
    throw std::runtime_error("Unknown frontend phase");
}

}
