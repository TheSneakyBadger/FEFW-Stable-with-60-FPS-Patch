// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <cstddef>

class InputProfiles;

namespace Core::HID {
class HIDCore;
}

// Fortune's Weave's mouse controls: the PC mouse is presented as a Joy-Con mouse while "Emulate
// mouse" (Settings::values.mouse_enabled) is on. Player 1 has two sets of controls, kept as the
// input profiles below: one for normal play and one for mouse play. Turning mouse emulation on
// or off swaps Player 1 to the matching set.
namespace FefwMouseMode {

inline constexpr const char* NormalProfile = "Fortunes Eden - normal controls";
inline constexpr const char* MouseProfile = "Fortunes Eden - mouse controls";

/// The bindings slot Player 1 uses: handheld (8) when Player 1 is a handheld controller, else 0.
[[nodiscard]] std::size_t BindingIndex(Core::HID::HIDCore& hid_core);

/// Stores Player 1's current bindings as the normal (mouse == false) or mouse set.
void StoreSet(InputProfiles& profiles, bool mouse, std::size_t index);

/// Loads the normal or mouse set into Player 1's bindings. False when that set has not been
/// stored yet (the current bindings are kept, so the set starts as a copy of them).
bool LoadSet(InputProfiles& profiles, bool mouse, std::size_t index);

/// Makes Player 1's emulated controllers use the bindings now in Settings.
void ReloadPlayer1(Core::HID::HIDCore& hid_core);

/// Turns mouse emulation on or off and swaps Player 1 to the matching set of controls.
void SetEnabled(bool enabled, Core::HID::HIDCore& hid_core);

} // namespace FefwMouseMode
