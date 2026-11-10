// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include "core/fefw_frame_rate.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <string>

#include <fmt/format.h>

#include "common/hex_util.h"
#include "common/logging.h"
#include "core/memory.h"

namespace FefwFrameRate {
namespace {
constexpr u64 AppObjectPointer = 0x21E60D8; // main-relative: pointer to the application object
constexpr u64 RateOffset = 0x1DC;            // int: the frames per second the game steps for
constexpr int MinRate = 30;                  // the game's own rate; below it, it slows as on Switch
constexpr int MaxRate = 60;
constexpr int SnapTo60From = 57;             // vsync jitter around 60 shouldn't wobble the rate

std::atomic<u64> main_base{0};
std::atomic<bool> enabled{false};

struct Measure {
    using Clock = std::chrono::steady_clock;
    Clock::time_point last{};
    double average_ms = 1000.0 / MaxRate;
    int written = 0;
    // Summary for the log, every 10 s.
    Clock::time_point period_start{};
    int min_rate = MaxRate;
    int max_rate = MinRate;
    u64 frames = 0;
    double rate_sum = 0.0;
};
Measure measure;
} // Anonymous namespace

void OnMainLoaded(const std::array<u8, 32>& build_id, u64 base) {
    const std::string hex = Common::HexToString(build_id, true);
    if (std::string_view{hex}.substr(0, MainBuildId.size()) == MainBuildId) {
        main_base = base;
        measure = {};
    } else {
        main_base = 0;
    }
}

void SetEnabled(bool on) {
    if (enabled.exchange(on) != on) {
        LOG_INFO(Loader, "Fortune's Weave 60 FPS mode {}", on ? "on (timing follows the real frame rate, 30-60)" : "off");
    }
}

bool Enabled() {
    return enabled && main_base != 0;
}

void OnPresent(Core::Memory::Memory& memory) {
    if (!Enabled()) {
        return;
    }
    using Clock = Measure::Clock;
    const auto now = Clock::now();
    if (measure.last == Clock::time_point{}) {
        measure.last = measure.period_start = now;
        return;
    }
    const double gap_ms = std::chrono::duration<double, std::milli>(now - measure.last).count();
    measure.last = now;
    // A load, a pause or the window in the background: keep the rate, start measuring again.
    if (gap_ms > 250.0) {
        measure.average_ms = 1000.0 / std::max(measure.written, MinRate);
        return;
    }
    // A short average (about 4 frames) follows real changes within a few frames without passing
    // single-frame hitches straight into the game's step.
    measure.average_ms += (gap_ms - measure.average_ms) * 0.25;
    int rate = static_cast<int>(std::lround(1000.0 / measure.average_ms));
    if (rate >= SnapTo60From) {
        rate = MaxRate;
    }
    rate = std::clamp(rate, MinRate, MaxRate);

    const u64 object_slot = main_base + AppObjectPointer;
    if (!memory.IsValidVirtualAddress(object_slot)) {
        return;
    }
    const u64 object = memory.Read64(object_slot);
    if (object == 0 || !memory.IsValidVirtualAddress(object + RateOffset)) {
        return;
    }
    if (static_cast<int>(memory.Read32(object + RateOffset)) != rate) {
        memory.Write32(object + RateOffset, static_cast<u32>(rate));
    }
    measure.written = rate;

    ++measure.frames;
    measure.rate_sum += rate;
    measure.min_rate = std::min(measure.min_rate, rate);
    measure.max_rate = std::max(measure.max_rate, rate);
    if (now - measure.period_start >= std::chrono::seconds{10}) {
        LOG_INFO(Loader, "60 FPS mode, last 10 s: game timing rate {:.1f} on average ({} to {})",
                 measure.rate_sum / static_cast<double>(measure.frames), measure.min_rate,
                 measure.max_rate);
        measure.period_start = now;
        measure.frames = 0;
        measure.rate_sum = 0.0;
        measure.min_rate = MaxRate;
        measure.max_rate = MinRate;
    }
}

} // namespace FefwFrameRate
