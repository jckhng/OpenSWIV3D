#include "../src/flight.hpp"

#include <iostream>
#include <stdexcept>

namespace {

void require(bool value, const char* message) {
    if (!value) {
        throw std::runtime_error(message);
    }
}

struct Fixture {
    swiv3d::EntityState player;
    swiv3d::FlightState flight;
    swiv3d::KeyboardState keys{};

    Fixture() {
        player.words[2] = 496;
        player.words[5] = 10;
        player.words[8] = 180 * 256;
        player.words[13] = 248;
        flight.attitude[0] = 248 * 256;
        flight.bindings = {0xcb, 0xcd, 0xc8, 0xd0, 0x33, 0x34, 0x39, 0x32, 0xc9, 0xd1, 0x1d};
    }

    void step() {
        swiv3d::detail::advance_helicopter(player, flight, keys, [](int, int) { return 100 * 256; });
    }
};

void test_control_order_and_drag() {
    Fixture f;
    f.keys[0xc8] = true;
    f.step();
    require(f.player.words[14] == -5 && f.player.words[12] == 0, "Input affected velocity before the next body update");
    f.step();
    require(f.player.words[9] == 0 && f.player.words[12] == -21 && f.player.words[10] == 0,
            "Body integration order or negative shift/low-speed drag changed");
    f.step();
    require(f.player.words[9] == 0x7fffff - 20, "Negative world position did not wrap to 23 bits");
    require(f.flight.control_ticks == 3 && f.player.words[6] == 3 && f.player.words[5] == 7,
            "Body/control timers lost a logical update");
}

void test_opposed_and_slide_controls() {
    Fixture f;
    for (auto scan : {0xcb, 0xcd, 0xc8, 0xd0, 0x33, 0x34}) {
        f.keys[scan] = true;
    }
    f.step();
    require(f.player.words[13] == 248 && f.player.words[14] == 0 && f.player.words[15] == 0,
            "Opposed keys did not cancel");
    f.keys.fill(false);
    f.keys[0xcb] = true;
    f.keys[0x1d] = true;
    f.step();
    require(f.player.words[13] == 248 && f.player.words[15] == -7,
            "Ctrl-slide changed yaw or used the turning bank");
    f.keys.fill(false);
    for (int i = 0; i < 100; ++i) {
        f.step();
    }
    require(f.player.words[15] == -1 && f.flight.attitude[2] == -7,
            "Negative bank decay lost the original arithmetic-shift residue");
}

void test_ground_response() {
    Fixture f;
    f.player.words[8] = 100 * 256;
    f.step();
    require(f.player.words[2] == 486 && f.player.words[11] == 1000 && f.flight.ground_impacts == 1,
            "Ground impact did not retain damage and vertical impulse");
}

void test_analog_controls() {
    Fixture f;
    f.flight.controller = swiv3d::ControllerKind::joystick;
    f.flight.axis_bindings = {1,2,3,1};
    f.flight.axes = {128,-64,32};
    f.keys[0xc8] = true;
    f.step();
    require(f.player.words[13] == 252 && f.player.words[14] == 2 && f.player.words[15] == 4,
            "Analog bindings did not replace the corresponding keyboard motion");
    Fixture slide;
    slide.flight.controller = swiv3d::ControllerKind::mouse;
    slide.flight.axes = {-128,0,0};
    slide.keys[0x1d] = true;
    slide.step();
    require(slide.player.words[13] == 248 && slide.player.words[15] == -7,
            "Analog slide changed yaw or used the turning bank");
}

}

int main() {
    try {
        test_control_order_and_drag();
        test_opposed_and_slide_controls();
        test_ground_response();
        test_analog_controls();
        std::cout << "Helicopter control, integration, wrap and ground tests passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
