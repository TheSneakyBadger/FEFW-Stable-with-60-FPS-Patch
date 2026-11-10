// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include "core/fefw_frame_rate.h"

#include <algorithm>
#include <array>
#include <bit>
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
constexpr int SnapTo60From = 59;             // vsync jitter around 60 shouldn't wobble the rate
// The 60 FPS game patch's table (bss tail, main-relative; layout in tools/fefw/make_60fps_patch.py):
// the value every patched site uses at the current rate.
constexpr u64 TableOffset = 0x22B3E00;

std::atomic<u64> main_base{0};
std::atomic<bool> enabled{false};

struct Measure {
    using Clock = std::chrono::steady_clock;
    Clock::time_point last{};
    double average_ms = 1000.0 / MaxRate;
    // The last three frame gaps; their median feeds the average, so one long frame (a shader
    // build, a hitch) doesn't pull the rate down and back up over the frames after it.
    std::array<double, 3> recent_ms{};
    u64 recent_count = 0;
    int written = 0;
    // Summary for the log, every 10 s.
    Clock::time_point period_start{};
    int min_rate = MaxRate;
    int max_rate = MinRate;
    u64 frames = 0;
    double rate_sum = 0.0;
};
Measure measure;
float Bits(u32 bits) {
    return std::bit_cast<float>(bits);
}

u32 F(double value) {
    return std::bit_cast<u32>(static_cast<float>(value));
}

// s = 30 / rate. Per-frame amounts scale by s, per-frame factors become powers of s, and frame
// counts scale by 1 / s, so each system keeps its 30 FPS speed at any rate.
void WriteTable(Core::Memory::Memory& memory, u64 table, int rate) {
    const double s = 30.0 / rate;
    const auto put = [&](u64 slot, u32 value) { memory.Write32(table + slot, value); };
    const auto times = [&](u32 bits) { return F(Bits(bits) * s); };
    put(0x04, F(s));
    put(0x08, static_cast<u32>(std::lround(4096.0 * s)));          // 30 Hz step, 1/4096 units
    put(0x0C, times(0x3D08AB86));                                 // clock fallback dt
    put(0x10, times(0x3D08AB85));                                 // shader time per frame
    put(0x14, times(0x3D088889));                                 // PhysX step (1/30 -> 1/T)
    put(0x18, F(std::pow(Bits(0x3F733333), s)));                  // x0.95 per frame
    put(0x1C, F(Bits(0x3F4CCCCD) / s));                           // hold frames x T/30
    put(0x20, F(3.0 * s));                                        // map UI angle per frame
    put(0x24, F(1.0 - std::pow(1.0 - Bits(0x3ECCCCCD), s)));      // marker glide lerp 0.4
    put(0x28, F(1.0 - std::pow(0.5, s)));                         // marker height ease 0.5
    put(0x2C, times(0x3DCCCCCD));                                 // +0.1 per frame
    put(0x30, times(0x41855555));                                 // +16.667 ms per frame
    put(0x34, F(1.0 - std::pow(31.0 / 32.0, s)));                 // marquee lerp 1/32
    put(0x38, times(0xBF19999A));                                 // turntable acceleration
    put(0x3C, F(1.0 / (1.0 - std::pow(5.0 / 6.0, s))));           // turntable release (/6)
    put(0x40, static_cast<u32>(std::max(1L, std::lround(2.0 * s)))); // PhysX substeps (2)
    put(0x44, static_cast<u32>(std::max(1L, std::lround(4.0 * s)))); // PhysX substeps (4)
    put(0x48, rate == 30 ? 0U : F(s - 1.0));                      // turntable angle: v x (s - 1)
    put(0x00, static_cast<u32>(rate));                            // last: the table is complete
}
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
        measure.recent_count = 0;
        return;
    }
    // Median of the last three gaps: a single slow frame is ignored (the game loses that bit of
    // time, as on Switch, rather than speeding up afterwards to catch up), while a real change
    // in frame rate shows up after two frames. Then a short average (about 4 frames) smooths it.
    measure.recent_ms[measure.recent_count % 3] = gap_ms;
    ++measure.recent_count;
    double sample = gap_ms;
    if (measure.recent_count >= 3) {
        auto sorted = measure.recent_ms;
        std::sort(sorted.begin(), sorted.end());
        sample = sorted[1];
    }
    measure.average_ms += (sample - measure.average_ms) * 0.25;
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
    const u64 table = main_base + TableOffset;
    if (memory.IsValidVirtualAddress(table) &&
        static_cast<int>(memory.Read32(table)) != rate) {
        WriteTable(memory, table, rate);
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
