// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <optional>
#include <span>
#include <string_view>
#include <vector>

#include "common/common_types.h"
#include "core/file_sys/vfs/vfs_types.h"

// Fortune's Weave's title logo ("FIRE EMBLEM / Fortune's Weave"), for Fortune's Eden's home
// screen: read from the player's own copy of the game (romfs Layout/Textures/Common.arc.zst, the
// texture Title_TitleLogo_<language>), never shipped with Fortune's Eden.
namespace FileSys::FefwTitleLogo {

struct Image {
    u32 width{};
    u32 height{};
    std::vector<u8> rgba; ///< width * height pixels, 4 bytes each, straight alpha
};

/// The logo in the game's language code (USen, JPja, KRko, CNzh or TWzh; USen when that one isn't
/// there), from the decompressed Common.arc. Nothing when it isn't the expected kind.
[[nodiscard]] std::optional<Image> Decode(std::span<const u8> arc, std::string_view language);

/// The logo from the game's romfs.
[[nodiscard]] std::optional<Image> Read(const VirtualDir& romfs, std::string_view language);

} // namespace FileSys::FefwTitleLogo
