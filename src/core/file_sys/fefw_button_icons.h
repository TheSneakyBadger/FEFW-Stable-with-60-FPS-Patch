// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <array>
#include <functional>
#include <string>
#include <vector>

#include "common/common_types.h"
#include "core/file_sys/vfs/vfs_types.h"

// Fortune's Weave draws its button icons (A, B, ZL, +, the d-pad...) from Nintendo's button font,
// romfs Font/__nintendo_ext_003*.bffnt.zst, in three sizes. With Xbox or PlayStation button icons
// chosen, Fortune's Eden redraws those characters in the player's own copy as the game loads it:
// the face buttons show the controller button each is on (with the usual mapping, Xbox B on the
// right where the game shows A),
// the shoulder buttons get their names (LB, LT / L1, L2), + and - become Menu and View (Options
// and Create), and the d-pad characters become a d-pad. Nothing is written to disk.
namespace FileSys::FefwButtonIcons {

/// The keyboard shortcut for switching mouse mode (as Qt writes it, e.g. "Ctrl+Return"): the game's
/// SL + SR icons show it. Set by the frontend before a game starts.
void SetMouseModeKeys(std::string keys);

/// A place on the controller's face: right, bottom, top, left.
enum class Face : u8 { East, South, North, West };

/// Where the player has the game's A, B, X and Y on their controller (by default A on the right, B
/// at the bottom, X on top and Y on the left): the icons show that button. Set by the frontend
/// before a game starts.
void SetFaceButtons(std::array<Face, 4> a_b_x_y);

/// True when the game's button icons are to be changed.
[[nodiscard]] bool Active();

/// Reads the game's button fonts from `romfs` and calls `add` with each changed one (its romfs
/// folder, bytes and name). Returns how many were changed.
size_t Build(const VirtualDir& romfs,
             const std::function<void(const std::vector<std::string>& path, std::vector<u8> bytes,
                                      const std::string& name)>& add);

} // namespace FileSys::FefwButtonIcons
