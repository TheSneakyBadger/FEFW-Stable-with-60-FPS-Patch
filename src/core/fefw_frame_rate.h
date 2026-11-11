// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <array>
#include <string>
#include <string_view>

#include "common/common_types.h"

namespace Core::Memory {
class Memory;
}

// Fortune's Eden's 60 FPS mode for Fire Emblem: Fortune's Weave 1.1.0.
//
// The game keeps a "timing rate" (frames per second it simulates for) in its application object:
// [main + 0x21E60D8] + 0x1DC, an int. Its own code sets it to 30. Every system that steps with the
// game's frame time reads it (60 / rate, as a float), and the presentation path uses it for the
// swap interval: refresh / rate, so 2 at 30 (main + 0x649924).
//
// The old 60 FPS cheat wrote a fixed 60 there, so whenever a PC fell below 60 FPS the whole game
// ran in slow motion (each frame still stepped 1/60 s). This mode instead sets the rate from the
// time between the game's real presents, every frame (30 to 60), and presents with interval 1, so
// game speed stays correct at any frame rate in between.
namespace FefwFrameRate {

/// main's build ID for 1.1.0 (the version the patch and these addresses were made for).
inline constexpr std::string_view MainBuildId = "DB93B0125F4850601D61C3EDDE012F8EB2ECFE77";

/// The game version the 60 FPS patch was applied to at this boot (PatchManager::PatchNSO): its
/// build ID (hex) and where it keeps the application object pointer and the patch's table
/// (main-relative). An empty build ID: the patch wasn't applied.
void SetFoundLayout(std::string_view build_id, u64 app_object, u64 table);

/// True for the version the patch was applied to at this boot.
[[nodiscard]] bool KnowsBuild(std::string_view build_id);

/// The main module was loaded at `base` (only kept for a build KnowsBuild).
void OnMainLoaded(const std::array<u8, 32>& build_id, u64 base);

/// Turned on when the "60fps" add-on is enabled for 1.1.0 (it replaces that add-on's cheat).
void SetEnabled(bool enabled);
[[nodiscard]] bool Enabled();

/// Called for every frame the game presents: measures the frame time and writes the rate.
void OnPresent(Core::Memory::Memory& memory);

} // namespace FefwFrameRate
