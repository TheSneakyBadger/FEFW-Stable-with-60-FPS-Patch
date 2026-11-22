// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <cstddef>

// Fortune's Eden test 93w: what the host CPU executes while the game's main (rendering) thread
// runs. About 500 times a second the emulated core running that thread is paused for a moment and
// its instruction pointer read: inside the game code the JIT compiled, inside Fortune's Eden.exe
// (by address, for the build's map file) or inside another DLL. Every 30 s the log lists the
// shares. In heavy battles the main thread needs twice the time per frame with nothing maxed out
// (#63, #82); this says whose code that time is spent in. Windows only; EDEN_SAMPLER=0: off.
namespace FefwSampler {

/// On each emulated core's host thread as it starts.
void RegisterCoreThread(std::size_t core);

/// On each guest thread switch, from that core's host thread.
void NoteCurrentThread(const void* thread);

/// The game's presenting thread (from the wait statistics).
void SetRenderThread(const void* thread);

/// Test 94g: the GPU emulation's draw thread, from its first draw. It is sampled too, and where
/// in Fortune's Eden.exe its time goes is written to log/draw-thread-N.txt every 20 s.
void RegisterDrawThread();

} // namespace FefwSampler
