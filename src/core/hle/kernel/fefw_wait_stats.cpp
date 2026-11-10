// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include "core/hle/kernel/fefw_wait_stats.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cstdlib>
#include <mutex>
#include <unordered_map>
#include <vector>

#include <fmt/format.h>

#include "common/logging.h"
#include "core/hle/kernel/k_synchronization_object.h"

namespace FefwWaitStats {
namespace {

using Clock = std::chrono::steady_clock;

struct Entry {
    unsigned count{};
    Clock::duration total{};
    Clock::duration worst{};
};

struct State {
    std::mutex mutex;
    std::array<std::atomic<const Kernel::KThread*>, 2> threads{};
    std::array<std::unordered_map<std::string, Entry>, 2> waits;
    std::unordered_map<std::string, Entry> handlers;
    std::unordered_map<const Kernel::KSynchronizationObject*, std::string> names;
    std::unordered_map<const Kernel::KThread*, std::string> commands;
    Clock::time_point window_start{};
};

State& Get() {
    static State state;
    return state;
}

void Add(Entry& entry, Clock::duration duration) {
    ++entry.count;
    entry.total += duration;
    entry.worst = (std::max)(entry.worst, duration);
}

double Ms(Clock::duration duration) {
    return std::chrono::duration<double, std::milli>(duration).count();
}

std::string Summary(std::unordered_map<std::string, Entry>& map, size_t limit) {
    std::vector<std::pair<std::string, Entry>> sorted(map.begin(), map.end());
    std::ranges::sort(sorted, [](const auto& a, const auto& b) { return a.second.total > b.second.total; });
    std::string out;
    for (size_t i = 0; i < sorted.size() && i < limit; ++i) {
        const auto& [label, entry] = sorted[i];
        if (!out.empty()) out += "; ";
        out += fmt::format("{} {}x {:.0f} ms (worst {:.1f})", label, entry.count, Ms(entry.total), Ms(entry.worst));
    }
    return out.empty() ? std::string{"none"} : out;
}

} // Anonymous namespace

bool Enabled() {
    static const bool enabled = [] {
        const char* const value = std::getenv("EDEN_BACK_THREAD_STATS");
        return value != nullptr && value[0] != '\0' && value[0] != '0';
    }();
    return enabled;
}

void MarkThread(Role role, const Kernel::KThread* thread) {
    if (!Enabled() || !thread) return;
    Get().threads[static_cast<size_t>(role)].store(thread, std::memory_order_relaxed);
}

void NameObject(const Kernel::KSynchronizationObject* object, std::string name) {
    if (!Enabled() || !object) return;
    auto& state = Get();
    std::scoped_lock lock{state.mutex};
    state.names[object] = std::move(name);
}

std::string ObjectLabel(Kernel::KSynchronizationObject* object) {
    if (!object) return "?";
    {
        auto& state = Get();
        std::scoped_lock lock{state.mutex};
        if (const auto it = state.names.find(object); it != state.names.end()) return it->second;
    }
    return object->GetTypeName();
}

void AccountWait(const Kernel::KThread* thread, std::string_view label, Clock::duration duration) {
    if (!thread) return;
    auto& state = Get();
    for (size_t role = 0; role < state.threads.size(); ++role) {
        if (state.threads[role].load(std::memory_order_relaxed) != thread) continue;
        std::scoped_lock lock{state.mutex};
        Add(state.waits[role][std::string{label}], duration);
    }
}

void SetCommand(const Kernel::KThread* thread, std::string label) {
    if (!Enabled() || !thread) return;
    auto& state = Get();
    std::scoped_lock lock{state.mutex};
    state.commands[thread] = std::move(label);
}

std::string TakeCommand(const Kernel::KThread* thread) {
    auto& state = Get();
    std::scoped_lock lock{state.mutex};
    const auto it = state.commands.find(thread);
    if (it == state.commands.end()) return "ipc";
    std::string label = "ipc " + it->second;
    state.commands.erase(it);
    return label;
}

void AccountHandler(std::string_view label, Clock::duration duration) {
    if (!Enabled()) return;
    auto& state = Get();
    std::scoped_lock lock{state.mutex};
    Add(state.handlers[std::string{label}], duration);
}

void MaybeReport() {
    if (!Enabled()) return;
    auto& state = Get();
    std::scoped_lock lock{state.mutex};
    const auto now = Clock::now();
    if (state.window_start == Clock::time_point{}) {
        state.window_start = now;
        return;
    }
    const double seconds = std::chrono::duration<double>(now - state.window_start).count();
    if (seconds < 10.0) return;
    static constexpr std::array<const char*, 2> RoleNames{"rendering thread", "movie thread"};
    for (size_t role = 0; role < state.waits.size(); ++role) {
        if (!state.threads[role].load(std::memory_order_relaxed)) continue;
        Clock::duration blocked{};
        for (const auto& [label, entry] : state.waits[role]) blocked += entry.total;
        LOG_INFO(Kernel, "Waits, last {:.0f} s, {}: blocked {:.0f} ms in all; {}", seconds,
                 RoleNames[role], Ms(blocked), Summary(state.waits[role], 8));
        state.waits[role].clear();
    }
    LOG_INFO(Kernel, "Service time, last {:.0f} s (all threads): {}", seconds, Summary(state.handlers, 8));
    state.handlers.clear();
    state.window_start = now;
}

} // namespace FefwWaitStats
