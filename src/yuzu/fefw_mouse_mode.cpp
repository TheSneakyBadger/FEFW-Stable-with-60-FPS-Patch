// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include "yuzu/fefw_mouse_mode.h"

#include <string>

#include "common/logging.h"
#include "common/settings.h"
#include "hid_core/frontend/emulated_controller.h"
#include "hid_core/hid_core.h"
#include "hid_core/hid_types.h"
#include "yuzu/configuration/input_profiles.h"

namespace FefwMouseMode {
namespace {
constexpr std::size_t HandheldIndex = 8;

std::string ProfileName(bool mouse) {
    return mouse ? MouseProfile : NormalProfile;
}
} // Anonymous namespace

std::size_t BindingIndex(Core::HID::HIDCore& hid_core) {
    const auto* player1 = hid_core.GetEmulatedController(Core::HID::NpadIdType::Player1);
    return player1->GetNpadStyleIndex() == Core::HID::NpadStyleIndex::Handheld ? HandheldIndex
                                                                                 : 0;
}

void StoreSet(InputProfiles& profiles, bool mouse, std::size_t index) {
    const auto name = ProfileName(mouse);
    if (!profiles.SaveProfile(name, index) && !profiles.CreateProfile(name, index)) {
        LOG_ERROR(Frontend, "Could not store the {} controls", mouse ? "mouse" : "normal");
    }
}

bool LoadSet(InputProfiles& profiles, bool mouse, std::size_t index) {
    // Loading writes the profile's name into the player; keep the player's own.
    auto& player = Settings::values.players.GetValue()[index];
    const auto profile_name = player.profile_name;
    const bool loaded = profiles.LoadProfile(ProfileName(mouse), index);
    player.profile_name = profile_name;
    return loaded;
}

void ReloadPlayer1(Core::HID::HIDCore& hid_core) {
    hid_core.GetEmulatedController(Core::HID::NpadIdType::Player1)->ReloadFromSettings();
    hid_core.GetEmulatedController(Core::HID::NpadIdType::Handheld)->ReloadFromSettings();
}

void SetEnabled(bool enabled, Core::HID::HIDCore& hid_core) {
    const bool was_enabled = Settings::values.mouse_enabled.GetValue();
    if (was_enabled == enabled) {
        return;
    }
    InputProfiles profiles;
    const auto index = BindingIndex(hid_core);
    StoreSet(profiles, was_enabled, index);
    if (!LoadSet(profiles, enabled, index)) {
        // First switch: the new set starts as a copy of the current controls.
        StoreSet(profiles, enabled, index);
    }
    Settings::values.mouse_enabled.SetValue(enabled);
    ReloadPlayer1(hid_core);
    LOG_INFO(Frontend, "Mouse emulation {}; Player 1 uses the {} controls",
             enabled ? "on" : "off", enabled ? "mouse" : "normal");
}

} // namespace FefwMouseMode
