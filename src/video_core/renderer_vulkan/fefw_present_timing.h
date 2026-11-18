// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <array>
#include <atomic>
#include <chrono>
#include <string>

#include <fmt/format.h>

#include "common/common_types.h"

/// Fortune's Eden: where the threads that hand the card its work spend their time waiting (#63:
/// with NVIDIA DLSS frame generation the card's work backed up while the card was only 70-80%
/// busy). Totals and the longest single wait since the last report, from any thread.
namespace FefwPresentTiming {

enum Kind : u32 {
    WorkerSubmitLock, ///< The worker waiting for the queue's submit lock.
    WorkerSubmit,     ///< The worker in vkQueueSubmit.
    WorkerRecord,     ///< The worker recording a chunk (all of it, the submit included).
    PresentAcquire,   ///< The present thread acquiring a swapchain image.
    PresentSubmitLock,
    PresentSubmit,    ///< The present thread's copy to the swapchain image, submitted.
    PresentCall,      ///< vkQueuePresentKHR (with DLSS frame generation: Streamline's present).
    ReflexSleep,      ///< NVIDIA Reflex's sleep (GPU thread, frame generation on).
    Count,
};

struct Slot {
    std::atomic<u64> total_ns{};
    std::atomic<u64> max_ns{};
    std::atomic<u32> calls{};
};

inline std::array<Slot, Count> slots{};

inline void Add(Kind kind, std::chrono::steady_clock::duration duration) {
    const u64 ns = static_cast<u64>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(duration).count());
    Slot& slot = slots[kind];
    slot.total_ns.fetch_add(ns, std::memory_order_relaxed);
    slot.calls.fetch_add(1, std::memory_order_relaxed);
    u64 max = slot.max_ns.load(std::memory_order_relaxed);
    while (ns > max && !slot.max_ns.compare_exchange_weak(max, ns, std::memory_order_relaxed)) {
    }
}

/// Times a scope.
class Scope {
public:
    explicit Scope(Kind kind_) : kind{kind_}, start{std::chrono::steady_clock::now()} {}
    ~Scope() {
        Add(kind, std::chrono::steady_clock::now() - start);
    }
    Scope(const Scope&) = delete;
    Scope& operator=(const Scope&) = delete;

private:
    Kind kind;
    std::chrono::steady_clock::time_point start;
};

/// "name X ms (max Y ms)" for each kind, and resets them.
inline std::string Take() {
    static constexpr std::array<const char*, Count> names{
        "worker: submit lock", "vkQueueSubmit", "recording",
        "present thread: acquire", "submit lock", "copy submit", "present",
        "Reflex sleep",
    };
    std::string text;
    for (u32 i = 0; i < Count; ++i) {
        Slot& slot = slots[i];
        const u64 total = slot.total_ns.exchange(0, std::memory_order_relaxed);
        const u64 max = slot.max_ns.exchange(0, std::memory_order_relaxed);
        const u32 calls = slot.calls.exchange(0, std::memory_order_relaxed);
        if (!text.empty()) {
            text += "; ";
        }
        text += fmt::format("{} {:.0f} ms ({}x, longest {:.1f} ms)", names[i],
                            static_cast<double>(total) / 1e6, calls,
                            static_cast<double>(max) / 1e6);
    }
    return text;
}

} // namespace FefwPresentTiming
