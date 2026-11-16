// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

// SPDX-FileCopyrightText: Copyright 2024 yuzu Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <algorithm>

#include "common/adpf.h"
#include "common/settings.h"
#include "common/thread.h"
#include "core/core.h"
#include "core/core_timing.h"
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

    while (!token.stop_requested()) {
        m_signal.Wait();

        if (m_system.IsShuttingDown()) {
            return;
        }

        this->ProcessVsync();
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
    if (settings.use_speed_limit.GetValue() && Settings::SpeedLimit() == 100 &&
        m_compose_speed_scale == 1.f) {
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
