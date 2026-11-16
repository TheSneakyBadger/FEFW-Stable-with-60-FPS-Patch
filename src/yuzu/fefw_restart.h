// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <QStringList>

/// A game with NVIDIA DLSS starts in a new Fortune's Eden (Streamline can't start again in this
/// one): asked for as this one closes, done once it has closed.
namespace FefwRestart {

/// Starts a new Fortune's Eden with these arguments once this one has closed.
void Request(const QStringList& arguments);

/// After the main window has closed: starts the new one, if asked for.
void RunIfRequested(int exit_code);

/// At the start of main: when started by RunIfRequested, waits (up to 20 s) for the previous
/// one to have gone.
void WaitForPrevious();

} // namespace FefwRestart
