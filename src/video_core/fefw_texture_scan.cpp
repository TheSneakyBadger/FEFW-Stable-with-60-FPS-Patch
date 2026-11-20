// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include "video_core/fefw_texture_scan.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstring>
#include <fstream>
#include <map>
#include <mutex>
#include <optional>
#include <span>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

#include <fmt/format.h>

#include "common/cityhash.h"
#include "common/common_types.h"
#include "common/zstd_compression.h"
#include "video_core/fefw_texture_disk_cache.h"
#include "video_core/surface.h"
#include "video_core/texture_cache/image_info.h"
#include "video_core/texture_cache/util.h"
#include "video_core/textures/astc.h"
#include "video_core/textures/decoders.h"
#include "video_core/textures/texture.h"

namespace FefwTextureScan {
namespace {

namespace fs = std::filesystem;
using Tegra::Texture::ComponentType;
using Tegra::Texture::TextureFormat;
using Tegra::Texture::TextureType;
using Tegra::Texture::TICEntry;
using Tegra::Texture::TICHeaderVersion;
using VideoCommon::ImageInfo;
using VideoCommon::SwizzleParameters;

/// Decompressed files larger than this are skipped.
constexpr u64 MAX_FILE = 1ULL << 30;

/// A BNTX (texture container) in a buffer: bounds-checked reads relative to its start.
struct Reader {
    std::span<const u8> data;

    template <typename T>
    std::optional<T> Get(u64 offset) const {
        if (offset > data.size() || data.size() - offset < sizeof(T)) {
            return std::nullopt;
        }
        T value;
        std::memcpy(&value, data.data() + offset, sizeof(T));
        return value;
    }
};

/// BNTX texture formats (the high byte) to the GPU's ASTC formats.
std::optional<TextureFormat> AstcFormat(u32 type) {
    switch (type) {
    case 0x2d: return TextureFormat::ASTC_2D_4X4;
    case 0x2e: return TextureFormat::ASTC_2D_5X4;
    case 0x2f: return TextureFormat::ASTC_2D_5X5;
    case 0x30: return TextureFormat::ASTC_2D_6X5;
    case 0x31: return TextureFormat::ASTC_2D_6X6;
    case 0x32: return TextureFormat::ASTC_2D_8X5;
    case 0x33: return TextureFormat::ASTC_2D_8X6;
    case 0x34: return TextureFormat::ASTC_2D_8X8;
    case 0x35: return TextureFormat::ASTC_2D_10X5;
    case 0x36: return TextureFormat::ASTC_2D_10X6;
    case 0x37: return TextureFormat::ASTC_2D_10X8;
    case 0x38: return TextureFormat::ASTC_2D_10X10;
    case 0x39: return TextureFormat::ASTC_2D_12X10;
    case 0x3a: return TextureFormat::ASTC_2D_12X12;
    default: return std::nullopt;
    }
}

struct Totals {
    u64 files = 0;
    u64 bntx = 0;
    u64 textures = 0;
    u64 astc = 0;
    u64 skipped = 0;
};

/// Each ASTC texture of the BNTX at the start of `bntx`: its key by game texture and colour.
void ScanBntx(std::span<const u8> bntx, std::vector<std::pair<u64, u32>>& out, Totals& totals) {
    const Reader r{bntx};
    const auto count = r.Get<u32>(0x24);
    const auto array = r.Get<u64>(0x28);
    if (!count || !array || *count > 65536) {
        return;
    }
    ++totals.bntx;
    for (u32 i = 0; i < *count; ++i) {
        const auto info_offset = r.Get<u64>(*array + u64{i} * 8);
        if (!info_offset || !r.Get<u32>(*info_offset) ||
            std::memcmp(bntx.data() + *info_offset, "BRTI", 4) != 0) {
            continue;
        }
        const u64 o = *info_offset;
        const auto mips = r.Get<u16>(o + 0x16);
        const auto format = r.Get<u32>(o + 0x1C);
        const auto width = r.Get<u32>(o + 0x24);
        const auto height = r.Get<u32>(o + 0x28);
        const auto depth = r.Get<u32>(o + 0x2C);
        const auto array_length = r.Get<u32>(o + 0x30);
        const auto layout = r.Get<u32>(o + 0x34);
        const auto target = r.Get<u32>(o + 0x5C);
        const auto pointers = r.Get<u64>(o + 0x70);
        if (!mips || !format || !width || !height || !depth || !array_length || !layout ||
            !target || !pointers) {
            continue;
        }
        ++totals.textures;
        const std::optional<TextureFormat> astc = AstcFormat(*format >> 8);
        if (!astc) {
            continue;
        }
        const auto data_offset = r.Get<u64>(*pointers);
        if (!data_offset || *data_offset >= bntx.size() || *mips == 0 || *mips > 16 ||
            *width == 0 || *height == 0 || *width > 16384 || *height > 16384) {
            ++totals.skipped;
            continue;
        }
        // The GPU texture header the game builds from it (NVN targets: 1 2D, 2 3D, 4 2D array,
        // 8 cube, 9 cube array).
        TICEntry tic{};
        tic.format.Assign(*astc);
        tic.r_type.Assign(ComponentType::UNORM);
        tic.g_type.Assign(ComponentType::UNORM);
        tic.b_type.Assign(ComponentType::UNORM);
        tic.a_type.Assign(ComponentType::UNORM);
        tic.header_version.Assign(TICHeaderVersion::BlockLinear);
        tic.block_height.Assign(*layout & 7);
        tic.max_mip_level.Assign(*mips - 1U);
        tic.width_minus_one.Assign(*width - 1);
        tic.height_minus_1.Assign(*height - 1);
        tic.srgb_conversion.Assign((*format & 0xFF) == 6 ? 1 : 0);
        TextureType type = TextureType::Texture2D;
        u32 tic_depth = 1;
        switch (*target) {
        case 1: break;
        case 2: type = TextureType::Texture3D; tic_depth = *depth; break;
        case 4: type = TextureType::Texture2DArray; tic_depth = *array_length; break;
        case 8: type = TextureType::TextureCubemap; break;
        case 9: type = TextureType::TextureCubeArray; tic_depth = *array_length / 6; break;
        default: ++totals.skipped; continue;
        }
        if (tic_depth == 0 || tic_depth > 2048) {
            ++totals.skipped;
            continue;
        }
        tic.texture_type.Assign(type);
        tic.depth_minus_1.Assign(tic_depth - 1);
        const ImageInfo info{tic};
        const u64 available = bntx.size() - *data_offset;
        const u32 guest_size = VideoCommon::CalculateGuestSizeInBytes(info);
        if (guest_size > available) {
            ++totals.skipped;
            continue;
        }
        const u8* const data = bntx.data() + *data_offset;
        // As the texture cache keys it (TextureCacheRuntime's DiskCacheKey, by game texture).
        u64 hash = Common::CityHash64(reinterpret_cast<const char*>(data), guest_size);
        const auto swizzles = VideoCommon::FullUploadSwizzles(info);
        for (const SwizzleParameters& swizzle : swizzles) {
            const std::array<u64, 8> fields{
                swizzle.num_tiles.width, swizzle.num_tiles.height, swizzle.num_tiles.depth,
                swizzle.block.width,     swizzle.block.height,     swizzle.block.depth,
                swizzle.buffer_offset,   static_cast<u64>(static_cast<s64>(swizzle.level)),
            };
            hash = Common::CityHash64WithSeed(reinterpret_cast<const char*>(fields.data()),
                                              sizeof(fields), hash);
        }
        const FefwTextureDiskCache::Key key{
            .data_hash = hash,
            .format = static_cast<u32>(info.format),
            .width = info.size.width,
            .height = info.size.height,
            .levels = static_cast<u32>(info.resources.levels),
            .layers = static_cast<u32>(info.resources.layers),
            .encoding = 0,
        };
        // The colour: the smallest level (its first layer) decoded and averaged, alpha 255 where
        // every texel is opaque (as the texture cache works it out from what it stores).
        const SwizzleParameters& smallest = swizzles.back();
        const u32 level_width = (std::max)(info.size.width >> smallest.level, 1U);
        const u32 level_height = (std::max)(info.size.height >> smallest.level, 1U);
        if (smallest.buffer_offset >= available) {
            ++totals.skipped;
            continue;
        }
        std::vector<u8> blocks(size_t{smallest.num_tiles.width} * smallest.num_tiles.height * 16);
        Tegra::Texture::UnswizzleTexture(
            blocks,
            std::span<const u8>(data + smallest.buffer_offset, available - smallest.buffer_offset),
            16, smallest.num_tiles.width, smallest.num_tiles.height, 1, smallest.block.height,
            smallest.block.depth);
        std::vector<u8> rgba(size_t{level_width} * level_height * 4);
        Tegra::Texture::ASTC::Decompress(blocks, level_width, level_height, 1,
                                         VideoCore::Surface::DefaultBlockWidth(info.format),
                                         VideoCore::Surface::DefaultBlockHeight(info.format),
                                         rgba);
        u64 sum[3]{};
        bool opaque = true;
        const u64 texels = u64{level_width} * level_height;
        for (u64 t = 0; t < texels; ++t) {
            sum[0] += rgba[t * 4];
            sum[1] += rgba[t * 4 + 1];
            sum[2] += rgba[t * 4 + 2];
            opaque = opaque && rgba[t * 4 + 3] >= 250;
        }
        const u32 color = static_cast<u32>(sum[0] / texels) |
                          (static_cast<u32>(sum[1] / texels) << 8) |
                          (static_cast<u32>(sum[2] / texels) << 16) | (opaque ? 0xFF000000U : 0);
        out.emplace_back(FefwTextureDiskCache::GuestName(key), color);
        ++totals.astc;
    }
}

/// The file's contents, decompressed if it is a .zst.
std::vector<u8> ReadFile(const fs::path& path) {
    std::error_code ec;
    const u64 size = fs::file_size(path, ec);
    if (ec || size == 0 || size > MAX_FILE) {
        return {};
    }
    std::vector<u8> bytes(size);
    std::ifstream file{path, std::ios::binary};
    if (!file.read(reinterpret_cast<char*>(bytes.data()), static_cast<std::streamsize>(size))) {
        return {};
    }
    if (path.extension() != ".zst") {
        return bytes;
    }
    // Only zstd frames that give their size, up to the limit.
    if (bytes.size() < 4 || bytes[0] != 0x28 || bytes[1] != 0xB5 || bytes[2] != 0x2F ||
        bytes[3] != 0xFD) {
        return {};
    }
    return Common::Compression::DecompressDataZSTD(bytes);
}

} // Anonymous namespace

int Run(const fs::path& romfs, const fs::path& output) {
    const auto start = std::chrono::steady_clock::now();
    std::vector<fs::path> files;
    std::error_code ec;
    for (auto it = fs::recursive_directory_iterator{romfs, ec};
         !ec && it != fs::recursive_directory_iterator{}; it.increment(ec)) {
        if (it->is_regular_file(ec)) {
            files.push_back(it->path());
        }
    }
    std::atomic<size_t> next{0};
    std::mutex mutex;
    std::unordered_map<u64, u32> colors;
    std::map<std::string, u64> by_folder;
    Totals totals;
    const auto work = [&] {
        std::vector<std::pair<u64, u32>> found;
        Totals mine;
        while (true) {
            const size_t index = next++;
            if (index >= files.size()) {
                break;
            }
            const std::vector<u8> bytes = ReadFile(files[index]);
            ++mine.files;
            found.clear();
            // BNTX at the start, and any inside other files (aligned to 16 bytes).
            for (size_t offset = 0; offset + 0x40 <= bytes.size(); offset += 16) {
                if (std::memcmp(bytes.data() + offset, "BNTX", 4) == 0) {
                    ScanBntx(std::span<const u8>(bytes).subspan(offset), found, mine);
                }
            }
            std::scoped_lock lock{mutex};
            const fs::path relative = files[index].lexically_relative(romfs);
            const std::string folder =
                relative.begin() != relative.end() ? relative.begin()->string() : "";
            for (const auto& [name, color] : found) {
                if (colors.try_emplace(name, color).second) {
                    ++by_folder[folder];
                }
            }
        }
        std::scoped_lock lock{mutex};
        totals.files += mine.files;
        totals.bntx += mine.bntx;
        totals.textures += mine.textures;
        totals.astc += mine.astc;
        totals.skipped += mine.skipped;
    };
    std::vector<std::thread> threads;
    const unsigned count = (std::max)(std::thread::hardware_concurrency() / 2, 1U);
    for (unsigned i = 0; i < count; ++i) {
        threads.emplace_back(work);
    }
    for (std::thread& thread : threads) {
        thread.join();
    }
    std::vector<std::pair<u64, u32>> sorted(colors.begin(), colors.end());
    std::ranges::sort(sorted);
    {
        std::ofstream file{output, std::ios::binary | std::ios::trunc};
        const u32 head[3]{0x43474546, 1, static_cast<u32>(sorted.size())}; // "FEGC", version 1
        file.write(reinterpret_cast<const char*>(head), sizeof(head));
        for (const auto& [name, color] : sorted) {
            file.write(reinterpret_cast<const char*>(&name), sizeof(name));
            file.write(reinterpret_cast<const char*>(&color), sizeof(color));
        }
        if (!file.good()) {
            return 1;
        }
    }
    fs::path summary_path = output;
    summary_path += ".txt";
    std::ofstream summary{summary_path, std::ios::trunc};
    summary << fmt::format(
        "{} files read in {} s; {} BNTX containers, {} textures, {} of them ASTC ({} skipped); "
        "{} different texture colours written to {}\n",
        totals.files,
        std::chrono::duration_cast<std::chrono::seconds>(std::chrono::steady_clock::now() - start)
            .count(),
        totals.bntx, totals.textures, totals.astc, totals.skipped, sorted.size(),
        output.filename().string());
    for (const auto& [folder, n] : by_folder) {
        summary << fmt::format("  {}: {}\n", folder, n);
    }
    return 0;
}

} // namespace FefwTextureScan
