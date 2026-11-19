// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <string>

namespace FefwExternalCaps {

/// Frame rate caps set outside Fortune's Eden that hold its frames back: RivaTuner Statistics
/// Server's (while RTSS is running) and the NVIDIA driver's Max Frame Rate (the NVIDIA app or
/// Control Panel, for Fortune's Eden or for all programs). 0: none found.
struct Caps {
    float rtss = 0.0f;
    float nvidia = 0.0f;
    /// What was found, for the log ("RTSS: 120 (global profile)", "NVIDIA: none").
    std::string rtss_detail;
    std::string nvidia_detail;
};

/// Reads them now (a few milliseconds up to about 0.1 s for the NVIDIA driver's settings), and
/// passes them to Settings for frame generation's pacing. Windows only; none elsewhere.
Caps Detect();

} // namespace FefwExternalCaps
