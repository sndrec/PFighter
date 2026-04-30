#pragma once

#include "core/math.hpp"

#include <array>
#include <cstdint>

namespace pf {

enum Button : uint16_t {
    ButtonJump = 1 << 0,
    ButtonSpecial = 1 << 1,
    ButtonAttack = 1 << 2,
    ButtonShield = 1 << 3,
    ButtonGrab = 1 << 4,
    ButtonPause = 1 << 5,
};

struct InputFrame {
    Vec2 move;
    Vec2 cStick;
    Fix shieldAnalog = 0;
    uint16_t buttons = 0;
};

struct InputBuffer {
    static constexpr int kSize = 32;
    std::array<InputFrame, kSize> frames{};

    void push(InputFrame input) {
        if ((input.buttons & ButtonGrab) != 0) {
            input.buttons |= ButtonShield | ButtonAttack;
        }
        for (int i = kSize - 1; i > 0; --i) {
            frames[static_cast<size_t>(i)] = frames[static_cast<size_t>(i - 1)];
        }
        frames[0] = input;
    }

    bool down(uint16_t button) const {
        return (frames[0].buttons & button) != 0;
    }

    bool justPressed(uint16_t button) const {
        return (frames[0].buttons & button) != 0 && (frames[1].buttons & button) == 0;
    }

    bool buffered(uint16_t button) const {
        for (const InputFrame& frame : frames) {
            if ((frame.buttons & button) != 0) {
                return true;
            }
        }
        return false;
    }

};

} // namespace pf
