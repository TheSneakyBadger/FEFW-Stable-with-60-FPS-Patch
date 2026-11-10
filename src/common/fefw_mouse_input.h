// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <mutex>

// Fortune's Eden mouse mode: the frontend locks the PC cursor in the middle of the game window and
// hands over how far the mouse moved (as fractions of the window) and wheel turns (Qt's 120 per
// notch). The HID service takes them when it builds the Joy-Con mouse samples.
namespace FefwMouseInput {

struct Pending {
    float dx{};
    float dy{};
    int wheel_x{};
    int wheel_y{};
};

namespace Detail {
inline std::mutex mutex;
inline Pending pending;
} // namespace Detail

inline void AddMotion(float dx, float dy) {
    std::scoped_lock lock{Detail::mutex};
    Detail::pending.dx += dx;
    Detail::pending.dy += dy;
}

inline void AddWheel(int x, int y) {
    std::scoped_lock lock{Detail::mutex};
    Detail::pending.wheel_x += x;
    Detail::pending.wheel_y += y;
}

/// Everything since the last call.
inline Pending Take() {
    std::scoped_lock lock{Detail::mutex};
    const Pending taken = Detail::pending;
    Detail::pending = {};
    return taken;
}

} // namespace FefwMouseInput
