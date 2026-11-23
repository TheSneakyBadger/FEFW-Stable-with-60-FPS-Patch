// SPDX-FileCopyrightText: Copyright 2020 yuzu Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include "common/common_types.h"

namespace Common {

struct MemoryInfo {
    u64 TotalPhysicalMemory{};
    u64 TotalSwapMemory{};
};

/**
 * Gets the memory info of the host system
 * @return Reference to a MemoryInfo struct with the physical and swap memory sizes in bytes
 */
[[nodiscard]] const MemoryInfo& GetMemInfo();

// Fortune's Eden: the system's memory right now, for logs when an allocation fails (#86).
// Zero where the platform doesn't say.
struct MemoryStatus {
    u64 available_physical{};
    u64 commit_limit{};     // RAM plus page file
    u64 commit_available{}; // what can still be committed before the limit
};
[[nodiscard]] MemoryStatus GetMemoryStatus();

} // namespace Common
