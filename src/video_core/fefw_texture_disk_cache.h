// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <optional>
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

/// Reads the setting and indexes the folder (when the renderer starts). 0 bytes: off.
void Start();

/// Waits for queued writes (when the renderer stops).
void Stop();

[[nodiscard]] bool Enabled();

/// Whether there is an entry for this texture (it may still fail to load).
[[nodiscard]] bool Contains(const Key& key);

/// The stored data for this texture, exactly `size` bytes, if there is any. Any thread.
[[nodiscard]] std::optional<std::vector<u8>> Load(const Key& key, u64 size);

/// Stores the data (on the background thread).
void Store(const Key& key, std::vector<u8> data);

/// Textures loaded and stored since the last call (for the log).
struct Counts {
    u32 loaded;
    u32 stored;
    u64 bytes_on_disk;
};
[[nodiscard]] Counts TakeCounts();

} // namespace FefwTextureDiskCache
