// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <optional>
#include <span>
#include <string>

#include "common/common_types.h"

// The 60 FPS game patch for Fortune's Weave versions other than 1.1.0. The patch was made for
// 1.1.0's addresses; here every place it touches (and the code and data its routines use) is
// found again in the loaded game by the instructions around it (fefw_60fps_search_data.h, made by
// tools/fefw/make_60fps_patch.py). Each must be found exactly once and every patched site must
// hold the exact instruction it holds in 1.1.0, or nothing is changed.
namespace FefwSixtySearch {

/// Fortune's Eden: room the NSO loader adds after main when 60 FPS mode is on (loader/nso.cpp),
/// for when a mod uses the free space the patch usually takes: the first page for the routines,
/// the second for the table. Read-write, part of main's data region to the game (an executable
/// page there looked like another module to its rtld). Zero until the patch uses it.
inline constexpr u64 ReserveBytes = 0x2000;

/// Where that room starts in the main image being patched (no NSO header), or 0 when there is
/// none. Set by the loader around PatchManager::PatchNSO.
inline thread_local u64 reserved_offset = 0;

struct Layout {
    u64 app_object; ///< main-relative: the pointer to the application object (the rate is at +0x1DC)
    u64 table;      ///< main-relative: the table the emulator writes each frame
    u64 routines;   ///< main-relative: where the routines were placed
    bool reserved;  ///< the routines or the table are in the loader's added room
};

/// `image` is main as loaded (no NSO header; .text at 0, .text `text_size` bytes long). Patches
/// it and returns the layout, or leaves it untouched and says why in `why`. `reserved`: where the
/// loader's added room starts in `image` (ReserveBytes long), or 0 when there is none.
/// `prefer_room`: put them in that room even where the game's own space is free (a test of it).
[[nodiscard]] std::optional<Layout> Apply(std::span<u8> image, u32 text_size, std::string& why,
                                          u64 reserved = 0, bool prefer_room = false);

/// Anchors found at 1.1.0's own addresses (a check of the search on 1.1.0; nothing is patched).
[[nodiscard]] bool MatchesReference(std::span<const u8> image, u32 text_size, std::string& why);

} // namespace FefwSixtySearch
