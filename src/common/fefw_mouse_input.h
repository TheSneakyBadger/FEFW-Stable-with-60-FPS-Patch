// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
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
// Fortune's Weave zooms with the left stick (up/down), not a wheel: each wheel notch holds the
// stick up or down for a moment.
inline std::atomic<std::int64_t> wheel_stick_until_ns{0};
inline std::atomic<int> wheel_stick_direction{0};

inline std::int64_t NowNs() {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(
               std::chrono::steady_clock::now().time_since_epoch())
        .count();
}
} // namespace Detail

inline void AddMotion(float dx, float dy) {
    std::scoped_lock lock{Detail::mutex};
    Detail::pending.dx += dx;
    Detail::pending.dy += dy;
}

inline void AddWheel(int x, int y) {
    {
        std::scoped_lock lock{Detail::mutex};
        Detail::pending.wheel_x += x;
        Detail::pending.wheel_y += y;
    }
    if (y == 0) {
        return;
    }
    // 120 ms of full stick per notch (Qt: 120 per notch), up to 600 ms queued.
    constexpr std::int64_t per_notch_ns = 120'000'000;
    constexpr std::int64_t max_ns = 600'000'000;
    const int direction = y > 0 ? 1 : -1;
    const std::int64_t now = Detail::NowNs();
    std::int64_t until = Detail::wheel_stick_until_ns.load();
    if (Detail::wheel_stick_direction.exchange(direction) != direction || until < now) {
        until = now; // A new direction starts over
    }
    const std::int64_t add = std::max<std::int64_t>(per_notch_ns * (y < 0 ? -y : y) / 120,
                                                    per_notch_ns / 4);
    Detail::wheel_stick_until_ns.store(std::min(until + add, now + max_ns));
}

/// The left stick's Y while a wheel turn is being played back (0 when none): +32767 up.
inline int WheelStickY() {
    if (Detail::NowNs() >= Detail::wheel_stick_until_ns.load()) {
        return 0;
    }
    return Detail::wheel_stick_direction.load() * 32767;
}

/// Everything since the last call.
inline Pending Take() {
    std::scoped_lock lock{Detail::mutex};
    const Pending taken = Detail::pending;
    Detail::pending = {};
    return taken;
}

} // namespace FefwMouseInput
