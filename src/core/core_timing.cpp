// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

// SPDX-FileCopyrightText: Copyright 2020 yuzu Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <algorithm>
#include <mutex>
#include <string>
#include <tuple>
#include "common/cpu_features.h"
#include "common/cpu_features.h"

#ifdef _WIN32
#include "common/windows/timer_resolution.h"
#endif

#include "common/settings.h"
#include "core/core_timing.h"
#include "core/hardware_properties.h"

namespace Core::Timing {

constexpr s64 MAX_SLICE_LENGTH = 10000;

std::shared_ptr<EventType> CreateEvent(std::string name, TimedCallback&& callback) {
    return std::make_shared<EventType>(std::move(callback), std::move(name));
}

struct CoreTiming::Event {
    s64 time;
    u64 fifo_order;
    std::weak_ptr<EventType> type;
    s64 reschedule_time;
    heap_t::handle_type handle{};

    // Sort by time, unless the times are the same, in which case sort by
    // the order added to the queue
    friend bool operator>(const Event& left, const Event& right) {
        return std::tie(left.time, left.fifo_order) > std::tie(right.time, right.fifo_order);
    }

    friend bool operator<(const Event& left, const Event& right) {
        return std::tie(left.time, left.fifo_order) < std::tie(right.time, right.fifo_order);
    }
};

CoreTiming::CoreTiming() = default;
CoreTiming::~CoreTiming() {
    Reset();
}

void CoreTiming::Initialize(std::function<void()>&& on_thread_init_) {
    Reset();
    on_thread_init = std::move(on_thread_init_);
    event_fifo_id = 0;
    cpu_ticks = 0;
    clock_frozen = false;
    offset_ns = 0;
    offset_cntpct = 0;
    offset_gpu_tick = 0;
    if (is_multicore) {
        timer_thread = std::jthread([this](std::stop_token stop_token) {
            Common::SetCurrentThreadName("HostTiming");
            Common::SetCurrentThreadPriority(Common::ThreadPriority::VeryHigh);
            Common::SetCurrentThreadToPerformanceCores();
            on_thread_init();
            has_started = true;

            // base frequency in MHz: 1ns (10^-9) = 1GHz (10^9)
            while (!stop_token.stop_requested()) {
                while (!paused && !stop_token.stop_requested()) {
                    paused_set = false;
                    if (auto const next_time = Advance(); next_time) {
                        // There are more events left in the queue, wait until the next event.
                        auto const wait_time = *next_time - GetGlobalTimeNs().count();
                        if (wait_time > 0) {
                            event.WaitFor(std::chrono::nanoseconds(wait_time));
                        }
                    } else {
                        // Queue is empty, wait until another event is scheduled and signals us to
                        // continue.
                        wait_set = true;
                        event.Wait();
                    }
                    wait_set = false;
                }
                paused_set = true;
                pause_event.Wait();
            }
        });
    }
}

void CoreTiming::ClearPendingEvents() {
    std::scoped_lock lock{advance_lock, basic_lock};
    event_queue.clear();
    event.Set();
}

void CoreTiming::Pause(bool is_paused) {
    paused = is_paused;
    pause_event.Set();

    if (!is_paused) {
        pause_end_time = GetGlobalTimeNs().count();
    }
}

void CoreTiming::FreezeGuestClock() {
    if (!is_multicore || clock_frozen) {
        return;
    }
    // A guest thread that read the clock just before this keeps its reading, so the frozen time
    // is put 1 ms ahead: the clock never goes backwards.
    static constexpr s64 Margin = 1'000'000; // ns
    frozen_ns = Common::g_wall_clock.GetTimeNS().count() - offset_ns + Margin;
    frozen_cntpct = Common::g_wall_clock.GetCNTPCT() - offset_cntpct +
                    Margin * s64{Common::WallClock::CNTFRQ} / 1'000'000'000;
    frozen_gpu_tick = Common::g_wall_clock.GetGPUTick() - offset_gpu_tick +
                      Margin * s64{Common::WallClock::GPUTickFreq} / 1'000'000'000;
    clock_frozen = true;
}

void CoreTiming::ThawGuestClock() {
    if (!clock_frozen) {
        return;
    }
    // Carry on from the frozen time: the pause is taken off. The offsets are stored before the
    // flag is cleared, so a reading after it uses them.
    offset_ns = Common::g_wall_clock.GetTimeNS().count() - frozen_ns;
    offset_cntpct = Common::g_wall_clock.GetCNTPCT() - frozen_cntpct;
    offset_gpu_tick = Common::g_wall_clock.GetGPUTick() - frozen_gpu_tick;
    clock_frozen = false;
}

void CoreTiming::SyncPause(bool is_paused) {
    if (is_paused == paused && paused_set == paused) {
        return;
    }

    // Fortune's Weave times its cutscenes, movies and voiced text by the clock: if it kept
    // running through a pause, the game raced to catch up afterwards and the voices fell behind
    // the pictures (#54).
    if (is_paused) {
        FreezeGuestClock();
    }
    Pause(is_paused);
    if (timer_thread.joinable()) {
        if (!is_paused) {
            pause_event.Set();
        }
        event.Set();
        while (paused_set != is_paused)
            ;
    }

    if (!is_paused) {
        ThawGuestClock();
        pause_end_time = GetGlobalTimeNs().count();
    }
}

bool CoreTiming::IsRunning() const {
    return !paused_set;
}

bool CoreTiming::HasPendingEvents() const {
    std::scoped_lock lock{basic_lock};
    return !(wait_set && event_queue.empty());
}

void CoreTiming::ScheduleEvent(std::chrono::nanoseconds ns_into_future,
                               const std::shared_ptr<EventType>& event_type, bool absolute_time) {
    {
        std::scoped_lock scope{basic_lock};
        const auto next_time{absolute_time ? ns_into_future : GetGlobalTimeNs() + ns_into_future};

        auto h{event_queue.emplace(Event{next_time.count(), event_fifo_id++, event_type, 0})};
        (*h).handle = h;
    }

    event.Set();
}

void CoreTiming::ScheduleLoopingEvent(std::chrono::nanoseconds start_time,
                                      std::chrono::nanoseconds resched_time,
                                      const std::shared_ptr<EventType>& event_type,
                                      bool absolute_time) {
    {
        std::scoped_lock scope{basic_lock};
        const auto next_time{absolute_time ? start_time : GetGlobalTimeNs() + start_time};

        auto h{event_queue.emplace(
            Event{next_time.count(), event_fifo_id++, event_type, resched_time.count()})};
        (*h).handle = h;
    }

    event.Set();
}

void CoreTiming::UnscheduleEvent(const std::shared_ptr<EventType>& event_type,
                                 UnscheduleEventType type) {
    {
        std::scoped_lock lk{basic_lock};

        std::vector<heap_t::handle_type> to_remove;
        for (auto itr = event_queue.begin(); itr != event_queue.end(); itr++) {
            const Event& e = *itr;
            if (e.type.lock().get() == event_type.get()) {
                to_remove.push_back(itr->handle);
            }
        }

        for (auto& h : to_remove) {
            event_queue.erase(h);
        }

        event_type->sequence_number++;
    }

    // Force any in-progress events to finish
    if (type == UnscheduleEventType::Wait) {
        std::scoped_lock lk{advance_lock};
    }
}

static u64 GetNextTickCount(u64 next_ticks) {
    if (Settings::values.use_custom_cpu_ticks.GetValue()) {
        return Settings::values.cpu_ticks.GetValue();
    }
    return next_ticks;
}

void CoreTiming::AddTicks(u64 ticks_to_add) {
    const u64 ticks = GetNextTickCount(ticks_to_add);
    cpu_ticks += ticks;
    downcount -= static_cast<s64>(ticks);
}

void CoreTiming::Idle() {
    AddTicks(1000U);
}

void CoreTiming::ResetTicks() {
    downcount = MAX_SLICE_LENGTH;
}

u64 CoreTiming::GetClockTicks() const {
    // The guest counter always runs at the real rate. CPU Clocks (Boost 1734 MHz, Overclock
    // 2040 MHz) used to scale it, which runs Fortune's Weave's clock 1.7-2x fast: its movies
    // play ahead of their voices, which drop out, and dialogue animations finish early. The
    // setting is ignored (and hidden), so a saved Boost or Overclock does nothing.
    u64 fres;
    if (!is_multicore) {
        fres = Common::WallClock::CPUTickToCNTPCT(cpu_ticks);
    } else if (clock_frozen) {
        fres = u64(frozen_cntpct.load());
    } else {
        fres = u64(Common::g_wall_clock.GetCNTPCT() - offset_cntpct.load());
    }
    if (::Settings::values.sync_core_speed.GetValue()) {
        auto const ticks = f64(fres);
        auto const speed_limit = f64(Settings::SpeedLimit()) * 0.01;
        return u64(ticks / speed_limit);
    }
    return fres;
}

u64 CoreTiming::GetGPUTicks() const {
    if (!is_multicore) {
        return Common::WallClock::CPUTickToGPUTick(cpu_ticks);
    }
    if (clock_frozen) {
        return u64(frozen_gpu_tick.load());
    }
    return u64(Common::g_wall_clock.GetGPUTick() - offset_gpu_tick.load());
}

std::optional<s64> CoreTiming::Advance() {
    std::scoped_lock lock{advance_lock, basic_lock};
    global_timer = GetGlobalTimeNs().count();

    while (!event_queue.empty() && event_queue.top().time <= global_timer) {
        const Event& evt = event_queue.top();

        if (const auto event_type{evt.type.lock()}) {
            const auto evt_time = evt.time;
            const auto evt_sequence_num = event_type->sequence_number;

            if (evt.reschedule_time == 0) {
                event_queue.pop();

                basic_lock.unlock();

                event_type->callback(
                    evt_time, std::chrono::nanoseconds{GetGlobalTimeNs().count() - evt_time});

                basic_lock.lock();
            } else {
                basic_lock.unlock();

                const auto new_schedule_time{event_type->callback(
                    evt_time, std::chrono::nanoseconds{GetGlobalTimeNs().count() - evt_time})};

                basic_lock.lock();

                if (evt_sequence_num != event_type->sequence_number) {
                    // Heap handle is invalidated after external modification.
                    continue;
                }

                const auto next_schedule_time{new_schedule_time.has_value()
                                                  ? new_schedule_time.value().count()
                                                  : evt.reschedule_time};

                // If this event was scheduled into a pause, its time now is going to be way
                // behind. Re-set this event to continue from the end of the pause.
                auto next_time{evt.time + next_schedule_time};
                if (evt.time < pause_end_time) {
                    next_time = pause_end_time + next_schedule_time;
                }

                event_queue.update(evt.handle, Event{next_time, event_fifo_id++, evt.type,
                                                     next_schedule_time, evt.handle});
            }
        }

        global_timer = GetGlobalTimeNs().count();
    }

    if (!event_queue.empty()) {
        return event_queue.top().time;
    } else {
        return std::nullopt;
    }
}

void CoreTiming::Reset() {
    paused = true;
    pause_event.Set();
    event.Set();
    if (timer_thread.joinable()) {
        timer_thread.request_stop();
        timer_thread.join();
    }
    has_started = false;
}

/// @brief Returns current time in nanoseconds.
std::chrono::nanoseconds CoreTiming::GetGlobalTimeNs() const noexcept {
    if (!is_multicore) {
        return std::chrono::nanoseconds{Common::WallClock::CPUTickToNS(cpu_ticks)};
    }
    if (clock_frozen) {
        return std::chrono::nanoseconds{frozen_ns.load()};
    }
    return Common::g_wall_clock.GetTimeNS() - std::chrono::nanoseconds{offset_ns.load()};
}

/// @brief Returns current time in microseconds.
std::chrono::microseconds CoreTiming::GetGlobalTimeUs() const noexcept {
    if (!is_multicore) {
        return std::chrono::microseconds{Common::WallClock::CPUTickToUS(cpu_ticks)};
    }
    return std::chrono::duration_cast<std::chrono::microseconds>(GetGlobalTimeNs());
}

#ifdef _WIN32
void CoreTiming::SetTimerResolutionNs(std::chrono::nanoseconds ns) {
    timer_resolution_ns = ns.count();
}
#endif

} // namespace Core::Timing
