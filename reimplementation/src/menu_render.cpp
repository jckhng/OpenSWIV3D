#include "menu_render.hpp"
#include "alpha.hpp"
#include "combat_math.hpp"

#include <stdexcept>
#include <cmath>

namespace swiv3d::detail {

void apply_gamma(MissionFrame& frame, unsigned gamma) {
    if (gamma == 0) { return; }
    if (gamma > 3) { throw std::runtime_error("Unsupported gamma level"); }
    constexpr double reciprocal_dac_maximum = 0.015873015873015876;
    for (auto& component : frame.palette) {
        component = static_cast<std::uint8_t>(std::pow(component * reciprocal_dac_maximum,1.0 - gamma * 0.125) * 63.0);
    }
}

void draw_menu(MissionFrame& frame, const Archive& globals, const std::vector<std::uint8_t>& alpha,
               const Frontend& frontend) {
    constexpr int center_x = 160, text_row_height = 19, margin = 5;
    const auto state = frontend.state();
    const int top_y = state.phase == FrontendPhase::mission_pause ? 260 :
                      state.phase == FrontendPhase::mission_grab ? 270 :
                      state.phase == FrontendPhase::mission_results ? 120 :
                      state.phase == FrontendPhase::pilot_name || state.phase == FrontendPhase::grab_name || state.phase == FrontendPhase::settings ||
                      state.phase == FrontendPhase::key_capture || state.phase == FrontendPhase::sound_volume ||
                      state.phase == FrontendPhase::gamma_selection || state.phase == FrontendPhase::music_selection ||
                      state.phase == FrontendPhase::exit_confirmation || state.phase == FrontendPhase::mission_abort ? 160 :
                      state.phase == FrontendPhase::language_selection ? 140 :
                      state.phase == FrontendPhase::controller_bindings ? 100 :
                      state.phase == FrontendPhase::axis_bindings ? 170 : 110;
    const auto rows = frontend.rows();
    const int half_width = state.width / 2 + margin;
    const int left = center_x - half_width, right = center_x + half_width;
    AlphaPresenter presenter(frame.pixels, alpha);
    const auto box = [&](int x1, int y1, int x2, int y2, std::uint8_t colour) {
        for (int y = y1; y < y2; ++y) {
            for (int x = x1; x <= x2; ++x) { presenter.blend(x, y, colour); }
        }
    };
    int y = top_y;
    for (std::size_t i = 0; i < rows.size(); ++i) {
        const auto& row = rows[i];
        if (row.kind == 3) {
            box(left, y, right, y + row.value, static_cast<std::uint8_t>(row.colour));
            y += row.value;
            continue;
        }
        if (row.kind < 0 || row.kind > 5) { throw std::runtime_error("Unsupported native menu row"); }
        const bool selected = (row.kind == 0 || row.kind == 4 || row.kind == 5) && row.value == state.selection;
        box(left, y, right, y + text_row_height, row.kind == 1 ? 0x76 : selected ? 3 : 0xa1);
        if (selected) {
            box(left, y, left, y + text_row_height, 10);
            box(right, y, right, y + text_row_height, 10);
            if (i == 0 || rows[i - 1].kind != 0 || rows[i - 1].value != state.selection) { box(left, y, right, y + 1, 10); }
            if (row.selection_bottom_border) { box(left, y + 18, right, y + 19, 10); }
        }
        y += text_row_height;
    }
    const auto font = globals.load(507), widths = globals.load(508), strings = globals.load(100);
    const auto lookup = [&](std::size_t table, std::uint32_t id) {
        for (auto offset = table; offset + 8 <= strings.size(); offset += 8) {
            const auto key = static_cast<std::uint32_t>(combat_math::word(strings, offset));
            if (key == id) { return static_cast<std::size_t>(combat_math::word(strings, offset + 4)); }
            if (key == UINT32_MAX) { break; }
        }
        throw std::runtime_error("Missing menu character page");
    };
    const auto page = lookup(lookup(0, 0x630002), 0);
    const auto glyph = [&](unsigned char character) {
        return character == 127 ? 0U : character < 128 ? static_cast<unsigned>(character) :
               static_cast<unsigned>(character + (strings.at(page + state.language) - '0') * 128);
    };
    y = top_y;
    for (const auto& row : rows) {
        if (row.kind == 3) { y += row.value; continue; }
        int width = 0;
        for (unsigned char character : row.text) { width += combat_math::word(widths, glyph(character) * 4); }
        int x = center_x - width / 2;
        for (unsigned char character : row.text) {
            const auto index = glyph(character);
            presenter.draw(font, x, y, index);
            x += combat_math::word(widths, index * 4);
        }
        if (row.kind == 4 || row.kind == 5) {
            constexpr int track_left = center_x - 39, track_right = center_x + 41;
            box(track_left,y+11,track_right,y+13,0);
            box(center_x-40,y+9,center_x+40,y+11,255);
            const int knob = center_x + row.option / 4 - 32;
            box(knob,y+5,knob+3,y+18,0);
            box(knob-1,y+3,knob+2,y+16,9);
            box(knob,y+5,knob+1,y+14,0);
        }
        y += text_row_height;
    }
}

}
