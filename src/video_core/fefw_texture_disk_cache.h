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

/// How many threads read from the cache: all but 4 of the CPU's threads, 2 to 16; 1 with 4 or fewer fast
/// cores (a laptop's), where more took time from the game's own threads (#75).
/// EDEN_DISK_READERS overrides it (1 to 16).
[[nodiscard]] u32 ReadThreads();

/// The video memory Fortune's Eden may use below which the cache is off (#75): a 4 GB card (about
/// 2.9 GiB usable) can't hold a scene's textures, frees and reloads thousands a minute, and
/// reading them back from disk fell behind converting them on the GPU (frames waited 4.7 s in
/// 30 s). EDEN_TEXTURE_CACHE_SMALL_CARD=1 keeps it on.
inline constexpr u64 SMALL_CARD_MEMORY = 4ULL << 30;

/// Reads the setting and indexes the folder (when the renderer starts), given the video memory
/// Fortune's Eden may use. 0 bytes: off.
void Start(u64 video_memory);

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

/// The texture's stand-in: its average colour (RGBA8, R in the low byte; alpha 0 where the
/// texture has see-through parts, 255 otherwise), shown while it is read from disk instead of
/// holding up the frame. Known once it has been stored or loaded (kept in standins.bin). Any
/// thread. EDEN_TC_STAND_IN=0: none.
[[nodiscard]] std::optional<u32> StandInColor(const Key& key);

/// The texture's colour by the game's texture alone (whatever it was encoded to): for the
/// stand-in while a texture the cache doesn't have yet waits to be converted. From this install
/// or the list that comes with Fortune's Eden. Any thread.
[[nodiscard]] std::optional<u32> GuestColor(const Key& key);

/// One compressed block of a single colour (rgba as StandInColor), for this encoding (1: BC1,
/// 8 bytes; 3: BC3 and 7: BC7, 16 bytes). Returns the block's size.
u32 SolidBlock(u32 encoding, u32 rgba, std::span<u8, 16> block);

/// Textures loaded and stored since the last call (for the log).
struct Counts {
    u32 loaded;
    u32 stored;
    u64 bytes_on_disk;
    u64 read_ns;     ///< Time the loads took on their threads, in all.
    u64 read_max_ns; ///< The longest load.
    u32 stand_ins;   ///< Textures with a stand-in colour known.
};
[[nodiscard]] Counts TakeCounts();

} // namespace FefwTextureDiskCache
