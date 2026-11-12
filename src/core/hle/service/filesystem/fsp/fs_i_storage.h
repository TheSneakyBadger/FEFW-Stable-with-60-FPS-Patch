// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

// SPDX-FileCopyrightText: Copyright 2023 yuzu Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <memory>
#include <utility>
#include <vector>

#include "core/file_sys/vfs/vfs.h"
#include "core/hle/service/cmif_types.h"
#include "core/hle/service/filesystem/filesystem.h"
#include "core/hle/service/service.h"

namespace Service::FileSystem {

class IStorage final : public ServiceFramework<IStorage> {
public:
    /// track_scene_loads: the game's own RomFS, whose map data reads mark its loading screens.
    explicit IStorage(Core::System& system_, FileSys::VirtualFile backend_,
                      bool track_scene_loads = false);

private:
    FileSys::VirtualFile backend;
    /// Sorted, merged byte ranges of the RomFS's per-scene map files (empty: not tracked).
    std::shared_ptr<const std::vector<std::pair<u64, u64>>> map_ranges;

    Result Read(
        OutBuffer<BufferAttr_HipcMapAlias | BufferAttr_HipcMapTransferAllowsNonSecure> out_bytes,
        s64 offset, s64 length);
    Result GetSize(Out<u64> out_size);
};

} // namespace Service::FileSystem
