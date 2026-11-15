// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "common/common_types.h"

namespace Tegra::Texture::BCN {

/// Encodes a 4x4 block of RGBA8 texels (row-major, 64 bytes) as a 16-byte BC7 block.
void EncodeBc7Block(u8* output, const u8* input);

} // namespace Tegra::Texture::BCN
