#include <windows.h>

#include "../src/window.hpp"
#include "../src/input_script.hpp"

#include <algorithm>
#include <deque>
#include <fstream>
#include <iostream>
#include <stdexcept>

namespace {

void require(bool value, const char* message) {
    if (!value) {
        throw std::runtime_error(message);
    }
}

struct WindowApi {
    WNDPROC procedure = nullptr;
    LONG_PTR user = 0;
    bool live = false;
    bool foreground = true;
    bool invalidated = false;
    RECT client{0, 0, 960, 720};
    RECT destination{};
    BITMAPINFOHEADER bitmap{};
    std::deque<MSG> queue;
    std::vector<std::uint32_t> pixels;
    std::size_t paints = 0;
    std::size_t key_messages = 0;
    std::size_t visible_clears = 0;
    std::size_t visible_copies = 0, visible_draws = 0;
    std::size_t surfaces_created = 0, surfaces_deleted = 0, bitmaps_created = 0, bitmaps_deleted = 0;
    std::size_t selected_bitmaps = 0;
    bool allocation_failure = false;
} api;

HWND handle() { return &api; }

void key(UINT message, WPARAM virtual_key, unsigned scan, bool repeat = false) {
    std::uint32_t packed = 1 | ((scan & 0x7f) << 16) | ((scan & 0x80) != 0 ? 1U << 24 : 0);
    if (repeat || message == WM_KEYUP || message == WM_SYSKEYUP) {
        packed |= 1U << 30;
    }
    if (message == WM_KEYUP || message == WM_SYSKEYUP) {
        packed |= 1U << 31;
    }
    api.queue.push_back({handle(), message, virtual_key, static_cast<LPARAM>(packed)});
    ++api.key_messages;
}

WPARAM virtual_key(unsigned scan) {
    switch (scan) {
    case 0xc8: return VK_UP;
    case 0xd0: return VK_DOWN;
    case 0xcb: return VK_LEFT;
    case 0xcd: return VK_RIGHT;
    case 0x1d: return VK_CONTROL;
    case 0x33: return VK_OEM_COMMA;
    case 0x34: return VK_OEM_PERIOD;
    default: return 0;
    }
}

void held(swiv3d::host::Window& window, const swiv3d::KeyboardState& keys) {
    const auto before = window.keys();
    for (unsigned scan = 0; scan < keys.size(); ++scan) {
        if (before[scan] != keys[scan]) {
            key(keys[scan] ? WM_KEYDOWN : WM_KEYUP, virtual_key(scan), scan);
        }
    }
    require(window.poll(), "Window closed while delivering flight input");
    require(window.keys() == keys, "Window message path changed the held-key input");
}

void verify_paint(const swiv3d::MissionFrame& frame) {
    constexpr std::array<unsigned,64> dac_levels{{
        0,4,8,12,16,20,24,28,32,36,40,44,48,52,56,60,
        65,69,73,77,81,85,89,93,97,101,105,109,113,117,121,125,
        130,134,138,142,146,150,154,158,162,166,170,174,178,182,186,190,
        195,199,203,207,211,215,219,223,227,231,235,239,243,247,251,255
    }};
    require(api.bitmap.biWidth == 320 && api.bitmap.biHeight == -400 && api.bitmap.biBitCount == 32,
            "Window presenter changed the top-down framebuffer format");
    require(api.pixels.size() == frame.pixels.size(), "Window presenter dropped framebuffer pixels");
    for (std::size_t i = 0; i < frame.pixels.size(); ++i) {
        const auto index = frame.pixels[i] * 3;
        const auto expected = dac_levels.at(frame.palette[index]) * 65536 +
                              dac_levels.at(frame.palette[index + 1]) * 256 + dac_levels.at(frame.palette[index + 2]);
        require(api.pixels[i] == expected, "Window presenter changed indexed palette colours or pixel order");
    }
}

void test_keys_and_focus() {
    api = {};
    swiv3d::host::Window window;
    require(window.focused(), "Foreground window not recognized");
    api.foreground = false;
    require(!window.focused(), "Background window reported focused");
    api.foreground = true;
    key(WM_KEYDOWN, VK_UP, 0xc8);
    key(WM_KEYDOWN, VK_UP, 0xc8, true);
    key(WM_KEYDOWN, VK_CONTROL, 0x1d);
    require(window.poll(), "Key press closed window");
    require(window.keys()[0xc8] && !window.keys()[0x48] && window.keys()[0x1d],
            "Extended arrow or Ctrl scan mapping changed");
    key(WM_KEYUP, VK_UP, 0xc8);
    key(WM_SYSKEYDOWN, VK_OEM_COMMA, 0x33);
    require(window.poll() && !window.keys()[0xc8] && window.keys()[0x33] && window.keys()[0x1d],
            "Key release cleared the wrong held controls");
    key(WM_SYSKEYUP, VK_OEM_COMMA, 0x33);
    key(WM_KEYDOWN, VK_UP, 0x48);
    require(window.poll() && window.keys()[0x48] && !window.keys()[0xc8] && !window.keys()[0x33],
            "Keypad and extended arrows were conflated");
    api.queue.push_back({handle(), WM_KILLFOCUS, 0, 0});
    require(window.poll(), "Focus loss closed the window");
    const auto released = window.keys();
    require(std::none_of(released.begin(), released.end(), [](bool down) { return down; }),
            "Focus loss left held keys active");
    window.escape_action(swiv3d::host::EscapeAction::forward);
    key(WM_KEYDOWN, VK_ESCAPE, 0x01);
    require(window.poll() && window.keys()[0x01], "Cutscene Escape closed the window");
    key(WM_KEYUP, VK_ESCAPE, 0x01);
    require(window.poll() && !window.keys()[0x01], "Cutscene Escape release was lost");
    key(WM_KEYDOWN, VK_PAUSE, 0x45);
    require(window.poll() && window.keys()[swiv3d::pause_scan] && !window.keys()[0x45],
            "Windows Pause did not reach the original logical pause scan");
    key(WM_KEYUP, VK_PAUSE, 0x45);
    require(window.poll() && !window.keys()[swiv3d::pause_scan], "Windows Pause release was lost");
    window.escape_action(swiv3d::host::EscapeAction::quit);
    key(WM_KEYDOWN, VK_ESCAPE, 0x01);
    require(!window.poll() && !api.live, "Escape did not terminate the window loop");
}

void test_presentation() {
    api = {};
    swiv3d::host::Window window;
    swiv3d::MissionFrame frame;
    frame.pixels.resize(swiv3d::image_width * swiv3d::image_height);
    for (std::size_t i = 0; i < 256; ++i) {
        frame.palette[i * 3] = static_cast<std::uint8_t>(i % 64);
        frame.palette[i * 3 + 1] = static_cast<std::uint8_t>((i * 2) % 64);
        frame.palette[i * 3 + 2] = static_cast<std::uint8_t>((i * 3) % 64);
    }
    for (std::size_t i = 0; i < frame.pixels.size(); ++i) {
        frame.pixels[i] = static_cast<std::uint8_t>(i);
    }
    window.present(frame);
    verify_paint(frame);
    require(api.visible_clears == 0,"Window cleared the visible frame before presenting its replacement");
    require(api.visible_copies == 1 && api.visible_draws == 0,"Window did not present one completed offscreen frame");
    require(api.procedure(handle(),WM_ERASEBKGND,0,0) == 1,"Window allowed a background erase between frames");
    require(api.destination.left == 0 && api.destination.top == 0 && api.destination.right == 960 && api.destination.bottom == 720,
            "Default window did not preserve 4:3 presentation");
    window.present(frame);
    require(api.surfaces_created == 1 && api.bitmaps_created == 1,"Window recreated the paint surface without a resize");
    api.client = {0, 0, 1200, 720};
    window.present(frame);
    require(api.destination.left == 120 && api.destination.top == 0 && api.destination.right == 1080 && api.destination.bottom == 720,
            "Wide window lost horizontal letterboxing");
    api.client = {0, 0, 640, 800};
    window.present(frame);
    require(api.destination.left == 0 && api.destination.top == 160 && api.destination.right == 640 && api.destination.bottom == 640,
            "Tall window lost vertical letterboxing");
    require(api.visible_copies == 4 && api.visible_clears == 0 && api.visible_draws == 0 && api.selected_bitmaps == 1,
            "Resize exposed a clear or leaked a selected paint bitmap");
    const auto copies = api.visible_copies;
    api.client = {0,0,0,0};
    window.present(frame);
    require(api.visible_copies == copies,"Minimized window attempted a zero-size presentation");
    api.client = {0,0,800,600}; api.allocation_failure = true;
    window.present(frame);
    require(api.visible_copies == copies && api.visible_clears == 0 && api.selected_bitmaps == 1,
            "Paint allocation failure cleared the visible frame or discarded the retained surface");
    api.allocation_failure = false;
}

void checkpoint(const swiv3d::Mission& mission, const swiv3d::MissionFrame& frame, const std::filesystem::path& path) {
    std::filesystem::create_directory(path);
    const auto state = mission.state();
    std::ofstream out(path / "state.json");
    out << "{\"tick\":" << state.tick << ",\"landscape_clock\":{\"rotor\":" << state.landscape_clock.rotor
        << ",\"water\":" << state.landscape_clock.water << "},\"control_ticks\":" << state.flight.control_ticks
        << ",\"previous_heading\":" << state.flight.previous_heading << ",\"engine_pitch\":" << state.flight.engine_pitch;
    const auto array = [&](const char* name, const auto& values) {
        out << ",\"" << name << "\":[";
        const char* separator = "";
        for (auto value : values) {
            out << separator << value;
            separator = ",";
        }
        out << ']';
    };
    array("accumulators", state.flight.attitude);
    array("bindings", state.flight.bindings);
    const auto player = std::find_if(state.entities.begin(), state.entities.end(), [&](const auto& entity) { return entity.slot == state.player; });
    require(player != state.entities.end(), "Flight lost its player");
    array("player", player->words);
    out << "}\n";
    std::ofstream pixels(path / "frame-indexed.bin", std::ios::binary);
    pixels.write(reinterpret_cast<const char*>(frame.pixels.data()), static_cast<std::streamsize>(frame.pixels.size()));
    require(out.good() && pixels.good(), "Cannot write window flight checkpoint");
}

void replay(const char* game, const char* startup, const char* movie, const std::filesystem::path& output) {
    require(!std::filesystem::exists(output), "Window flight output must be fresh");
    auto session = swiv3d::detail::start_mission(game, startup);
    std::ifstream input(movie);
    const auto segments = swiv3d::detail::read_flight(input);
    std::filesystem::create_directories(output);
    api = {};
    swiv3d::host::Window window;
    auto frame = session.mission->render(session.globals);
    window.present(frame);
    verify_paint(frame);
    checkpoint(*session.mission, frame, output / "initial");
    std::size_t ordinal = 0;
    for (const auto& segment : segments) {
        held(window, segment.keys);
        for (std::uint32_t i = 0; i < segment.updates; ++i) {
            session.mission->advance_flight(window.keys());
            frame = session.mission->render(session.globals);
            window.present(frame);
            verify_paint(frame);
        }
        checkpoint(*session.mission, frame, output / ("step-" + std::to_string(ordinal++)));
    }
    std::ofstream report(output / "window-tests.json");
    report << "{\"input\":\"simulated Win32 messages; production window procedure\","
           << "\"windows_api\":\"in-process mock; no desktop input or capture\","
           << "\"key_focus_quit_tests\":true,\"palette_letterbox_tests\":true,"
           << "\"visible_clears\":" << api.visible_clears << ",\"completed_frame_copies\":" << api.visible_copies << ','
           << "\"key_messages\":" << api.key_messages << ",\"paints\":" << api.paints
           << ",\"ticks\":" << session.mission->state().tick << "}\n";
    require(report.good(), "Cannot write window test receipt");
}

}

HINSTANCE GetModuleHandleW(const wchar_t*) { return handle(); }
HCURSOR LoadCursor(HINSTANCE, const char*) { return handle(); }
unsigned short RegisterClassW(const WNDCLASSW* type) { api.procedure = type->lpfnWndProc; return 1; }
BOOL AdjustWindowRect(RECT*, DWORD, BOOL) { return TRUE; }
HWND CreateWindowW(const wchar_t*, const wchar_t*, DWORD, int, int, int width, int height, HWND, void*, HINSTANCE, void* parameter) {
    api.live = true;
    api.client = {0, 0, width, height};
    CREATESTRUCTW creation{parameter};
    api.procedure(handle(), WM_NCCREATE, 0, reinterpret_cast<LPARAM>(&creation));
    return handle();
}
LONG_PTR SetWindowLongPtrW(HWND, int, LONG_PTR value) { api.user = value; return 0; }
LONG_PTR GetWindowLongPtrW(HWND, int) { return api.user; }
LRESULT DefWindowProcW(HWND, UINT message, WPARAM, LPARAM) { return message == WM_NCCREATE ? TRUE : 0; }
BOOL ShowWindow(HWND, int) { return TRUE; }
BOOL IsWindow(HWND) { return api.live; }
BOOL DestroyWindow(HWND window) { api.live = false; api.procedure(window, WM_DESTROY, 0, 0); return TRUE; }
BOOL PeekMessageW(MSG* message, HWND, UINT, UINT, UINT) {
    if (api.queue.empty()) {
        return FALSE;
    }
    *message = api.queue.front();
    api.queue.pop_front();
    return TRUE;
}
BOOL TranslateMessage(const MSG*) { return TRUE; }
LRESULT DispatchMessageW(const MSG* message) { return api.procedure(message->hwnd, message->message, message->wParam, message->lParam); }
HWND GetForegroundWindow() { return api.foreground ? handle() : nullptr; }
BOOL InvalidateRect(HWND, const RECT*, BOOL) { api.invalidated = true; return TRUE; }
BOOL UpdateWindow(HWND window) {
    if (api.invalidated) {
        api.invalidated = false;
        api.procedure(window, WM_PAINT, 0, 0);
    }
    return TRUE;
}
HDC BeginPaint(HWND, PAINTSTRUCT*) { return handle(); }
BOOL EndPaint(HWND, const PAINTSTRUCT*) { return TRUE; }
BOOL GetClientRect(HWND, RECT* area) { *area = api.client; return TRUE; }
HGDIOBJ GetStockObject(int) { return handle(); }
int FillRect(HDC dc, const RECT*, HBRUSH) { if (dc == handle()) { ++api.visible_clears; } return 1; }
HDC CreateCompatibleDC(HDC) { ++api.surfaces_created; return new int(0); }
HBITMAP CreateCompatibleBitmap(HDC, int, int) {
    if (api.allocation_failure) { return nullptr; }
    ++api.bitmaps_created;
    return new int(1);
}
HGDIOBJ SelectObject(HDC dc, HGDIOBJ bitmap) {
    if (bitmap == handle()) { --api.selected_bitmaps; return dc; }
    ++api.selected_bitmaps;
    return handle();
}
BOOL DeleteObject(HGDIOBJ bitmap) { ++api.bitmaps_deleted; delete static_cast<int*>(bitmap); return TRUE; }
BOOL DeleteDC(HDC dc) { ++api.surfaces_deleted; delete static_cast<int*>(dc); return TRUE; }
BOOL BitBlt(HDC dc, int, int, int, int, HDC source, int, int, DWORD) {
    require(dc == handle() && source != handle(),"Completed frame copy did not use the offscreen surface");
    ++api.visible_copies;
    return TRUE;
}
int SetStretchBltMode(HDC, int) { return 1; }
int StretchDIBits(HDC dc, int x, int y, int width, int height, int, int, int, int, const void* pixels,
                  const BITMAPINFO* info, UINT, DWORD) {
    api.destination = {x, y, x + width, y + height};
    api.bitmap = info->bmiHeader;
    if (dc == handle()) { ++api.visible_draws; }
    const auto* data = static_cast<const std::uint32_t*>(pixels);
    api.pixels.assign(data, data + info->bmiHeader.biWidth * -info->bmiHeader.biHeight);
    ++api.paints;
    return height;
}
void PostQuitMessage(int) { api.queue.push_back({handle(), WM_QUIT, 0, 0}); }

int main(int argc, char** argv) {
    try {
        test_keys_and_focus();
        test_presentation();
        require(api.surfaces_created == api.surfaces_deleted && api.bitmaps_created == api.bitmaps_deleted && api.selected_bitmaps == 0,
                "Window did not release its offscreen GDI resources");
        if (argc == 5) {
            replay(argv[1], argv[2], argv[3], argv[4]);
        } else {
            require(argc == 1, "Expected GAME_DIRECTORY FRONTEND_SCRIPT FLIGHT_SCRIPT OUTPUT_DIRECTORY");
        }
        std::cout << "Production window handler: keys, focus, Escape, palette, letterboxing, offscreen presentation and GDI cleanup passed; Win32 APIs mocked\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
