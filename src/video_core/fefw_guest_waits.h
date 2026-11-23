// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <atomic>
#include <chrono>

#include "common/common_types.h"

// Fortune's Eden: time the game's own threads spend inside the GPU caches when they touch memory
// the caches track, for the log. After an hour of play the game's main thread took 12 ms a frame
// on a map where it took 7.5 ms after a restart, with the same GPU work and no game code
// recompiled, while 5.2 GB of textures from earlier maps were still cached (1.7 GB after the
// restart) (#63). A write to a page any texture was ever on takes the texture cache's lock (held
// by the GPU thread while it records draws), and a read of memory the GPU may have written waits
// for the GPU thread; both can grow with what is cached. Counters only rise; the report takes
// differences.
namespace FefwGuestWaits {

struct Counters {
    std::atomic<u64> texture_writes{0};    ///< writes that took the texture cache's lock
    std::atomic<u64> texture_write_wait_ns{0}; ///< waiting for that lock
    std::atomic<u64> texture_write_work_ns{0}; ///< under it
    std::atomic<u64> read_areas{0};        ///< reads that asked the caches what to copy back
    std::atomic<u64> read_area_ns{0};
    // Where that time goes (#82: a read took 8 us at the start of a session and 28-32 us after
    // 10-16 minutes): waiting for the buffer cache's lock, its record of the pages the CPU reads,
    // waiting for the texture cache's lock, its lookup, and the image entries that lookup walked.
    std::atomic<u64> read_buffer_wait_ns{0};
    std::atomic<u64> read_buffer_work_ns{0};
    std::atomic<u64> read_texture_wait_ns{0};
    std::atomic<u64> read_texture_work_ns{0};
    std::atomic<u64> read_texture_entries{0};
    std::atomic<u64> read_syncs{0};        ///< reads that waited for the GPU thread to copy back
    std::atomic<u64> read_sync_ns{0};
    std::atomic<s64> last_report_ms{0};
};

inline Counters& Get() {
    static Counters counters;
    return counters;
}

[[nodiscard]] inline u64 NowNs() {
    return static_cast<u64>(std::chrono::duration_cast<std::chrono::nanoseconds>(
                                std::chrono::steady_clock::now().time_since_epoch())
                                .count());
}

/// Whether 10 s have passed since the last report; true for exactly one caller each time.
[[nodiscard]] inline bool ReportDue() {
    auto& c = Get();
    const s64 now = static_cast<s64>(NowNs() / 1'000'000);
    s64 last = c.last_report_ms.load(std::memory_order_relaxed);
    if (last == 0) {
        c.last_report_ms.compare_exchange_strong(last, now);
        return false;
    }
    return now - last >= 10'000 && c.last_report_ms.compare_exchange_strong(last, now);
}

} // namespace FefwGuestWaits
