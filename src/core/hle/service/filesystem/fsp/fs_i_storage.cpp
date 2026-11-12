// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

// SPDX-FileCopyrightText: Copyright 2023 yuzu Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <algorithm>
#include <array>
#include <cstring>
#include <mutex>
#include <string_view>
#include <unordered_map>

#include "common/alignment.h"
#include "common/fefw_scene_load.h"
#include "common/literals.h"
#include "common/logging.h"
#include "core/file_sys/errors.h"
#include "core/hle/service/cmif_serialization.h"
#include "core/hle/service/filesystem/fsp/fs_i_storage.h"

namespace Service::FileSystem {

using namespace Common::Literals;

namespace {

// The byte ranges of a RomFS image's files under Map/Btl, Hub, Dng, Gmp, Bqt and Ptd (each map's
// own data, read while its loading screen shows), sorted and merged. Empty if it isn't a RomFS.
std::vector<std::pair<u64, u64>> FindMapRanges(const FileSys::VirtualFile& file) {
    std::vector<std::pair<u64, u64>> ranges;
    std::array<u64, 10> header{};
    if (file == nullptr || file->ReadObject(&header) != sizeof(header) || header[0] != 0x50) {
        return ranges;
    }
    const u64 dir_meta_offset = header[3], dir_meta_size = header[4];
    const u64 file_meta_offset = header[7], file_meta_size = header[8];
    const u64 data_offset = header[9];
    if (dir_meta_size > 64_MiB || file_meta_size > 64_MiB) {
        return ranges;
    }
    std::vector<u8> dirs(dir_meta_size);
    std::vector<u8> files(file_meta_size);
    if (file->Read(dirs.data(), dirs.size(), dir_meta_offset) != dirs.size() ||
        file->Read(files.data(), files.size(), file_meta_offset) != files.size()) {
        return ranges;
    }
    const auto u32_at = [](const std::vector<u8>& v, size_t at) {
        u32 value{};
        std::memcpy(&value, v.data() + at, sizeof(value));
        return value;
    };
    // Directory entry: parent, sibling, child dir, child file, hash, name size, name.
    const auto DirName = [&](u32 offset) -> std::string_view {
        if (offset + 24 > dirs.size()) {
            return {};
        }
        const u32 size = u32_at(dirs, offset + 20);
        if (offset + 24 + size > dirs.size()) {
            return {};
        }
        return {reinterpret_cast<const char*>(dirs.data() + offset + 24), size};
    };
    const auto DirParent = [&](u32 offset) {
        return offset + 4 <= dirs.size() ? u32_at(dirs, offset) : 0U;
    };
    // Whether a directory is (in) a per-scene map folder, memoised.
    std::unordered_map<u32, bool> scene_dirs;
    const auto IsSceneDir = [&](u32 offset) {
        std::vector<u32> chain;
        bool result = false;
        for (u32 at = offset; at != 0 && chain.size() < 64; at = DirParent(at)) {
            if (const auto it = scene_dirs.find(at); it != scene_dirs.end()) {
                result = it->second;
                break;
            }
            const u32 parent = DirParent(at);
            const u32 grandparent = DirParent(parent);
            const std::string_view name = DirName(at);
            if (parent != 0 && grandparent == 0 && DirName(parent) == "Map" &&
                (name == "Btl" || name == "Hub" || name == "Dng" || name == "Gmp" ||
                 name == "Bqt" || name == "Ptd")) {
                result = true;
                chain.push_back(at);
                break;
            }
            chain.push_back(at);
        }
        for (const u32 at : chain) {
            scene_dirs[at] = result;
        }
        return result;
    };
    // File entry: parent, sibling, offset (u64), size (u64), hash, name size, name.
    for (size_t at = 0; at + 32 <= files.size();) {
        const u32 parent = u32_at(files, at);
        u64 offset{}, size{};
        std::memcpy(&offset, files.data() + at + 8, sizeof(offset));
        std::memcpy(&size, files.data() + at + 16, sizeof(size));
        const u32 name_size = u32_at(files, at + 28);
        if (size != 0 && IsSceneDir(parent)) {
            ranges.emplace_back(data_offset + offset, data_offset + offset + size);
        }
        at += 32 + Common::AlignUp(name_size, 4);
    }
    std::sort(ranges.begin(), ranges.end());
    std::vector<std::pair<u64, u64>> merged;
    for (const auto& range : ranges) {
        if (!merged.empty() && range.first <= merged.back().second) {
            merged.back().second = (std::max)(merged.back().second, range.second);
        } else {
            merged.push_back(range);
        }
    }
    return merged;
}

} // Anonymous namespace

IStorage::IStorage(Core::System& system_, FileSys::VirtualFile backend_, bool track_scene_loads)
    : ServiceFramework{system_, "IStorage"}, backend(std::move(backend_)) {
    if (track_scene_loads) {
        // Parsed once per RomFS image (the game opens its RomFS again for each file system).
        static std::mutex cache_mutex;
        static std::weak_ptr<FileSys::VfsFile> cached_file;
        static std::shared_ptr<const std::vector<std::pair<u64, u64>>> cached_ranges;
        std::scoped_lock lock{cache_mutex};
        if (cached_file.lock() != backend || cached_ranges == nullptr) {
            cached_ranges =
                std::make_shared<const std::vector<std::pair<u64, u64>>>(FindMapRanges(backend));
            cached_file = backend;
            LOG_INFO(Service_FS, "Scene loads: {} map data ranges in the game's RomFS",
                     cached_ranges->size());
        }
        map_ranges = cached_ranges;
    }
    static const FunctionInfo functions[] = {
        {0, D<&IStorage::Read>, "Read"},
        {1, nullptr, "Write"},
        {2, nullptr, "Flush"},
        {3, nullptr, "SetSize"},
        {4, D<&IStorage::GetSize>, "GetSize"},
        {5, nullptr, "OperateRange"},
    };
    RegisterHandlers(functions);
}

Result IStorage::Read(
    OutBuffer<BufferAttr_HipcMapAlias | BufferAttr_HipcMapTransferAllowsNonSecure> out_bytes,
    s64 offset, s64 length) {
    LOG_DEBUG(Service_FS, "called, offset={:#x}, length={}", offset, length);

    R_UNLESS(length >= 0, FileSys::ResultInvalidSize);
    R_UNLESS(offset >= 0, FileSys::ResultInvalidOffset);

    // Read the data from the Storage backend
    backend->Read(out_bytes.data(), length, offset);

    if (map_ranges != nullptr && !map_ranges->empty() && length > 0) {
        const u64 begin = static_cast<u64>(offset);
        const u64 end = begin + static_cast<u64>(length);
        auto it = std::upper_bound(map_ranges->begin(), map_ranges->end(),
                                   std::pair<u64, u64>{begin, ~0ULL});
        if (it != map_ranges->begin()) {
            --it;
        }
        u64 bytes = 0;
        for (; it != map_ranges->end() && it->first < end; ++it) {
            const u64 lo = (std::max)(begin, it->first);
            const u64 hi = (std::min)(end, it->second);
            if (hi > lo) {
                bytes += hi - lo;
            }
        }
        if (bytes != 0) {
            Common::FefwSceneLoad::map_bytes_read.fetch_add(bytes, std::memory_order_relaxed);
        }
    }

    R_SUCCEED();
}

Result IStorage::GetSize(Out<u64> out_size) {
    *out_size = backend->GetSize();

    LOG_DEBUG(Service_FS, "called, size={}", *out_size);

    R_SUCCEED();
}

} // namespace Service::FileSystem
