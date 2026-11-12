// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

// SPDX-FileCopyrightText: Copyright 2018 yuzu Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <algorithm>
#include <cstring>
#include <optional>
#include <string>
#include <vector>
#include "common/logging.h"
#include "common/random.h"
#include "common/scope_exit.h"
#include "common/settings.h"
#include "core/core.h"
#include "core/file_sys/content_archive.h"
#include "core/file_sys/fefw_60fps_search.h"
#include "core/file_sys/control_metadata.h"
#include "core/file_sys/nup_container.h"
#include "core/file_sys/patch_manager.h"
#include "core/file_sys/romfs_factory.h"
#include "core/file_sys/romfs.h"
#include "core/hle/kernel/k_page_table.h"
#include "core/hle/kernel/k_process.h"
#include "core/hle/service/filesystem/filesystem.h"
#include "core/loader/deconstructed_rom_directory.h"
#include "core/loader/nso.h"

#ifdef HAS_NCE
#include "core/arm/nce/patcher.h"
#endif

#ifndef HAS_NCE
namespace Core::NCE {
class Patcher {};
} // namespace Core::NCE
#endif

namespace Loader {

struct PatchCollection {
    explicit PatchCollection(bool is_application_) : is_application{is_application_} {
        module_patcher_indices.fill(-1);
        patchers.emplace_back();
    }

    std::vector<Core::NCE::Patcher>* GetPatchers() {
        if (is_application && Settings::IsNceEnabled()) {
            return &patchers;
        }
        return nullptr;
    }

    size_t GetTotalPatchSize() const {
        size_t total_size{};
#ifdef HAS_NCE
        for (auto& patcher : patchers) {
            total_size += patcher.GetSectionSize();
        }
#endif
        return total_size;
    }

    void SaveIndex(size_t module) {
        module_patcher_indices[module] = static_cast<s32>(patchers.size() - 1);
    }

    s32 GetIndex(size_t module) const {
        return module_patcher_indices[module];
    }

    s32 GetLastIndex() const {
        return static_cast<s32>(patchers.size()) - 1;
    }

    bool is_application;
    std::vector<Core::NCE::Patcher> patchers;
    std::array<s32, 13> module_patcher_indices{};
};

namespace {

/// A folder name that is a whole number (an update's version, e.g. 65536).
std::optional<u64> NumericFolderName(const std::string& name) {
    if (name.empty() || name.size() > 19 ||
        !std::all_of(name.begin(), name.end(), [](char c) { return c >= '0' && c <= '9'; })) {
        return std::nullopt;
    }
    return std::stoull(name);
}

/// The newest loose update for a dump whose ExeFS is exefs_dir: a numeric folder with an exefs or
/// romfs inside "updates" (or "update") next to the dump's exefs/romfs, or next to the folder
/// holding the dump (<folder>/base/exefs with <folder>/updates/<number>/...).
FileSys::VirtualDir FindLooseUpdate(const FileSys::VirtualDir& exefs_dir) {
    if (exefs_dir == nullptr) {
        return nullptr;
    }
    const auto title_dir = exefs_dir->GetParentDirectory();
    if (title_dir == nullptr) {
        return nullptr;
    }
    std::vector<FileSys::VirtualDir> roots{title_dir};
    if (const auto above = title_dir->GetParentDirectory()) {
        roots.push_back(above);
    }
    FileSys::VirtualDir best;
    u64 best_version{};
    for (const auto& root : roots) {
        for (const char* container_name : {"updates", "update"}) {
            const auto container = root->GetSubdirectory(container_name);
            if (container == nullptr) {
                continue;
            }
            for (const auto& candidate : container->GetSubdirectories()) {
                const auto version = NumericFolderName(candidate->GetName());
                if (!version || (candidate->GetSubdirectory("exefs") == nullptr &&
                                 candidate->GetSubdirectory("romfs") == nullptr)) {
                    continue;
                }
                if (best == nullptr || *version > best_version) {
                    best = candidate;
                    best_version = *version;
                }
            }
        }
        if (best != nullptr) {
            break;
        }
    }
    return best;
}

/// control.nacp (or any .nacp) in dir.
FileSys::VirtualFile FindNacp(const FileSys::VirtualDir& dir) {
    if (dir == nullptr) {
        return nullptr;
    }
    if (auto nacp = dir->GetFile("control.nacp")) {
        return nacp;
    }
    for (const auto& f : dir->GetFiles()) {
        if (f->GetExtension() == "nacp") {
            return f;
        }
    }
    return nullptr;
}

} // namespace

AppLoader_DeconstructedRomDirectory::AppLoader_DeconstructedRomDirectory(FileSys::VirtualFile file_, bool override_update_)
    : AppLoader(std::move(file_))
    , override_update(override_update_)
{
    const auto file_dir = file->GetContainingDirectory();

    // Title ID
    const auto npdm = file_dir->GetFile("main.npdm");
    if (npdm != nullptr) {
        const auto res = metadata.Load(npdm);
        if (res == ResultStatus::Success)
            title_id = metadata.GetTitleID();
    }

    // Icon
    FileSys::VirtualFile icon_file = nullptr;
    for (const auto& language : FileSys::LANGUAGE_NAMES) {
        icon_file = file_dir->GetFile("icon_" + std::string(language) + ".dat");
        if (icon_file != nullptr) {
            icon_data = icon_file->ReadAllBytes();
            break;
        }
    }

    if (icon_data.empty()) {
        // Any png, jpeg, or bmp file
        const auto& files = file_dir->GetFiles();
        const auto icon_iter =
            std::find_if(files.begin(), files.end(), [](const FileSys::VirtualFile& f) {
                return f->GetExtension() == "png" || f->GetExtension() == "jpg" ||
                       f->GetExtension() == "bmp" || f->GetExtension() == "jpeg";
            });
        if (icon_iter != files.end())
            icon_data = (*icon_iter)->ReadAllBytes();
    }

    // Metadata: control.nacp next to main, else in the dump's folder or the one above it
    FileSys::VirtualFile nacp_file = FindNacp(file_dir);
    auto nacp_parent = file_dir->GetParentDirectory();
    for (int level = 0; level < 2 && nacp_file == nullptr && nacp_parent != nullptr; ++level) {
        nacp_file = FindNacp(nacp_parent);
        nacp_parent = nacp_parent->GetParentDirectory();
    }

    // A loose update next to the dump: its program runs, its romfs is laid over the base's, and
    // its control data (name, version) wins.
    update_dir = FindLooseUpdate(file_dir);
    if (update_dir != nullptr) {
        const auto update_exefs = update_dir->GetSubdirectory("exefs");
        if (update_exefs != nullptr && update_exefs->GetFile("main") != nullptr &&
            update_exefs->GetFile("main.npdm") != nullptr) {
            dir = update_exefs;
        }
        if (auto update_nacp = FindNacp(update_dir); update_nacp != nullptr) {
            nacp_file = update_nacp;
        } else if (auto exefs_nacp = FindNacp(update_exefs); exefs_nacp != nullptr) {
            nacp_file = exefs_nacp;
        }
        LOG_INFO(Loader, "Loose update {} found next to the dump: {}{}", update_dir->GetName(),
                 dir != nullptr ? "its program runs" : "its program is missing, the base runs",
                 update_dir->GetSubdirectory("romfs") != nullptr ? ", its romfs is laid over the base's"
                                                                 : "");
    }

    if (nacp_file != nullptr) {
        control.emplace(nacp_file);
        name = control->GetApplicationName();
        LOG_INFO(Loader, "Dump control data: {} version {}", name, control->GetVersionString());
    }
}

AppLoader_DeconstructedRomDirectory::AppLoader_DeconstructedRomDirectory(
    FileSys::VirtualDir directory, bool override_update_)
    : AppLoader(directory->GetFile("main"))
    , dir(std::move(directory))
    , override_update(override_update_)
    , standalone(false)
{}

FileType AppLoader_DeconstructedRomDirectory::IdentifyType(const FileSys::VirtualFile& dir_file) {
    const auto directory = dir_file->GetContainingDirectory();
    if (dir_file->GetName() == "main" && directory &&
        AppLoader_NSO::IdentifyType(dir_file) == FileType::NSO &&
        FileSys::IsDirectoryExeFS(directory)) {
        return FileType::DeconstructedRomDirectory;
    }

    return FileType::Error;
}

AppLoader_DeconstructedRomDirectory::LoadResult AppLoader_DeconstructedRomDirectory::Load(
    Kernel::KProcess& process, Core::System& system) {
    if (is_loaded) {
        return {ResultStatus::ErrorAlreadyLoaded, {}};
    }

    if (dir == nullptr) {
        if (file == nullptr) {
            return {ResultStatus::ErrorNullFile, {}};
        }

        dir = file->GetContainingDirectory();
    }

    // Read meta to determine title ID
    FileSys::VirtualFile npdm = dir->GetFile("main.npdm");
    if (npdm == nullptr) {
        return {ResultStatus::ErrorMissingNPDM, {}};
    }

    const ResultStatus result = metadata.Load(npdm);
    if (result != ResultStatus::Success) {
        return {result, {}};
    }

    if (override_update) {
        const FileSys::PatchManager patch_manager(
            metadata.GetTitleID(), system.GetFileSystemController(), system.GetContentProvider());
        dir = patch_manager.PatchExeFS(dir);
    }

    // Reread in case PatchExeFS affected the main.npdm
    npdm = dir->GetFile("main.npdm");
    if (npdm == nullptr) {
        return {ResultStatus::ErrorMissingNPDM, {}};
    }

    const ResultStatus result2 = metadata.Reload(npdm);
    if (result2 != ResultStatus::Success) {
        return {result2, {}};
    }
    metadata.Print();

    // Extended ACID programs currently require the Dynarmic compatibility path.
    const bool is_39bit =
        metadata.GetAddressSpaceType() == FileSys::ProgramAddressSpaceType::Is39Bit;
    const bool is_application = metadata.GetPoolPartition() == FileSys::PoolPartition::Application;
    Settings::SetNceEnabled(is_39bit && !metadata.UsesExtendedAcid());

    const std::array static_modules = {"rtld",    "main",    "subsdk0", "subsdk1", "subsdk2",
                                       "subsdk3", "subsdk4", "subsdk5", "subsdk6", "subsdk7",
                                       "subsdk8", "subsdk9", "sdk"};

    std::size_t code_size{};

    // Define an nce patch context for each potential module.
    PatchCollection patch_ctx{is_application};

    // Use the NSO module loader to figure out the code layout
    for (size_t i = 0; i < static_modules.size(); i++) {
        const auto& module = static_modules[i];
        const FileSys::VirtualFile module_file{dir->GetFile(module)};
        if (!module_file) {
            continue;
        }

        const bool should_pass_arguments = std::strcmp(module, "rtld") == 0;
        const auto tentative_next_load_addr = AppLoader_NSO::LoadModule(
            process, system, *module_file, code_size, should_pass_arguments, false, {},
            patch_ctx.GetPatchers(), patch_ctx.GetLastIndex());
        if (!tentative_next_load_addr) {
            return {ResultStatus::ErrorLoadingNSO, {}};
        }

        patch_ctx.SaveIndex(i);
        code_size = *tentative_next_load_addr;
    }

    // Enable direct memory mapping in case of NCE.
    const u64 fastmem_base = [&]() -> size_t {
        if (is_application && Settings::IsNceEnabled()) {
            auto& buffer = system.DeviceMemory().buffer;
            buffer.EnableDirectMappedAddress();
            return reinterpret_cast<u64>(buffer.VirtualBasePointer());
        }
        return 0;
    }();

    // Add patch size to the total module size
    code_size += patch_ctx.GetTotalPatchSize();

    // Fortune's Eden: room for the 60 FPS patch's routines and table after the last module, for
    // when a mod uses the free space they usually go in. After every module rather than right
    // after main, so no module moves: mods reach other modules at fixed distances from main, and
    // a room after main broke one that does (#45). Not with native code (NCE), whose patch
    // sections sit after the modules and which would need the room executable.
    const std::size_t fefw_room_at = code_size;
    const bool fefw_room_possible = !Settings::IsNceEnabled();
    if (fefw_room_possible) {
        code_size += FefwSixtySearch::ReserveBytes;
    }

    // TODO: this is bad form of ASLR, it sucks
    std::uintptr_t aslr_offset = ((::Settings::values.rng_seed_enabled.GetValue()
        ? ::Settings::values.rng_seed.GetValue() : Common::Random::Random64(0)) << 12) & 0xfff000;

    // Setup the process code layout
    if (process.LoadFromMetadata(system.Kernel(), metadata, code_size, fastmem_base, aslr_offset).IsError()) {
        return {ResultStatus::ErrorUnableToParseKernelMetadata, {}};
    }

    // Load NSO modules
    modules.clear();
    const VAddr base_address{GetInteger(process.GetEntryPoint())};
    VAddr next_load_addr{base_address};
    const FileSys::PatchManager pm{metadata.GetTitleID(), system.GetFileSystemController(),
                                   system.GetContentProvider()};
    std::vector<u8> fefw_room;
    if (fefw_room_possible && pm.FefwSixtyEnabled()) {
        fefw_room.assign(FefwSixtySearch::ReserveBytes, 0);
        FefwSixtySearch::loader_room = {base_address + fefw_room_at, fefw_room};
    }
    SCOPE_EXIT {
        FefwSixtySearch::loader_room = {};
    };
    for (size_t i = 0; i < static_modules.size(); i++) {
        const auto& module = static_modules[i];
        const FileSys::VirtualFile module_file{dir->GetFile(module)};
        if (!module_file) {
            continue;
        }

        const VAddr load_addr{next_load_addr};
        const bool should_pass_arguments = std::strcmp(module, "rtld") == 0;
        const auto tentative_next_load_addr = AppLoader_NSO::LoadModule(
            process, system, *module_file, load_addr, should_pass_arguments, true, pm,
            patch_ctx.GetPatchers(), patch_ctx.GetIndex(i));
        if (!tentative_next_load_addr) {
            return {ResultStatus::ErrorLoadingNSO, {}};
        }

        next_load_addr = *tentative_next_load_addr;
        modules.insert_or_assign(load_addr, module);
        LOG_DEBUG(Loader, "loaded module {} @ {:#x}", module, load_addr);
    }

    if (!fefw_room.empty()) {
        // Read-write: the game's rtld takes every executable region as the start of a module (an
        // executable room was read as one, and the game stuck at boot following its "header").
        // The emulated CPU runs code from any mapped page.
        const VAddr room_address = base_address + fefw_room_at;
        process.GetMemory().WriteBlock(room_address, fefw_room.data(), fefw_room.size());
        const Result result = process.GetPageTable().SetProcessMemoryPermission(
            room_address, fefw_room.size(), Kernel::Svc::MemoryPermission::ReadWrite);
        if (result.IsError()) {
            LOG_ERROR(Loader,
                      "60 FPS mode: the room after the game's modules at {:#x} could not be set up",
                      room_address);
        }
    }

    is_loaded = true;
    if (standalone) {
        title_id = metadata.GetTitleID();
        // Register the game's own program ID (as the NUP, NSP and XCI loaders do), so account
        // saves go to save/.../<program id> instead of save/.../0000000000000000.
        system.GetFileSystemController().RegisterProcess(
            process.GetProcessId(), title_id,
            std::make_shared<FileSys::RomFSFactory>(*this, system.GetContentProvider(),
                                                    system.GetFileSystemController()));
    }
    return {ResultStatus::Success,
            LoadParameters{metadata.GetMainThreadPriority(), metadata.GetMainThreadStackSize()}};
}

ResultStatus AppLoader_DeconstructedRomDirectory::ReadRomFS(FileSys::VirtualFile& out_dir) {
    if (romfs == nullptr && standalone) {
        const auto directory = file->GetContainingDirectory();
        for (const auto& entry : directory->GetFiles()) {
            if (entry->GetExtension() == "romfs") {
                romfs = entry;
                break;
            }
        }
        FileSys::VirtualDir loose;
        if (romfs == nullptr) {
            loose = directory->GetSubdirectory("romfs");
            if (!loose && directory->GetName() == "exefs") {
                const auto parent = directory->GetParentDirectory();
                if (parent) {
                    loose = parent->GetSubdirectory("romfs");
                }
            }
        }
        // A loose update's romfs holds the files it changes or adds: lay it over the base's.
        if (update_dir != nullptr) {
            if (const auto update_romfs = update_dir->GetSubdirectory("romfs")) {
                FileSys::VirtualDir base = loose;
                if (base == nullptr && romfs != nullptr) {
                    base = FileSys::ExtractRomFS(romfs);
                }
                loose = FileSys::NupContainer::MergeShadowDirs(base, update_romfs);
                romfs = nullptr;
            }
        }
        if (romfs == nullptr && loose) {
            romfs = FileSys::CreateRomFS(std::move(loose));
        }
    }
    if (romfs == nullptr) {
        return ResultStatus::ErrorNoRomFS;
    }

    out_dir = romfs;
    return ResultStatus::Success;
}

ResultStatus AppLoader_DeconstructedRomDirectory::ReadIcon(std::vector<u8>& out_buffer) {
    if (icon_data.empty()) {
        return ResultStatus::ErrorNoIcon;
    }

    out_buffer = icon_data;
    return ResultStatus::Success;
}

ResultStatus AppLoader_DeconstructedRomDirectory::ReadProgramId(u64& out_program_id) {
    out_program_id = title_id;
    return ResultStatus::Success;
}

ResultStatus AppLoader_DeconstructedRomDirectory::ReadTitle(std::string& out_title) {
    if (name.empty()) {
        return ResultStatus::ErrorNoControl;
    }

    out_title = name;
    return ResultStatus::Success;
}

ResultStatus AppLoader_DeconstructedRomDirectory::ReadControlData(FileSys::NACP& out_control) {
    if (!control) {
        return ResultStatus::ErrorNoControl;
    }
    out_control = *control;
    return ResultStatus::Success;
}

bool AppLoader_DeconstructedRomDirectory::IsRomFSUpdatable() const {
    return false;
}

ResultStatus AppLoader_DeconstructedRomDirectory::ReadNSOModules(Modules& out_modules) {
    if (!is_loaded) {
        return ResultStatus::ErrorNotInitialized;
    }

    out_modules = this->modules;
    return ResultStatus::Success;
}

} // namespace Loader
