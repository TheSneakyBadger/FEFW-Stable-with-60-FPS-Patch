// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

// SPDX-FileCopyrightText: Copyright 2024 yuzu Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <thread>

#include <fmt/format.h>

#include "common/logging.h"

#include "common/adpf.h"
#include "common/fefw_frame_gen_hook.h"
#include "common/settings.h"
#include "common/thread.h"
#include "core/core.h"
#include "core/core_timing.h"
#include "core/fefw_frame_rate.h"
#include "core/hle/service/vi/conductor.h"
#include "core/hle/service/vi/container.h"
#include "core/hle/service/vi/display_list.h"
#include "core/hle/service/vi/vsync_manager.h"

constexpr auto FrameNs = std::chrono::nanoseconds{1000000000 / 60};

constexpr s64 UNLOCKED_TARGET_DIVISOR = 4;

namespace Service::VI {

Conductor::Conductor(Core::System& system, Container& container, DisplayList& displays)
    : m_system(system), m_container(container) {
    displays.ForEachDisplay([&](Display& display) {
        m_vsync_managers.insert({display.GetId(), VsyncManager{}});
    });

    if (system.IsMulticore()) {
        m_event = Core::Timing::CreateEvent(
            "ScreenComposition",
            [this](s64 time,
                   std::chrono::nanoseconds ns_late) -> std::optional<std::chrono::nanoseconds> {
                m_signal.Set();
                return std::chrono::nanoseconds(this->GetNextTicks());
            });

        system.CoreTiming().ScheduleLoopingEvent(FrameNs, FrameNs, m_event);
        m_thread = std::jthread([this](std::stop_token token) { this->VsyncThread(token); });
    } else {
        m_event = Core::Timing::CreateEvent(
            "ScreenComposition",
            [this](s64 time,
                   std::chrono::nanoseconds ns_late) -> std::optional<std::chrono::nanoseconds> {
                this->ProcessVsync();
                return std::chrono::nanoseconds(this->GetNextTicks());
            });

        system.CoreTiming().ScheduleLoopingEvent(FrameNs, FrameNs, m_event);
    }
}

Conductor::~Conductor() {
    m_system.CoreTiming().UnscheduleEvent(m_event);

    if (m_system.IsMulticore()) {
        m_thread.request_stop();
        m_signal.Set();
    }
}

void Conductor::LinkVsyncEvent(u64 display_id, Event* event) {
    if (auto it = m_vsync_managers.find(display_id); it != m_vsync_managers.end()) {
        it->second.LinkVsyncEvent(event);
    }
}

void Conductor::UnlinkVsyncEvent(u64 display_id, Event* event) {
    if (auto it = m_vsync_managers.find(display_id); it != m_vsync_managers.end()) {
        it->second.UnlinkVsyncEvent(event);
    }
}

void Conductor::ProcessVsync() {
    Common::PollThreadPolicies();
    Common::ADPF::SetTargetWorkDuration(std::chrono::nanoseconds{this->GetFramePeriodNs()});

    for (auto& [display_id, manager] : m_vsync_managers) {
        m_container.ComposeOnDisplay(&m_swap_interval, &m_compose_speed_scale, display_id);
        manager.SignalVsync(m_system.Kernel());
    }
}

void Conductor::VsyncThread(std::stop_token token) {
    Common::SetCurrentThreadName("VSyncThread");
    Common::SetCurrentThreadPriority(Common::ThreadPriority::VeryHigh);
    Common::SetCurrentThreadToPerformanceCores();

    std::chrono::steady_clock::time_point last_vsync{};
    while (!token.stop_requested()) {
        if (m_reflex_paced.load(std::memory_order_relaxed) &&
            !FefwFrameGenHook::timer_paces.load(std::memory_order_relaxed)) {
            // NVIDIA Reflex sets when each vsync comes: the timer isn't waited for (at 60 Hz the
            // two clocks beat, Reflex at 59.5 a second waiting out ticks that came late: the game
            // averaged 58.6). About the timer's own interval after the last one at the soonest,
            // for the frames Reflex doesn't sleep before: 1 ms less, as the sleep overshoots by
            // up to a millisecond (with the whole interval it was the limit itself: 56.7 a second).
            std::this_thread::sleep_until(last_vsync +
                                          std::chrono::nanoseconds{this->GetNextTicks()} -
                                          std::chrono::milliseconds{1});
        } else {
            m_signal.Wait();
        }

        if (m_system.IsShuttingDown()) {
            return;
        }

        this->VsyncGrace(token);

        if (auto* const reflex_sleep =
                FefwFrameGenHook::vsync_sleep.load(std::memory_order_acquire)) {
            m_reflex_paced.store(reflex_sleep(), std::memory_order_relaxed);
        }

        last_vsync = std::chrono::steady_clock::now();
        this->ProcessVsync();
    }
}

/// Fortune's Eden, 60 FPS mode: when a vsync comes and the game's next frame isn't queued yet,
/// the vsync waits for it a few ms (as soon as it is queued) instead of going ahead without it and
/// showing it a whole vsync later. Frames taking 17-20 ms (a busy battle) otherwise alternated
/// between 16.7 and 33.3 ms on screen, 40 FPS in all (#63, #78), where the game could have made
/// about 55. It is the same vsync, only later: NVIDIA Reflex still sleeps and marks the frame's
/// start once for it (test 91 showed a late frame with a composite of its own, past Reflex, which
/// left Reflex sleeping 24 ms before every frame for minutes: 41.7 FPS, #63). Not with swap
/// intervals over 1 (the game's own 30 FPS), movies or a speed limit other than 100%.
/// EDEN_VSYNC_GRACE_MS=<ms> (0-8, default 4; 0: off).
void Conductor::VsyncGrace(std::stop_token token) {
    static const s64 grace_ns = [] {
        const char* const value = std::getenv("EDEN_VSYNC_GRACE_MS");
        const int ms = value ? std::clamp(std::atoi(value), 0, 8) : 4;
        LOG_INFO(Service_VI, "Vsync grace: {}", ms > 0 ? fmt::format("{} ms", ms)
                                                         : std::string{"off"});
        return static_cast<s64>(ms) * 1'000'000;
    }();
    const auto& settings = Settings::values;
    if (grace_ns <= 0 || !FefwFrameRate::Enabled() || m_swap_interval != 1 ||
        m_compose_speed_scale != 1.f || !settings.use_speed_limit.GetValue() ||
        Settings::SpeedLimit() != 100 || m_system.GetNVDECActive()) {
        return;
    }
    const auto game_frame_queued = [&] {
        bool queued = false;
        for (const auto& [display_id, manager] : m_vsync_managers) {
            queued = queued || m_container.HasQueuedGameFrame(display_id);
        }
        return queued;
    };
    if (game_frame_queued()) {
        return;
    }
    using Clock = std::chrono::steady_clock;
    const auto start = Clock::now();
    const auto deadline = start + std::chrono::nanoseconds{grace_ns};
    bool shown = false;
    while (!token.stop_requested() && !m_system.IsShuttingDown()) {
        std::this_thread::sleep_for(std::chrono::microseconds{250});
        if (game_frame_queued()) {
            shown = true;
            break;
        }
        if (Clock::now() >= deadline) {
            break;
        }
    }
    if (shown) {
        ++m_grace_shown;
        m_grace_wait_ns += std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now() -
                                                                                start)
                               .count();
    } else {
        ++m_grace_missed;
    }
    const auto now = Clock::now();
    if (m_grace_log_time == Clock::time_point{}) {
        m_grace_log_time = now;
    } else if (now - m_grace_log_time >= std::chrono::seconds{10}) {
        if (m_grace_shown > 0) {
            LOG_INFO(Service_VI,
                     "Vsync grace since the last report: {} vsyncs waited {:.1f} ms on average for a "
                     "late game frame (instead of showing it a whole vsync later); {} went ahead "
                     "without one",
                     m_grace_shown,
                     static_cast<double>(m_grace_wait_ns) / 1e6 /
                         static_cast<double>(m_grace_shown),
                     m_grace_missed);
        }
        m_grace_log_time = now;
        m_grace_shown = 0;
        m_grace_missed = 0;
        m_grace_wait_ns = 0;
    }
}

s64 Conductor::GetNextTicks() const {
    const auto& settings = Settings::values;
    auto speed_scale = 1.f;
    if (settings.use_multi_core.GetValue()) {
        if (settings.use_speed_limit.GetValue()) {
            // Scales the speed based on speed_limit setting on MC. SC is handled by
            // SpeedLimiter::DoSpeedLimiting.
            speed_scale = 100.f / Settings::SpeedLimit();
        } else {
            // Run at unlocked framerate.
            speed_scale = 0.01f;
        }
    }

    // Adjust by speed limit determined during composition.
    speed_scale /= m_compose_speed_scale;

    if (m_system.GetNVDECActive() && settings.use_video_framerate.GetValue()) {
        // Run at intended presentation rate during video playback.
        speed_scale = 1.f;
    }

    const f32 effective_fps = 60.f / static_cast<f32>(m_swap_interval);
    const s64 ticks = static_cast<s64>(speed_scale * (1000000000.f / effective_fps));
    // While frame generation makes frames: the game's frames come no faster than the display (or
    // the frame rate cap) can show them with the frames made between them (165 Hz at 3x: 54 a
    // second), evenly, from the game's own vsync. (Held back on the GPU thread instead, the game
    // still saw a 60 Hz vsync: its frames came 1 or 2 vsyncs apart and the motion stuttered.)
    // NVIDIA Reflex pacing the vsync itself: its frame limit holds the game to the rate instead.
    if (settings.use_speed_limit.GetValue() && Settings::SpeedLimit() == 100 &&
        m_compose_speed_scale == 1.f &&
        (!m_reflex_paced.load(std::memory_order_relaxed) ||
         FefwFrameGenHook::timer_paces.load(std::memory_order_relaxed))) {
        return (std::max)(ticks, Settings::FrameGenerationPeriodNs());
    }
    return ticks;
}

s64 Conductor::GetFramePeriodNs() const {
    const auto& settings = Settings::values;
    f32 speed_scale = 1.f;
    bool unlocked = false;
    if (settings.use_multi_core.GetValue()) {
        if (settings.use_speed_limit.GetValue()) {
            speed_scale = 100.f / Settings::SpeedLimit();
        } else {
            unlocked = true;
        }
    }
    speed_scale /= m_compose_speed_scale;

    const f32 effective_fps = 60.f / static_cast<f32>(m_swap_interval);
    s64 period = static_cast<s64>(speed_scale * (1000000000.f / effective_fps));
    if (unlocked) {
        period /= UNLOCKED_TARGET_DIVISOR;
    }
    return std::clamp<s64>(period, 1'000'000, 100'000'000);
}

} // namespace Service::VI
