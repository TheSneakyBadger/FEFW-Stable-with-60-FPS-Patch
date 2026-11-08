// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include "core/loader/nup.h"

#include <algorithm>
#include <array>
#include <cstring>

#include "common/logging.h"
#include "common/string_util.h"
#include "core/core.h"
#include "core/file_sys/nup_container.h"
#include "core/file_sys/control_metadata.h"
#include "core/file_sys/patch_manager.h"
#include "core/file_sys/romfs.h"
#include "core/file_sys/romfs_factory.h"
#include "core/file_sys/vfs/vfs_vector.h"
#include "core/hle/kernel/k_process.h"
#include "core/hle/service/filesystem/filesystem.h"
#include "core/loader/deconstructed_rom_directory.h"

namespace Loader {

namespace {

using FileSys::NupContainer;

// NACP stores 16 uncompressed language entries; map BCP-47 tags onto them.
struct LanguageSlot {
    std::string_view tag;
    std::size_t index;
};

constexpr std::array<LanguageSlot, 18> LanguageSlots{{
    {"en-us", 0},   {"en-gb", 1},  {"ja", 2},     {"fr", 3},     {"de", 4},
    {"es-419", 5},  {"es", 6},     {"it", 7},     {"nl", 8},     {"fr-ca", 9},
    {"pt", 10},     {"ru", 11},    {"ko", 12},    {"zh-hant", 13}, {"zh-hans", 14},
    {"pt-br", 15},  {"pl", 16},    {"th", 17},
}};

void CopyNameToEntry(FileSys::LanguageEntry& entry, std::string_view name) {
    const std::size_t length = std::min(name.size(), entry.application_name.size() - 1);
    std::memcpy(entry.application_name.data(), name.data(), length);
    entry.application_name[length] = '\0';
}

void CopyDeveloperToEntry(FileSys::LanguageEntry& entry, std::string_view developer) {
    const std::size_t length = std::min(developer.size(), entry.developer_name.size() - 1);
    std::memcpy(entry.developer_name.data(), developer.data(), length);
    entry.developer_name[length] = '\0';
}

std::unique_ptr<FileSys::NACP> BuildNacp(const NupContainer::PackageInfo& base,
                                         const std::string& version) {
    const std::string& effective_title =
        !base.title_name.empty() ? base.title_name : base.source_dump;
    if (effective_title.empty() && base.localized_titles.empty() && base.publisher.empty() &&
        version.empty()) {
        return nullptr;
    }
    auto nacp = std::make_unique<FileSys::NACP>();
    nacp->raw.supported_languages = static_cast<FileSys::SupportedLanguage>(0x3FFFF);

    for (const auto& [lang, name] : base.localized_titles) {
        for (const auto& slot : LanguageSlots) {
            // The uncompressed RawNACP only stores 16 entries; Polish/Thai exist
            // only in the compressed format, so they fall back to the default.
            if (slot.index < 16 && slot.tag == lang) {
                CopyNameToEntry(nacp->raw.language_entries.language_entries[slot.index], name);
            }
        }
    }
    if (!effective_title.empty()) {
        CopyNameToEntry(nacp->raw.language_entries.language_entries[0], effective_title);
    }
    if (!base.publisher.empty()) {
        CopyDeveloperToEntry(nacp->raw.language_entries.language_entries[0], base.publisher);
    }
    if (!base.publisher.empty()) {
        auto& entry = nacp->raw.language_entries.language_entries[0];
        const std::size_t length =
            std::min(base.publisher.size(), entry.developer_name.size() - 1);
        std::memcpy(entry.developer_name.data(), base.publisher.data(), length);
        entry.developer_name[length] = '\0';
    }
    if (!version.empty()) {
        const std::size_t length = std::min(version.size(), nacp->raw.version_string.size() - 1);
        std::memcpy(nacp->raw.version_string.data(), version.data(), length);
        nacp->raw.version_string[length] = '\0';
    }
    nacp->raw.save_data_owner_id = base.title_id;

    nacp->language_entries.resize(16);
    std::memcpy(nacp->language_entries.data(), nacp->raw.language_entries.language_entries.data(),
                sizeof(nacp->raw.language_entries.language_entries));
    return nacp;
}

/// The version a package's own control data names: a control.nacp (or any .nacp) at its root or in
/// a control folder there.
std::string VersionFromPackageNacp(const FileSys::VirtualDir& package_dir) {
    if (package_dir == nullptr) {
        return {};
    }
    for (const auto& dir : {package_dir, package_dir->GetSubdirectory("control")}) {
        if (dir == nullptr) {
            continue;
        }
        FileSys::VirtualFile nacp_file = dir->GetFile("control.nacp");
        if (nacp_file == nullptr) {
            for (const auto& file : dir->GetFiles()) {
                if (file->GetExtension() == "nacp") {
                    nacp_file = file;
                    break;
                }
            }
        }
        if (nacp_file != nullptr) {
            return FileSys::NACP{nacp_file}.GetVersionString();
        }
    }
    return {};
}

/// Fortune's Weave builds whose version is known, for containers that carry no version at all
/// (no display version, no title version, no control data): the build ID of main.
std::string VersionFromKnownBuild(const FileSys::VirtualDir& exefs) {
    const auto main = exefs != nullptr ? exefs->GetFile("main") : nullptr;
    if (main == nullptr) {
        return {};
    }
    std::array<u8, 0x20> build_id{};
    if (main->ReadBytes(build_id.data(), build_id.size(), 0x40) != build_id.size()) {
        return {};
    }
    struct KnownBuild {
        std::array<u8, 8> prefix;
        const char* version;
    };
    static constexpr std::array<KnownBuild, 1> known{{
        // Version 1.1.0 (the build Kompas9's 60 FPS and ultrawide patches are made for).
        {{0xDB, 0x93, 0xB0, 0x12, 0x5F, 0x48, 0x50, 0x60}, "1.1.0"},
    }};
    for (const auto& build : known) {
        if (std::memcmp(build_id.data(), build.prefix.data(), build.prefix.size()) == 0) {
            return build.version;
        }
    }
    return {};
}

std::string DecodeVersion(u32 version) {
    if (version == 0) {
        return {};
    }
    return fmt::format("{}.{}.{}", version >> 16, (version >> 8) & 0xFF, version & 0xFF);
}

} // namespace

AppLoader_NUP::AppLoader_NUP(FileSys::VirtualFile file_)
    : AppLoader(std::move(file_)), nup(std::make_unique<NupContainer>(file)) {
    if (nup->GetStatus() != ResultStatus::Success) {
        return;
    }

    const auto base_index = nup->FindBasePackage();
    const auto update_index = nup->FindBestUpdatePackage();

    // The program comes from the base package; a standalone update container
    // (SPEC §4.1.2) carries a complete patched tree and boots on its own.
    std::optional<std::size_t> program_index;
    std::optional<std::size_t> exefs_index;
    if (base_index && nup->GetExeFSDir(*base_index) != nullptr) {
        program_index = base_index;
        exefs_index = update_index && nup->GetExeFSDir(*update_index) != nullptr
                          ? update_index
                          : base_index;
    } else if (update_index && nup->GetExeFSDir(*update_index) != nullptr) {
        LOG_INFO(Loader,
                 "NUP container is a standalone update; booting the update program directly");
        program_index = update_index;
        exefs_index = update_index;
    }
    if (!program_index || !exefs_index) {
        LOG_ERROR(Loader, "NUP container has no package with a usable ExeFS");
        return;
    }

    const auto& program = nup->GetPackages()[*program_index];
    base_title_id = program.title_id;
    has_program = true;

    const auto exefs = nup->GetExeFSDir(*exefs_index);
    if (exefs == nullptr || exefs->GetFile("main") == nullptr ||
        exefs->GetFile("main.npdm") == nullptr) {
        LOG_ERROR(Loader, "NUP package has no usable ExeFS");
        return;
    }
    secondary_loader = std::make_unique<AppLoader_DeconstructedRomDirectory>(exefs, true);

    const auto& version_package = nup->GetPackages()[update_index ? *update_index : *program_index];
    display_version = version_package.display_version;
    if (display_version.empty()) {
        // The container names no version (e.g. made without its metadata): the package's own
        // control data, else a known build of the program, else the numeric version. The numeric
        // one comes last because it may be a Nintendo title version (65536 is the first update),
        // which would read as 1.0.0. Otherwise the game's menu shows 1.0.0.
        const std::size_t version_index = update_index ? *update_index : *program_index;
        display_version = VersionFromPackageNacp(nup->GetPackageDir(version_index));
        const char* source = "its control data";
        if (display_version.empty()) {
            display_version = VersionFromKnownBuild(exefs);
            source = "the program's build ID";
        }
        if (display_version.empty() && (version_package.version & 0xFFFF) != 0) {
            // A multiple of 0x10000 is ambiguous (title version N or major.0.0), so skip it.
            display_version = DecodeVersion(version_package.version);
            source = "its version number";
        }
        if (!display_version.empty()) {
            LOG_INFO(Loader, "NUP container names no version; version {} from {}", display_version,
                     source);
        }
    }
    nacp_file = BuildNacp(program, display_version);
    if (!program.icon.empty()) {
        icon_file = std::make_shared<FileSys::VectorVfsFile>(program.icon, "icon.dat");
    } else {
        for (const auto& [lang, icon] : program.localized_icons) {
            if (lang == "en-us") {
                icon_file = std::make_shared<FileSys::VectorVfsFile>(icon, "icon_" + lang + ".dat");
            }
        }
        if (icon_file == nullptr && !program.localized_icons.empty()) {
            icon_file = std::make_shared<FileSys::VectorVfsFile>(
                program.localized_icons.front().second, "icon.dat");
        }
    }

    const auto addon_count = std::count_if(nup->GetPackages().begin(), nup->GetPackages().end(),
                                           [](const auto& package) {
                                               return package.kind ==
                                                      NupContainer::PackageKind::Addon;
                                           });
    if (addon_count > 0) {
        LOG_WARNING(Loader,
                    "NUP container has {} add-on content package(s); AoC mounting is not "
                    "supported yet",
                    addon_count);
    }
}

AppLoader_NUP::~AppLoader_NUP() = default;

FileType AppLoader_NUP::IdentifyType(const FileSys::VirtualFile& nup_file) {
    return FileSys::NupContainer::IsNupFile(nup_file) ? FileType::NUP : FileType::Error;
}

AppLoader::LoadResult AppLoader_NUP::Load(Kernel::KProcess& process, Core::System& system) {
    if (is_loaded) {
        return {ResultStatus::ErrorAlreadyLoaded, {}};
    }
    if (nup->GetStatus() != ResultStatus::Success) {
        return {nup->GetStatus(), {}};
    }
    if (!has_program || secondary_loader == nullptr) {
        return {ResultStatus::ErrorNUPNoBasePackage, {}};
    }

    const auto result = secondary_loader->Load(process, system);
    if (result.first != ResultStatus::Success) {
        return result;
    }

    system.GetFileSystemController().RegisterProcess(
        process.GetProcessId(), base_title_id,
        std::make_shared<FileSys::RomFSFactory>(*this, system.GetContentProvider(),
                                                system.GetFileSystemController()));

    is_loaded = true;
    return result;
}

ResultStatus AppLoader_NUP::VerifyIntegrity(
    std::function<bool(size_t, size_t)> progress_callback) {
    if (nup->GetStatus() != ResultStatus::Success) {
        return nup->GetStatus();
    }
    return nup->VerifyIntegrity(std::move(progress_callback));
}

ResultStatus AppLoader_NUP::ReadRomFS(FileSys::VirtualFile& out_file) {
    const auto base_index = nup->FindBasePackage();
    const auto base_romfs = base_index ? nup->GetRomFSDir(*base_index) : FileSys::VirtualDir{};
    const auto update_index = nup->FindBestUpdatePackage();
    const auto update_romfs =
        update_index ? nup->GetRomFSDir(*update_index) : FileSys::VirtualDir{};
    // A standalone update carries the complete merged tree; a base+update pair
    // overlays the update onto the base (SPEC §4.1.4).
    const auto merged = FileSys::NupContainer::MergeShadowDirs(base_romfs, update_romfs);
    if (merged == nullptr) {
        return ResultStatus::ErrorNoRomFS;
    }
    out_file = FileSys::CreateRomFS(merged);
    if (out_file == nullptr) {
        return ResultStatus::ErrorNoRomFS;
    }
    return ResultStatus::Success;
}

bool AppLoader_NUP::IsRomFSUpdatable() const {
    // The update is already merged into the exposed RomFS.
    return false;
}

ResultStatus AppLoader_NUP::ReadProgramId(u64& out_program_id) {
    if (!has_program) {
        return ResultStatus::ErrorNotInitialized;
    }
    out_program_id = base_title_id;
    return ResultStatus::Success;
}

ResultStatus AppLoader_NUP::ReadProgramIds(std::vector<u64>& out_program_ids) {
    if (!has_program) {
        return ResultStatus::ErrorNotInitialized;
    }
    out_program_ids.push_back(base_title_id);
    return ResultStatus::Success;
}

ResultStatus AppLoader_NUP::ReadIcon(std::vector<u8>& buffer) {
    if (icon_file == nullptr) {
        return ResultStatus::ErrorNoIcon;
    }
    buffer = icon_file->ReadAllBytes();
    return ResultStatus::Success;
}

ResultStatus AppLoader_NUP::ReadTitle(std::string& title) {
    if (nacp_file == nullptr) {
        return ResultStatus::ErrorNoControl;
    }
    title = nacp_file->GetApplicationName();
    if (title.empty()) {
        return ResultStatus::ErrorNoControl;
    }
    return ResultStatus::Success;
}

ResultStatus AppLoader_NUP::ReadControlData(FileSys::NACP& control) {
    if (nacp_file == nullptr) {
        return ResultStatus::ErrorNoControl;
    }
    control = *nacp_file;
    return ResultStatus::Success;
}

ResultStatus AppLoader_NUP::ReadNSOModules(Modules& modules) {
    if (secondary_loader == nullptr) {
        return ResultStatus::ErrorNotInitialized;
    }
    return secondary_loader->ReadNSOModules(modules);
}

} // namespace Loader
