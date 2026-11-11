// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

namespace Kernel {
class KThread;
} // namespace Kernel

// Fortune's Eden: where the game's own CPU time goes. Every 30 s, the log lists the game's busiest
// threads (their share of one core, name, priority and core) and each emulated core's load, so
// player logs show which game threads limit the frame rate on slower CPUs.
namespace FefwCpuStats {

/// Call on each presented frame with the game's presenting (rendering) thread.
void MaybeReport(const Kernel::KThread* render_thread);

} // namespace FefwCpuStats
