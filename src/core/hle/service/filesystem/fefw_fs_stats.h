// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <atomic>
#include <chrono>

#include "common/common_types.h"

// Fortune's Eden: how long the game's file system calls took on the PC (reads of its own files,
// its save data written and flushed, files opened), for the long-frame lines in the log: a frame
// that took 0.3-0.8 s with nothing graphical behind it lined up with the game saving or loading
// (#63). Counters only rise; readers take differences.
namespace FefwFsStats {

struct Counters {
    std::atomic<u64> reads{0};
    std::atomic<u64> read_bytes{0};
    std::atomic<u64> read_ns{0};
    std::atomic<u64> writes{0};
    std::atomic<u64> write_bytes{0};
    std::atomic<u64> write_ns{0};
    std::atomic<u64> flushes{0};   ///< flushes and commits
    std::atomic<u64> flush_ns{0};
    std::atomic<u64> opens{0};     ///< files opened, created, deleted or renamed
    std::atomic<u64> open_ns{0};
};

inline Counters& Get() {
    static Counters counters;
    return counters;
}

/// Adds the time from its creation to its end to `ns`, and 1 to `count`.
class Timer {
public:
    Timer(std::atomic<u64>& count, std::atomic<u64>& ns)
        : count_{count}, ns_{ns}, start_{std::chrono::steady_clock::now()} {}
    ~Timer() {
        count_.fetch_add(1, std::memory_order_relaxed);
        ns_.fetch_add(static_cast<u64>(std::chrono::duration_cast<std::chrono::nanoseconds>(
                                           std::chrono::steady_clock::now() - start_)
                                           .count()),
                      std::memory_order_relaxed);
    }
    Timer(const Timer&) = delete;
    Timer& operator=(const Timer&) = delete;

private:
    std::atomic<u64>& count_;
    std::atomic<u64>& ns_;
    std::chrono::steady_clock::time_point start_;
};

} // namespace FefwFsStats
