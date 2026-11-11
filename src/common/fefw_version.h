// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

namespace Common {

// One place for the version: the title is built from it, so the two can't disagree (v1.6.7.4's
// window title still said v1.6.7.3).
#define FEFW_VERSION_STRING "v1.6.8.1"

/// Fortune's Eden's release, shown in the window title and written at the top of the log.
inline constexpr const char* FefwVersion = FEFW_VERSION_STRING;

/// The window title, e.g. "Fortune's Eden v1.6.7.4".
inline constexpr const char* FefwTitle = "Fortune's Eden " FEFW_VERSION_STRING;

} // namespace Common
