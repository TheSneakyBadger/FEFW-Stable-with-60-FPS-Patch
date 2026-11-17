// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <vector>

#include "common/common_types.h"

class QWidget;

/// Fortune's Eden: NVIDIA DLDSR (or AMD VSR) in fullscreen. Once DLDSR is turned on in the NVIDIA app
/// or Control Panel, the driver offers resolutions above the monitor's own; in fullscreen the
/// monitor showing the game is switched to one of them (temporarily: Windows puts the desktop
/// back if Fortune's Eden closes), and back when leaving fullscreen or switching to another
/// window. Windows only: elsewhere nothing is offered.
namespace FefwDldsr {

/// The factors (the pixel count times 100) offered on the monitor showing `window`: DLDSR's 178
/// and 225 on NVIDIA cards, every VSR factor the driver offers on AMD cards.
[[nodiscard]] std::vector<u32> AvailableFactors(QWidget* window);

/// Whether the monitor showing `window` is on an AMD card (Virtual Super Resolution).
[[nodiscard]] bool UsesAmdVsr(QWidget* window);

/// Switches the monitor showing `window` (fullscreen) to the factor's resolution and fits the
/// window to it. Nothing when the factor is 0 or the monitor doesn't offer it.
void Apply(QWidget* window, u32 factor, bool borderless);

/// Puts the monitor back to its desktop resolution.
void Restore();

/// Whether the resolution was changed in the last moment (a switch can briefly move the focus).
[[nodiscard]] bool RecentlySwitched();

} // namespace FefwDldsr
