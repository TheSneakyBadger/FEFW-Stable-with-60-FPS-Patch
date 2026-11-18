// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <atomic>

/// Fortune's Eden: NVIDIA Reflex's sleep at the game's own vsync, as native games do it at the
/// start of their game loop (#63). The renderer sets the hook once NVIDIA DLSS frame generation
/// is loaded; the vsync thread calls it before each vsync it signals. It returns whether it paced
/// that vsync (frame generation making frames); while it does, Reflex's frame limit is the only
/// pacer and the vsync timer doesn't also hold the game to the frame generation rate.
namespace FefwFrameGenHook {

inline std::atomic<bool (*)()> vsync_sleep{nullptr};

} // namespace FefwFrameGenHook
