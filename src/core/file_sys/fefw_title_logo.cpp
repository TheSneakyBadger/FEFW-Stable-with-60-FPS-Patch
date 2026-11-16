// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include "core/file_sys/fefw_title_logo.h"

#include <algorithm>
#include <array>
#include <cstring>
#include <string>

#include "common/zstd_compression.h"
#include "core/file_sys/vfs/vfs.h"

namespace FileSys::FefwTitleLogo {
namespace {

template <typename T>
bool ReadAt(std::span<const u8> data, size_t offset, T& out) {
    if (offset > data.size() || data.size() - offset < sizeof(T)) {
        return false;
    }
    std::memcpy(&out, data.data() + offset, sizeof(T));
    return true;
}

size_t Find(std::span<const u8> data, std::string_view magic, size_t from = 0) {
    if (data.size() < magic.size()) {
        return std::string_view::npos;
    }
    for (size_t i = from; i + magic.size() <= data.size(); ++i) {
        if (std::memcmp(data.data() + i, magic.data(), magic.size()) == 0) {
            return i;
        }
    }
    return std::string_view::npos;
}

/// One BC3 block (alpha, then colour) into a 4x4 RGBA tile.
void DecodeBlock(const u8* block, std::array<u8, 64>& out) {
    std::array<u8, 8> alpha{block[0], block[1]};
    if (alpha[0] > alpha[1]) {
        for (int i = 1; i < 7; ++i) {
            alpha[i + 1] = static_cast<u8>(((7 - i) * alpha[0] + i * alpha[1]) / 7);
        }
    } else {
        for (int i = 1; i < 5; ++i) {
            alpha[i + 1] = static_cast<u8>(((5 - i) * alpha[0] + i * alpha[1]) / 5);
        }
        alpha[6] = 0;
        alpha[7] = 255;
    }
    u64 alpha_bits = 0;
    for (int i = 0; i < 6; ++i) {
        alpha_bits |= static_cast<u64>(block[2 + i]) << (8 * i);
    }
    const u16 c0 = static_cast<u16>(block[8] | (block[9] << 8));
    const u16 c1 = static_cast<u16>(block[10] | (block[11] << 8));
    const auto rgb = [](u16 c) {
        return std::array<int, 3>{((c >> 11) & 31) * 255 / 31, ((c >> 5) & 63) * 255 / 63,
                                  (c & 31) * 255 / 31};
    };
    std::array<std::array<int, 3>, 4> colours{rgb(c0), rgb(c1)};
    for (int k = 0; k < 3; ++k) {
        colours[2][k] = (2 * colours[0][k] + colours[1][k]) / 3;
        colours[3][k] = (colours[0][k] + 2 * colours[1][k]) / 3;
    }
    const u32 colour_bits = static_cast<u32>(block[12] | (block[13] << 8) | (block[14] << 16) |
                                             (static_cast<u32>(block[15]) << 24));
    for (int p = 0; p < 16; ++p) {
        const auto& colour = colours[(colour_bits >> (2 * p)) & 3];
        out[p * 4 + 0] = static_cast<u8>(colour[0]);
        out[p * 4 + 1] = static_cast<u8>(colour[1]);
        out[p * 4 + 2] = static_cast<u8>(colour[2]);
        out[p * 4 + 3] = alpha[(alpha_bits >> (3 * p)) & 7];
    }
}

/// Where byte x of block row y is in a block-linear (GOB) texture `width_bytes` wide.
size_t SwizzledOffset(u32 x, u32 y, u32 width_bytes, u32 block_height) {
    const u32 gobs_x = (width_bytes + 63) / 64;
    const size_t base = (static_cast<size_t>(y / (8 * block_height)) * gobs_x + x / 64) * 512 *
                            block_height +
                        static_cast<size_t>((y % (8 * block_height)) / 8) * 512;
    const u32 gob = (x % 16) + (y & 1) * 16 + ((y >> 1) & 1) * 32 + ((x >> 4) & 1) * 64 +
                    ((y >> 2) & 1) * 128 + ((x >> 5) & 1) * 256;
    return base + gob;
}

} // Anonymous namespace

std::optional<Image> Decode(std::span<const u8> arc, std::string_view language) {
    const size_t bntx = Find(arc, "BNTX");
    if (bntx == std::string_view::npos) {
        return std::nullopt;
    }
    const auto texture = arc.subspan(bntx);
    const std::string wanted = "Title_TitleLogo_" + std::string{language};
    const std::string fallback = "Title_TitleLogo_USen";
    size_t found = std::string_view::npos;
    for (size_t at = Find(texture, "BRTI"); at != std::string_view::npos;
         at = Find(texture, "BRTI", at + 4)) {
        u64 name_offset{};
        u16 name_size{};
        if (!ReadAt(texture, at + 0x60, name_offset) ||
            !ReadAt(texture, static_cast<size_t>(name_offset), name_size) ||
            name_offset + 2 + name_size > texture.size()) {
            continue;
        }
        const std::string_view name{
            reinterpret_cast<const char*>(texture.data() + name_offset + 2), name_size};
        if (name.starts_with(wanted)) {
            found = at;
            break;
        }
        if (name.starts_with(fallback) && found == std::string_view::npos) {
            found = at;
        }
    }
    if (found == std::string_view::npos) {
        return std::nullopt;
    }
    u32 format{}, width{}, height{}, block_height_log2{}, size{};
    u64 pointers{}, data_offset{};
    if (!ReadAt(texture, found + 0x1C, format) || !ReadAt(texture, found + 0x24, width) ||
        !ReadAt(texture, found + 0x28, height) ||
        !ReadAt(texture, found + 0x34, block_height_log2) ||
        !ReadAt(texture, found + 0x50, size) || !ReadAt(texture, found + 0x70, pointers) ||
        !ReadAt(texture, static_cast<size_t>(pointers), data_offset)) {
        return std::nullopt;
    }
    // BC3 (0x1C, unorm or sRGB), at most 4096 pixels across.
    if ((format >> 8) != 0x1C || width == 0 || height == 0 || width > 4096 || height > 4096 ||
        block_height_log2 > 5 || data_offset > texture.size() ||
        texture.size() - data_offset < size) {
        return std::nullopt;
    }
    const auto data = texture.subspan(static_cast<size_t>(data_offset), size);
    const u32 blocks_x = (width + 3) / 4, blocks_y = (height + 3) / 4;
    const u32 width_bytes = blocks_x * 16;
    const u32 block_height = 1u << block_height_log2;

    Image image{width, height, std::vector<u8>(static_cast<size_t>(width) * height * 4)};
    std::array<u8, 64> tile{};
    for (u32 by = 0; by < blocks_y; ++by) {
        for (u32 bx = 0; bx < blocks_x; ++bx) {
            const size_t at = SwizzledOffset(bx * 16, by, width_bytes, block_height);
            if (at + 16 > data.size()) {
                return std::nullopt;
            }
            // The 16 bytes of a block are together in a GOB (16-byte aligned).
            DecodeBlock(data.data() + at, tile);
            for (u32 py = 0; py < 4; ++py) {
                const u32 y = by * 4 + py;
                for (u32 px = 0; px < 4 && y < height; ++px) {
                    const u32 x = bx * 4 + px;
                    if (x < width) {
                        std::memcpy(&image.rgba[(static_cast<size_t>(y) * width + x) * 4],
                                    &tile[(py * 4 + px) * 4], 4);
                    }
                }
            }
        }
    }
    return image;
}

std::optional<Image> Read(const VirtualDir& romfs, std::string_view language) {
    if (romfs == nullptr) {
        return std::nullopt;
    }
    const auto layout = romfs->GetSubdirectory("Layout");
    const auto textures = layout ? layout->GetSubdirectory("Textures") : nullptr;
    const auto file = textures ? textures->GetFile("Common.arc.zst") : nullptr;
    if (file == nullptr) {
        return std::nullopt;
    }
    const auto packed = file->ReadAllBytes();
    const auto arc = Common::Compression::DecompressDataZSTD(packed);
    if (arc.empty()) {
        return std::nullopt;
    }
    return Decode(arc, language);
}

} // namespace FileSys::FefwTitleLogo
