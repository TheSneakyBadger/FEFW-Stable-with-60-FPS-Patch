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

/// Fortune's Eden: room the loader adds after the last module (rtld, main, subsdks, sdk) when 60
/// FPS mode is on (loader/deconstructed_rom_directory.cpp), for when a mod uses the free space the
/// patch usually takes: the first page for the routines, the second for the table. Read-write (an
/// executable page looked like another module to the game's rtld). After every module, not right
/// after main, so no module moves: mods reach other modules at fixed distances from main (#45).
inline constexpr u64 ReserveBytes = 0x2000;

/// That room while main is patched: where it starts from main's start (0 when there is none) and
/// its bytes, which the loader maps there once every module is loaded. Set by the loader around
/// PatchManager::PatchNSO for main.
struct Room {
    u64 offset = 0;
    std::span<u8> bytes;
};
inline thread_local Room room{};

/// The room for the whole load: its address in the process (0 when there is none) and its bytes.
/// Set by the loader for its loading pass; nso.cpp turns it into `room` while main is patched.
struct LoaderRoom {
    u64 address = 0;
    std::span<u8> bytes;
};
inline thread_local LoaderRoom loader_room{};

struct Layout {
    u64 app_object; ///< main-relative: the pointer to the application object (the rate is at +0x1DC)
    u64 table;      ///< main-relative: the table the emulator writes each frame
    u64 routines;   ///< main-relative: where the routines were placed
    bool reserved;  ///< the routines or the table are in the loader's added room
};

/// `image` is main as loaded (no NSO header; .text at 0, .text `text_size` bytes long). Patches
/// it and returns the layout, or leaves it untouched and says why in `why`. `reserved`: where the
/// loader's added room starts, from main's start (0 when there is none); `room`: its bytes
/// (ReserveBytes), written when the routines go there. `prefer_room`: put them in that room even
/// where the game's own space is free (a test of it).
[[nodiscard]] std::optional<Layout> Apply(std::span<u8> image, u32 text_size, std::string& why,
                                          u64 reserved = 0, std::span<u8> room = {},
                                          bool prefer_room = false);

/// Anchors found at 1.1.0's own addresses (a check of the search on 1.1.0; nothing is patched).
[[nodiscard]] bool MatchesReference(std::span<const u8> image, u32 text_size, std::string& why);

} // namespace FefwSixtySearch
