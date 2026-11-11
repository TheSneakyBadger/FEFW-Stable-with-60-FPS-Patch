// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <atomic>

#include "common/common_types.h"

// What the emulator did between two presents, for the frame pacing log: when a frame takes over
// 100 ms (loading in, re-entering an area), it says whether pipelines, shader translations or
// texture re-compression were part of it. Counters only rise; readers take differences.
namespace FefwLoadStats {

struct Counters {
    std::atomic<u64> graphics_pipelines{0}; ///< built at run time (not from the disk cache at boot)
    std::atomic<u64> translations{0};       ///< of those, needing new shaders (not reused ones)
    std::atomic<u64> compute_pipelines{0};
    std::atomic<u64> draws_dropped{0};      ///< draws skipped while their pipeline was building
    std::atomic<u64> astc_textures{0};      ///< ASTC textures re-compressed on the GPU
    std::atomic<u64> astc_waits{0};         ///< times that waited for the GPU (memory cap)
};

inline Counters& Get() {
    static Counters counters;
    return counters;
}

inline void Add(std::atomic<u64>& counter, u64 amount = 1) {
    counter.fetch_add(amount, std::memory_order_relaxed);
}

} // namespace FefwLoadStats
