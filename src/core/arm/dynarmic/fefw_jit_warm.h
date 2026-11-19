// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <array>
#include <cstddef>
#include <vector>

#include "common/common_types.h"

/// Fortune's Eden: JIT warm-up. The emulator compiles the game's code to the PC's the first time
/// it runs, each session: entering an area for the first time in a session compiled 20,000-45,000
/// blocks, 200-460 ms in all, much of it in frames over 50 ms (#63). Which blocks each CPU core
/// compiled is kept in user/cache/jit (their addresses, relative to the game's code, not the
/// compiled code), and the next session compiles them before the core first runs, so they are
/// ready when the game reaches them. EDEN_JIT_WARM=0 turns it off.
namespace FefwJitWarm {

[[nodiscard]] bool Enabled();

/// The block locations (Dynarmic A64 location hashes) saved for this game build and core, made
/// absolute for code loaded at base (only those within code_size of it).
[[nodiscard]] std::vector<u64> Load(const std::array<u8, 0x20>& build_id, std::size_t core,
                                    u64 base, u64 code_size);

/// Adds the locations compiled this session to the saved ones (oldest dropped past the limit).
void Save(const std::array<u8, 0x20>& build_id, std::size_t core, u64 base, u64 code_size,
          const std::vector<u64>& compiled);

} // namespace FefwJitWarm
