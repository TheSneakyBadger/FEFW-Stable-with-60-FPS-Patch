// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <filesystem>
#include <optional>
#include <span>
#include <vector>

#include "common/common_types.h"

/// Fortune's Eden: converted ASTC textures (BCn, as the GPU encoded them) kept on disk in
/// user/cache/textures, so a texture seen before is loaded instead of decoded and encoded again
/// (the stutter of an area visited before, #63). Writes go to a background thread; the oldest
/// entries are dropped once the cache is over its size (the texture_disk_cache setting).
namespace FefwTextureDiskCache {

struct Key {
    u64 data_hash;  ///< The guest (swizzled ASTC) bytes and the swizzle parameters.
    u32 format;     ///< The guest pixel format (block size, sRGB).
    u32 width;
    u32 height;
    u32 levels;
    u32 layers;
    u32 encoding;   ///< What the GPU encoded to (1: BC1, 3: BC3, 7: BC7).
};

/// Whether the folder is on a hard drive (a spinning disk), where the cache is off: its reads and
/// writes would slow the game's own. Unknown (a network share, no answer): false.
[[nodiscard]] bool OnHardDrive(const std::filesystem::path& folder);

/// Reads the setting and indexes the folder (when the renderer starts). 0 bytes: off.
void Start();

/// Waits for queued writes (when the renderer stops).
void Stop();

[[nodiscard]] bool Enabled();

/// Whether there is an entry for this texture (it may still fail to load).
[[nodiscard]] bool Contains(const Key& key);

/// Reads the stored data for this texture into output (exactly its size), if there is any. Any
/// thread.
[[nodiscard]] bool LoadInto(const Key& key, std::span<u8> output);

/// Stores the data (on the background thread).
void Store(const Key& key, std::vector<u8> data);

/// Textures loaded and stored since the last call (for the log).
struct Counts {
    u32 loaded;
    u32 stored;
    u64 bytes_on_disk;
    u64 read_ns;     ///< Time the loads took on their threads, in all.
    u64 read_max_ns; ///< The longest load.
};
[[nodiscard]] Counts TakeCounts();

} // namespace FefwTextureDiskCache
