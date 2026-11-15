// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include "yuzu/fefw_mouse_mode.h"

#include <string>

#include "common/logging.h"
#include "common/settings.h"
#include "hid_core/frontend/emulated_controller.h"
#include "hid_core/hid_core.h"
#include "hid_core/hid_types.h"
#include "input_common/main.h"
#include "qt_common/config/qt_config.h"
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

/// Whether any of the player's buttons or sticks is on the keyboard.
bool HasKeyboard(const Settings::PlayerInput& player) {
    const auto on_keyboard = [](const std::string& param) {
        return param.find("engine:keyboard") != std::string::npos;
    };
    for (const auto& button : player.buttons) {
        if (on_keyboard(button)) {
            return true;
        }
    }
    for (const auto& analog : player.analogs) {
        if (on_keyboard(analog)) {
            return true;
        }
    }
    return false;
}

/// The default keyboard controls (WASD for the left stick, and so on), as Restore Defaults sets.
void SetKeyboardDefaults(Settings::PlayerInput& player) {
    for (int i = 0; i < Settings::NativeButton::NumButtons; ++i) {
        player.buttons[i] = InputCommon::GenerateKeyboardParam(QtConfig::default_buttons[i]);
    }
    for (int i = 0; i < Settings::NativeAnalog::NumAnalogs; ++i) {
        const auto& keys = QtConfig::default_analogs[i];
        player.analogs[i] = InputCommon::GenerateAnalogParamFromKeys(
            keys[0], keys[1], keys[2], keys[3], QtConfig::default_stick_mod[i], 0.5f);
    }
    for (int i = 0; i < Settings::NativeMotion::NumMotions; ++i) {
        player.motions[i] = InputCommon::GenerateKeyboardParam(QtConfig::default_motions[i]);
    }
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
    // Mouse play is mouse and keyboard: a mouse set without any keyboard controls (copied from a
    // controller's) gets the default keyboard controls, so WASD walks.
    if (enabled) {
        auto& player = Settings::values.players.GetValue()[index];
        if (!HasKeyboard(player)) {
            SetKeyboardDefaults(player);
            StoreSet(profiles, true, index);
            LOG_INFO(Frontend, "Mouse controls had no keyboard keys; set to the default keyboard "
                               "controls (WASD)");
        }
    }
    Settings::values.mouse_enabled.SetValue(enabled);
    ReloadPlayer1(hid_core);
    LOG_INFO(Frontend, "Mouse emulation {}; Player 1 uses the {} controls",
             enabled ? "on" : "off", enabled ? "mouse" : "normal");
}

} // namespace FefwMouseMode
