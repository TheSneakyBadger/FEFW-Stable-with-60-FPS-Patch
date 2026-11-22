// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <atomic>
#include <cstdint>

/// Fortune's Eden: what the JIT compiled, for the frame pacing log (#63: hitches the first time a
/// unit is moved). Counters only rise; readers take differences.
namespace Dynarmic::FefwJitStats {

struct Counters {
    std::atomic<std::uint64_t> blocks{0};          ///< blocks compiled
    std::atomic<std::uint64_t> compile_ns{0};      ///< time compiling them (translate, optimise, emit)
    std::atomic<std::uint64_t> cache_clears{0};    ///< the whole code cache thrown away
    std::atomic<std::uint64_t> range_clears{0};    ///< ranges of compiled code thrown away
};

inline Counters& Get() {
    static Counters counters;
    return counters;
}

/// Test 94b: told about every compiled A64 block (host code range and the location descriptor,
/// whose low 56 bits are the guest PC), so a sampler can map host addresses back to game code.
using BlockHook = void (*)(const void* begin, const void* end, std::uint64_t location);

inline std::atomic<BlockHook>& BlockRegistered() {
    static std::atomic<BlockHook> hook{nullptr};
    return hook;
}

} // namespace Dynarmic::FefwJitStats
