// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include "core/arm/dynarmic/fefw_jit_warm.h"

#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <system_error>
#include <unordered_set>

#include <fmt/format.h>

#include "common/fs/fs_util.h"
#include "common/fs/path_util.h"
#include "common/hex_util.h"
#include "common/logging.h"

namespace FefwJitWarm {
namespace {

namespace fs = std::filesystem;

constexpr u32 MAGIC = 0x574A4546; // "FEJW"
constexpr u32 VERSION = 1;
/// Per core, about 12 MiB on disk; a long session compiles a few hundred thousand.
constexpr std::size_t MAX_ENTRIES = 1'500'000;
/// Dynarmic's A64 location: the PC in the low 56 bits, the rest flags (FPCR, single-stepping).
constexpr u64 PC_MASK = (u64{1} << 56) - 1;

struct Header {
    u32 magic;
    u32 version;
    u64 count;
};

fs::path PathFor(const std::array<u8, 0x20>& build_id, std::size_t core) {
    const std::string id = Common::HexToString(build_id, true).substr(0, 32);
    return Common::FS::GetEdenPath(Common::FS::EdenPath::CacheDir) / "jit" /
           fmt::format("{}_core{}.bin", id, core);
}

/// The saved entries: PCs relative to the start of the game's code.
std::vector<u64> ReadRelative(const fs::path& path) {
    std::ifstream file{path, std::ios::binary};
    Header header{};
    if (!file.read(reinterpret_cast<char*>(&header), sizeof(header)) || header.magic != MAGIC ||
        header.version != VERSION || header.count > MAX_ENTRIES) {
        return {};
    }
    std::vector<u64> entries(static_cast<std::size_t>(header.count));
    if (!file.read(reinterpret_cast<char*>(entries.data()),
                   static_cast<std::streamsize>(entries.size() * sizeof(u64)))) {
        return {};
    }
    return entries;
}

bool ValidBuild(const std::array<u8, 0x20>& build_id) {
    return std::any_of(build_id.begin(), build_id.end(), [](u8 b) { return b != 0; });
}

} // Anonymous namespace

bool Enabled() {
    static const bool enabled = [] {
        const char* const value = std::getenv("EDEN_JIT_WARM");
        return !(value && value[0] == '0');
    }();
    return enabled;
}

std::vector<u64> Load(const std::array<u8, 0x20>& build_id, std::size_t core, u64 base,
                      u64 code_size) {
    if (!Enabled() || !ValidBuild(build_id)) {
        return {};
    }
    std::vector<u64> entries = ReadRelative(PathFor(build_id, core));
    std::size_t kept = 0;
    for (const u64 entry : entries) {
        const u64 offset = entry & PC_MASK;
        if (offset < code_size) {
            entries[kept++] = (entry & ~PC_MASK) | ((base + offset) & PC_MASK);
        }
    }
    entries.resize(kept);
    return entries;
}

void Save(const std::array<u8, 0x20>& build_id, std::size_t core, u64 base, u64 code_size,
          const std::vector<u64>& compiled) {
    if (!Enabled() || !ValidBuild(build_id) || compiled.empty()) {
        return;
    }
    const fs::path path = PathFor(build_id, core);
    std::vector<u64> entries = ReadRelative(path);
    std::unordered_set<u64> seen(entries.begin(), entries.end());
    std::size_t added = 0;
    for (const u64 value : compiled) {
        const u64 pc = value & PC_MASK;
        if (pc < base || pc - base >= code_size) {
            continue;
        }
        const u64 relative = (value & ~PC_MASK) | (pc - base);
        if (seen.insert(relative).second) {
            entries.push_back(relative);
            ++added;
        }
    }
    if (added == 0) {
        return;
    }
    if (entries.size() > MAX_ENTRIES) {
        entries.erase(entries.begin(),
                      entries.begin() + static_cast<std::ptrdiff_t>(entries.size() - MAX_ENTRIES));
    }
    std::error_code ec;
    fs::create_directories(path.parent_path(), ec);
    const fs::path temporary = fs::path{path}.concat(".tmp");
    {
        std::ofstream file{temporary, std::ios::binary | std::ios::trunc};
        const Header header{MAGIC, VERSION, entries.size()};
        if (!file.write(reinterpret_cast<const char*>(&header), sizeof(header)) ||
            !file.write(reinterpret_cast<const char*>(entries.data()),
                        static_cast<std::streamsize>(entries.size() * sizeof(u64)))) {
            LOG_WARNING(Core_ARM, "JIT warm-up: couldn't save the list for core {}", core);
            return;
        }
    }
    fs::rename(temporary, path, ec);
    if (ec) {
        LOG_WARNING(Core_ARM, "JIT warm-up: couldn't save the list for core {}: {}", core,
                    ec.message());
        return;
    }
    LOG_INFO(Core_ARM, "JIT warm-up: core {} list saved, {} blocks ({} new this session)", core,
             entries.size(), added);
}

} // namespace FefwJitWarm
