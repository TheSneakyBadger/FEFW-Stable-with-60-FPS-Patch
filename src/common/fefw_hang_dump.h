// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <string>

namespace Common::FefwHangDump {

/// Testing: when EDEN_HANG_DUMP_DIR names a folder, writes a small dump of every thread's stack
/// (no memory beyond the stacks) there, once per run, and returns its path; otherwise empty.
/// Windows only.
std::string Write(const char* reason);

} // namespace Common::FefwHangDump
