// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

// SPDX-FileCopyrightText: Copyright 2018 yuzu Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "common/assert.h"
#include "common/common_types.h"
#include "common/logging.h"
#include "common/uuid.h"
#include "core/core.h"
#include "core/file_sys/savedata_factory.h"
#include "core/file_sys/vfs/vfs.h"

namespace FileSys {

namespace {

bool ShouldSaveDataBeAutomaticallyCreated(SaveDataSpaceId space, const SaveDataAttribute& attr) {
    return attr.type == SaveDataType::Cache || attr.type == SaveDataType::Temporary ||
           (space == SaveDataSpaceId::User && ///< Normal Save Data -- Current Title & User
            (attr.type == SaveDataType::Account || attr.type == SaveDataType::Device) &&
            attr.program_id == 0 && attr.system_save_data_id == 0);
}

std::string GetFutureSaveDataPath(SaveDataSpaceId space_id, SaveDataType type, u64 title_id,
                                  u128 user_id) {
    // Only detect nand user saves.
    const auto space_id_path = [space_id]() -> std::string_view {
        switch (space_id) {
        case SaveDataSpaceId::User:
            return "/user/save";
        default:
            return "";
        }
    }();

    if (space_id_path.empty()) {
        return "";
    }

    Common::UUID uuid;
    std::memcpy(uuid.uuid.data(), user_id.data(), sizeof(Common::UUID));

    // Only detect account/device saves from the future location.
    switch (type) {
    case SaveDataType::Account:
        return fmt::format("{}/account/{}/{:016X}/0", space_id_path, uuid.RawString(), title_id);
    case SaveDataType::Device:
        return fmt::format("{}/device/{:016X}/0", space_id_path, title_id);
    default:
        return "";
    }
}

} // Anonymous namespace

namespace {

// A save folder with no files except the save-size file, and no subfolders.
bool IsEmptySave(const VirtualDir& save) {
    if (!save->GetSubdirectories().empty()) {
        return false;
    }
    for (const auto& file : save->GetFiles()) {
        if (file->GetName() != GetSaveDataSizeFileName()) {
            return false;
        }
    }
    return true;
}

// Loose-file (exefs) dumps used to register no program ID, so their account and device saves went
// to title 0000000000000000. They now use the game's real ID. The first time that ID has no save
// but title 0 does, copy the old save across; the old folder is left for older builds.
void MigrateTitleZeroSave(const VirtualDir& dir, ProgramId program_id, SaveDataSpaceId space,
                          const SaveDataAttribute& meta) {
    if (meta.type != SaveDataType::Account && meta.type != SaveDataType::Device) {
        return;
    }
    const u64 title_id = meta.program_id != 0 ? meta.program_id : program_id;
    if (title_id == 0 || title_id != program_id) {
        return;
    }
    const auto new_path = SaveDataFactory::GetFullPath(program_id, dir, space, meta.type, title_id,
                                                       meta.user_id, meta.system_save_data_id);
    // A folder holding nothing but the save-size file counts as missing: EnsureSaveData,
    // GetSaveDataSize and "Open Save Data Location" create the folder (through program ID 0)
    // before the game opens its save, which used to stop the old save from ever being copied.
    const auto existing = dir->GetDirectoryRelative(new_path);
    if (existing != nullptr && !IsEmptySave(existing)) {
        return;
    }
    const auto old_path = SaveDataFactory::GetFullPath(0, dir, space, meta.type, 0, meta.user_id,
                                                       meta.system_save_data_id);
    const auto old_dir = dir->GetDirectoryRelative(old_path);
    if (old_dir == nullptr || IsEmptySave(old_dir)) {
        return;
    }
    // Copy into a sibling folder and rename it into place only when the copy is complete, so a
    // copy that fails part way (disk full, a file in use) never becomes the save.
    const auto slash = new_path.find_last_of('/');
    const std::string parent_path = slash == std::string::npos ? std::string{} : new_path.substr(0, slash);
    const std::string leaf = slash == std::string::npos ? new_path : new_path.substr(slash + 1);
    const std::string temp_name = leaf + ".migrating";
    const auto parent_dir = parent_path.empty() ? dir : dir->CreateDirectoryRelative(parent_path);
    if (parent_dir == nullptr) {
        LOG_ERROR(Service_FS, "Could not open {} to copy the loose-file save {}", parent_path, old_path);
        return;
    }
    parent_dir->DeleteSubdirectoryRecursive(temp_name);
    const auto temp_dir = parent_dir->CreateSubdirectory(temp_name);
    if (temp_dir == nullptr || !VfsRawCopyD(old_dir, temp_dir)) {
        parent_dir->DeleteSubdirectoryRecursive(temp_name);
        LOG_ERROR(Service_FS, "Could not copy the loose-file save {} to {}", old_path, new_path);
        return;
    }
    // Keep the size the empty folder was given if the old save never had one.
    if (existing != nullptr && old_dir->GetFile(GetSaveDataSizeFileName()) == nullptr) {
        if (const auto size_file = existing->GetFile(GetSaveDataSizeFileName())) {
            if (const auto copy = temp_dir->CreateFile(GetSaveDataSizeFileName())) {
                VfsRawCopy(size_file, copy);
            }
        }
    }
    if (existing != nullptr && !parent_dir->DeleteSubdirectoryRecursive(leaf)) {
        parent_dir->DeleteSubdirectoryRecursive(temp_name);
        LOG_ERROR(Service_FS, "Could not replace the empty save folder {}", new_path);
        return;
    }
    if (!temp_dir->Rename(leaf)) {
        LOG_ERROR(Service_FS, "Could not move the copied save into {} (it is in {})", new_path,
                  temp_name);
        return;
    }
    LOG_INFO(Service_FS, "Copied the loose-file save {} to {} (the old copy is kept)", old_path,
             new_path);
}

} // Anonymous namespace

SaveDataFactory::SaveDataFactory(Core::System& system_, ProgramId program_id_,
                                 VirtualDir save_directory_)
    : system{system_}, program_id{program_id_}, dir{std::move(save_directory_)} {}

SaveDataFactory::~SaveDataFactory() = default;

VirtualDir SaveDataFactory::Create(SaveDataSpaceId space, const SaveDataAttribute& meta) const {
    MigrateTitleZeroSave(dir, program_id, space, meta);
    const auto save_directory = GetFullPath(program_id, dir, space, meta.type, meta.program_id,
                                            meta.user_id, meta.system_save_data_id);

    return dir->CreateDirectoryRelative(save_directory);
}

VirtualDir SaveDataFactory::Open(SaveDataSpaceId space, const SaveDataAttribute& meta) const {
    MigrateTitleZeroSave(dir, program_id, space, meta);
    const auto save_directory = GetFullPath(program_id, dir, space, meta.type, meta.program_id,
                                            meta.user_id, meta.system_save_data_id);

    auto out = dir->GetDirectoryRelative(save_directory);

    if (out == nullptr && (ShouldSaveDataBeAutomaticallyCreated(space, meta) && auto_create)) {
        return Create(space, meta);
    }

    return out;
}

VirtualDir SaveDataFactory::GetSaveDataSpaceDirectory(SaveDataSpaceId space) const {
    return dir->GetDirectoryRelative(GetSaveDataSpaceIdPath(space));
}

std::string SaveDataFactory::GetSaveDataSpaceIdPath(SaveDataSpaceId space) {
    switch (space) {
    case SaveDataSpaceId::System:
        return "/system/";
    case SaveDataSpaceId::User:
    case SaveDataSpaceId::SdUser:
        return "/user/";
    case SaveDataSpaceId::Temporary:
        return "/temp/";
    default:
        ASSERT_MSG(false, "Unrecognized SaveDataSpaceId: {:02X}", static_cast<u8>(space));
        return "/unrecognized/"; ///< To prevent corruption when ignoring asserts.
    }
}

std::string SaveDataFactory::GetFullPath(ProgramId program_id, VirtualDir dir,
                                         SaveDataSpaceId space, SaveDataType type, u64 title_id,
                                         u128 user_id, u64 save_id) {
    // According to switchbrew, if a save is of type SaveData and the title id field is 0, it should
    // be interpreted as the title id of the current process.
    if (type == SaveDataType::Account || type == SaveDataType::Device) {
        if (title_id == 0) {
            title_id = program_id;
        }
    }

    // For compat with a future impl.
    if (std::string future_path =
            GetFutureSaveDataPath(space, type, title_id & ~(0xFFULL), user_id);
        !future_path.empty()) {
        // Check if this location exists, and prefer it over the old.
        if (const auto future_dir = dir->GetDirectoryRelative(future_path); future_dir != nullptr) {
            LOG_INFO(Service_FS, "Using save at new location: {}", future_path);
            return future_path;
        }
    }

    std::string out = GetSaveDataSpaceIdPath(space);

    switch (type) {
    case SaveDataType::System:
        return fmt::format("{}save/{:016X}/{:016X}{:016X}", out, save_id, user_id[1], user_id[0]);
    case SaveDataType::Account:
    case SaveDataType::Device:
        return fmt::format("{}save/{:016X}/{:016X}{:016X}/{:016X}", out, 0, user_id[1], user_id[0],
                           title_id);
    case SaveDataType::Temporary:
        return fmt::format("{}{:016X}/{:016X}{:016X}/{:016X}", out, 0, user_id[1], user_id[0],
                           title_id);
    case SaveDataType::Cache:
        return fmt::format("{}save/cache/{:016X}", out, title_id);
    default:
        ASSERT_MSG(false, "Unrecognized SaveDataType: {:02X}", static_cast<u8>(type));
        return fmt::format("{}save/unknown_{:X}/{:016X}", out, static_cast<u8>(type), title_id);
    }
}

std::string SaveDataFactory::GetUserGameSaveDataRoot(u128 user_id, bool future) {
    if (future) {
        Common::UUID uuid;
        std::memcpy(uuid.uuid.data(), user_id.data(), sizeof(Common::UUID));
        return fmt::format("/user/save/account/{}", uuid.RawString());
    }
    return fmt::format("/user/save/{:016X}/{:016X}{:016X}", 0, user_id[1], user_id[0]);
}

SaveDataSize SaveDataFactory::ReadSaveDataSize(SaveDataType type, u64 title_id,
                                               u128 user_id) const {
    const auto path =
        GetFullPath(program_id, dir, SaveDataSpaceId::User, type, title_id, user_id, 0);
    const auto relative_dir = GetOrCreateDirectoryRelative(dir, path);

    const auto size_file = relative_dir->GetFile(GetSaveDataSizeFileName());
    if (size_file == nullptr || size_file->GetSize() < sizeof(SaveDataSize)) {
        return {0, 0};
    }

    SaveDataSize out;
    if (size_file->ReadObject(&out) != sizeof(SaveDataSize)) {
        return {0, 0};
    }

    return out;
}

void SaveDataFactory::WriteSaveDataSize(SaveDataType type, u64 title_id, u128 user_id,
                                        SaveDataSize new_value) const {
    const auto path =
        GetFullPath(program_id, dir, SaveDataSpaceId::User, type, title_id, user_id, 0);
    const auto relative_dir = GetOrCreateDirectoryRelative(dir, path);

    const auto size_file = relative_dir->CreateFile(GetSaveDataSizeFileName());
    if (size_file == nullptr) {
        return;
    }

    size_file->Resize(sizeof(SaveDataSize));
    size_file->WriteObject(new_value);
}

void SaveDataFactory::SetAutoCreate(bool state) {
    auto_create = state;
}

} // namespace FileSys
