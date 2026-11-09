// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <optional>
#include <string>

#include "common/common_types.h"

class QWidget;

/// Fortune's Eden's own updater: looks up the newest release of
/// TheSneakyBadger/FEFW-Stable-with-60-FPS-Patch on GitHub when the emulator opens, asks before
/// doing anything, downloads the portable zip, checks its SHA-256 against the one GitHub lists,
/// and replaces the program files after the emulator closes. The user folder (saves, settings,
/// keys, mods, shader cache) is never touched.
namespace FefwUpdater {

struct Release {
    std::string tag;       ///< "v1.6.4"
    std::string name;      ///< Release title
    std::string html_url;  ///< Release page
    std::string asset_url; ///< The portable zip's download address
    std::string sha256;    ///< Hex, from the asset's digest
    u64 asset_size{};
};

/// Network lookup (run off the UI thread). A release newer than this build, unless it is the one
/// the player chose to skip; nothing when offline or on any error.
std::optional<Release> CheckForUpdate();

/// Asks the player (Update now / Later / Skip this version). On "Update now": downloads and checks
/// the zip, starts the helper that installs it once the emulator has closed, and returns true:
/// the caller then closes the main window.
bool OfferUpdate(QWidget* parent, const Release& release);

} // namespace FefwUpdater
