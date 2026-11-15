// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <atomic>

#include "common/common_types.h"

// Fortune's Eden: how much of the game's per-scene map data (romfs Map/Btl, Hub, Dng, Gmp, Bqt,
// Ptd) it has read. A burst of it is a loading screen: the texture cache then frees what the
// previous scene left behind (see TextureCache::TickFrame).
namespace Common::FefwSceneLoad {

inline std::atomic<u64> map_bytes_read{0};

// Whether the scene on screen is a battle map (its map data, under romfs Map/Btl, is what the game
// read last). Mouse mode's wheel zooms only there (see FefwMouseInput::WheelStickY).
inline std::atomic<bool> in_battle_map{false};

} // namespace Common::FefwSceneLoad
