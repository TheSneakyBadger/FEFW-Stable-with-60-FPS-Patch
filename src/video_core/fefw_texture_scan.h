// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <filesystem>

/// Fortune's Eden: the texture colours for the list that comes with it (texture-colours), worked
/// out from the game's own files instead of collected in play. Goes through a folder of the
/// game's extracted files (romfs; .zst files decompressed in memory) for its BNTX textures, and
/// for each ASTC one works out the key the texture cache gives it once the game has loaded it
/// (its bytes and layout, as a GPU texture header would describe them) and the average colour of
/// its smallest level. Writes only those pairs (colours.bin) and a summary (.txt beside it):
/// none of the game's data. Reads only; started with --scan-textures ROMFS OUT_FILE.
namespace FefwTextureScan {

/// Returns the process exit code (0: written).
int Run(const std::filesystem::path& romfs, const std::filesystem::path& output);

} // namespace FefwTextureScan
