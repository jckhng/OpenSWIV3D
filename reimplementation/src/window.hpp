#pragma once

#include "swiv3d/mission.hpp"

namespace swiv3d::host {

enum class EscapeAction { quit, forward };

class Window {
public:
    Window();
    ~Window();
    bool poll();
    bool focused() const;
    KeyboardState keys() const;
    void present(const MissionFrame& frame);
    void escape_action(EscapeAction action);

private:
    class Impl;
    std::unique_ptr<Impl> impl_;
};

}
