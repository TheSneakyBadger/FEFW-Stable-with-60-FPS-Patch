// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <functional>
#include <string>
#include <vector>

#include "common/common_types.h"
#include "core/file_sys/vfs/vfs_types.h"

// Fortune's Weave draws its button icons (A, B, ZL, +, the d-pad...) from Nintendo's button font,
// romfs Font/__nintendo_ext_003*.bffnt.zst, in three sizes. With Xbox or PlayStation button icons
// chosen, Fortune's Eden redraws those characters in the player's own copy as the game loads it:
// the face buttons follow the button's position (Xbox A, at the bottom, where the game shows B),
// the shoulder buttons get their names (LB, LT / L1, L2), + and - become Menu and View (Options
// and Create), and the d-pad characters become a d-pad. Nothing is written to disk.
namespace FileSys::FefwButtonIcons {

/// True when the game's button icons are to be changed.
[[nodiscard]] bool Active();

/// Reads the game's button fonts from `romfs` and calls `add` with each changed one (its romfs
/// folder, bytes and name). Returns how many were changed.
size_t Build(const VirtualDir& romfs,
             const std::function<void(const std::vector<std::string>& path, std::vector<u8> bytes,
                                      const std::string& name)>& add);

} // namespace FileSys::FefwButtonIcons
