// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "common/common_types.h"
#include "core/file_sys/vfs/vfs_types.h"

// Fortune's Weave keeps its graphics settings per map in MessagePack files under romfs
// Graphics/Settings (.set files, and Preset/lodPresets.lod for model detail). Fortune's Eden's
// optional performance settings change a few values in them as the game loads them: values are
// patched in place (same size), so nothing else in a file moves, and nothing is written to disk.
namespace FileSys::FefwRomfsTweaks {

inline constexpr u64 TitleId = 0x04007EF01F8AE000;

/// True when any of the settings differs from the game's own look.
[[nodiscard]] bool Any();

/// A directory with the changed files at their romfs paths, read from `romfs` (the game's romfs,
/// with any mods already layered). nullptr when nothing needs changing.
[[nodiscard]] VirtualDir Build(const VirtualDir& romfs);

} // namespace FileSys::FefwRomfsTweaks
