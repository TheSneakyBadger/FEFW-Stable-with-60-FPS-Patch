// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include "core/hle/kernel/fefw_cpu_stats.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdlib>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

#include <fmt/format.h>

#include "common/logging.h"
#include "core/hardware_properties.h"
#include "core/hle/kernel/k_light_lock.h"
#include "core/hle/kernel/k_process.h"
#include "core/hle/kernel/k_thread.h"
#include "core/memory.h"

namespace FefwCpuStats {
namespace {

using Clock = std::chrono::steady_clock;

// EDEN_CPU_STATS=0 turns the report off; EDEN_CPU_STATS=N reports every N seconds (default 30).
double IntervalSeconds() {
    static const double seconds = [] {
        if (const char* value = std::getenv("EDEN_CPU_STATS")) {
            return static_cast<double>(std::atoi(value));
        }
        return 30.0;
    }();
    return seconds;
}

struct State {
    std::mutex mutex;
    Clock::time_point window_start{};
    std::unordered_map<u64, s64> last_cpu_time; // thread id -> ticks at the window start
    std::unordered_map<u64, std::string> names;  // thread id -> name, read once
};

State& Get() {
    static State state;
    return state;
}

// The game's name for a thread: the SDK's thread object (pointed to from the thread's TLS at
// +0x1F8) holds its name as text. Finds the first run of 3+ printable characters ending in NUL in
// that object; empty when there is none.
std::string ReadName(Kernel::KProcess& process, const Kernel::KThread& thread) {
    auto& memory = process.GetMemory();
    const u64 tls = GetInteger(thread.GetTlsAddress());
    if (tls == 0 || !memory.IsValidVirtualAddressRange(tls + 0x1F8, 8)) {
        return {};
    }
    const u64 object = memory.Read64(tls + 0x1F8);
    std::array<u8, 0x240> bytes{};
    if (object == 0 || !memory.IsValidVirtualAddressRange(object, bytes.size()) ||
        !memory.ReadBlock(object, bytes.data(), bytes.size())) {
        return {};
    }
    for (size_t start = 0; start < bytes.size();) {
        size_t end = start;
        while (end < bytes.size() && bytes[end] >= 0x20 && bytes[end] < 0x7f) {
            ++end;
        }
        if (end - start >= 3 && end < bytes.size() && bytes[end] == 0) {
            return std::string(reinterpret_cast<const char*>(bytes.data() + start), end - start);
        }
        start = end + 1;
    }
    return {};
}

} // Anonymous namespace

void MaybeReport(const Kernel::KThread* render_thread) {
    const double interval = IntervalSeconds();
    if (interval <= 0.0 || render_thread == nullptr) {
        return;
    }
    Kernel::KProcess* const process = render_thread->GetOwnerProcess();
    if (process == nullptr) {
        return;
    }
    auto& state = Get();
    std::unique_lock lock{state.mutex, std::try_to_lock};
    if (!lock.owns_lock()) {
        return;
    }
    const auto now = Clock::now();
    const bool first = state.window_start == Clock::time_point{};
    const double seconds = std::chrono::duration<double>(now - state.window_start).count();
    if (!first && seconds < interval) {
        return;
    }

    struct Row {
        u64 id;
        double share; // of one core
        s32 priority;
        s32 core;
        bool render;
    };
    std::vector<Row> rows;
    std::array<double, Core::Hardware::NUM_CPU_CORES> core_load{};
    std::unordered_map<u64, s64> current;
    {
        Kernel::KScopedLightLock list_lock{process->GetListLock()};
        for (auto& thread : process->GetThreadList()) {
            if (thread.GetState() == Kernel::ThreadState::Terminated) {
                continue;
            }
            const u64 id = thread.GetThreadId();
            const s64 cpu = thread.GetCpuTime();
            current[id] = cpu;
            if (!state.names.contains(id)) {
                state.names[id] = ReadName(*process, thread);
            }
            if (first) {
                continue;
            }
            const auto last = state.last_cpu_time.find(id);
            const s64 delta = cpu - (last != state.last_cpu_time.end() ? last->second : 0);
            const double share = static_cast<double>(delta) /
                                 (static_cast<double>(Core::Hardware::CNTFREQ) * seconds);
            const s32 core = thread.GetActiveCore();
            if (core >= 0 && core < static_cast<s32>(core_load.size())) {
                core_load[core] += share;
            }
            rows.push_back({id, share, thread.GetPriority(), core, &thread == render_thread});
        }
    }
    state.last_cpu_time = std::move(current);
    state.window_start = now;
    if (first) {
        return;
    }
    std::ranges::sort(rows, [](const Row& a, const Row& b) { return a.share > b.share; });
    double total = 0.0;
    for (const Row& row : rows) {
        total += row.share;
    }
    std::string busiest;
    for (size_t i = 0; i < rows.size() && i < 10; ++i) {
        const Row& row = rows[i];
        if (row.share < 0.02) {
            break;
        }
        const auto& name = state.names[row.id];
        busiest += fmt::format("{}{} {:.0f}% ({}prio {}, core {})", i ? "; " : "",
                               name.empty() ? fmt::format("thread {}", row.id) : name,
                               row.share * 100.0, row.render ? "presents frames, " : "",
                               row.priority, row.core);
    }
    std::string cores;
    for (size_t i = 0; i < core_load.size(); ++i) {
        cores += fmt::format("{}{:.0f}%", i ? " / " : "", core_load[i] * 100.0);
    }
    LOG_INFO(Kernel,
             "Game CPU, last {:.0f} s: {:.0f}% of a core in all ({} threads); emulated cores {}; "
             "busiest: {}",
             seconds, total * 100.0, rows.size(), cores, busiest.empty() ? "none" : busiest);
}

} // namespace FefwCpuStats
