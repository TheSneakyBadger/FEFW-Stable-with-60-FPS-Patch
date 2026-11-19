// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include "common/fefw_external_caps.h"

#include <fmt/format.h>

#include "common/fs/fs_util.h"
#include "common/logging.h"
#include "common/settings.h"

#ifdef _WIN32
#include <algorithm>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <cwctype>
#include <filesystem>
#include <fstream>
#include <optional>
#include <string_view>
#include <utility>
#include <vector>

#include <windows.h>
#include <tlhelp32.h>
#endif

namespace FefwExternalCaps {

#ifdef _WIN32
namespace {

std::wstring Lower(std::wstring text) {
    std::transform(text.begin(), text.end(), text.begin(),
                   [](wchar_t c) { return static_cast<wchar_t>(std::towlower(c)); });
    return text;
}

std::filesystem::path OwnExe() {
    std::vector<wchar_t> buffer(32768);
    const DWORD length = GetModuleFileNameW(nullptr, buffer.data(),
                                            static_cast<DWORD>(buffer.size()));
    if (length == 0 || length >= buffer.size()) {
        return {};
    }
    return std::filesystem::path{std::wstring{buffer.data(), length}};
}

// ---- RivaTuner Statistics Server ----------------------------------------------------------

/// RTSS.exe's folder while it runs; nothing when it doesn't (its cap then holds nothing back).
/// Running but its path unreadable: its usual install folder.
std::optional<std::filesystem::path> RunningRtssFolder() {
    const HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snapshot == INVALID_HANDLE_VALUE) {
        return std::nullopt;
    }
    PROCESSENTRY32W entry{};
    entry.dwSize = sizeof(entry);
    std::optional<DWORD> pid;
    for (BOOL more = Process32FirstW(snapshot, &entry); more;
         more = Process32NextW(snapshot, &entry)) {
        if (Lower(entry.szExeFile) == L"rtss.exe") {
            pid = entry.th32ProcessID;
            break;
        }
    }
    CloseHandle(snapshot);
    if (!pid) {
        return std::nullopt;
    }
    if (const HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, *pid)) {
        std::vector<wchar_t> buffer(32768);
        DWORD length = static_cast<DWORD>(buffer.size());
        const BOOL ok = QueryFullProcessImageNameW(process, 0, buffer.data(), &length);
        CloseHandle(process);
        if (ok && length > 0) {
            return std::filesystem::path{std::wstring{buffer.data(), length}}.parent_path();
        }
    }
    // The install folder RTSS records (a folder or its exe), else the default one.
    for (const wchar_t* const key :
         {L"SOFTWARE\\WOW6432Node\\Unwinder\\RTSS", L"SOFTWARE\\Unwinder\\RTSS"}) {
        wchar_t value[MAX_PATH * 2]{};
        DWORD size = sizeof(value);
        if (RegGetValueW(HKEY_LOCAL_MACHINE, key, L"InstallPath", RRF_RT_REG_SZ, nullptr, value,
                         &size) == ERROR_SUCCESS &&
            value[0] != L'\0') {
            std::filesystem::path path{value};
            if (Lower(path.extension().wstring()) == L".exe") {
                path = path.parent_path();
            }
            return path;
        }
    }
    wchar_t program_files[MAX_PATH]{};
    if (GetEnvironmentVariableW(L"ProgramFiles(x86)", program_files, MAX_PATH) == 0) {
        return std::nullopt;
    }
    return std::filesystem::path{program_files} / L"RivaTuner Statistics Server";
}

struct RtssLimit {
    bool has_limit_key = false;
    long limit = 0;
    long denominator = 1;
    long limit_time = 0;
    long sync_limiter = -1; ///< SyncLimiter: the limiter's mode (async, edge sync, Reflex...).
};

/// The [Framerate] section of an RTSS profile (Limit=120, LimitDenominator=1 for 120 FPS;
/// Limit=59940 with LimitDenominator=1000 for 59.94). Nothing if the file isn't there.
std::optional<RtssLimit> ReadRtssProfile(const std::filesystem::path& file) {
    std::ifstream in{file};
    if (!in) {
        return std::nullopt;
    }
    RtssLimit result;
    bool in_framerate = false;
    std::string line;
    while (std::getline(in, line)) {
        while (!line.empty() && (line.back() == '\r' || line.back() == ' ')) {
            line.pop_back();
        }
        if (!line.empty() && line.front() == '[') {
            in_framerate = line == "[Framerate]";
            continue;
        }
        if (!in_framerate) {
            continue;
        }
        const auto equals = line.find('=');
        if (equals == std::string::npos) {
            continue;
        }
        const std::string key = line.substr(0, equals);
        const long value = std::strtol(line.c_str() + equals + 1, nullptr, 10);
        if (key == "Limit") {
            result.has_limit_key = true;
            result.limit = value;
        } else if (key == "LimitDenominator") {
            result.denominator = value > 0 ? value : 1;
        } else if (key == "LimitTime") {
            result.limit_time = value;
        } else if (key == "SyncLimiter") {
            result.sync_limiter = value;
        }
    }
    return result;
}

void DetectRtss(Caps& caps) {
    const std::optional<std::filesystem::path> folder = RunningRtssFolder();
    if (!folder) {
        caps.rtss_detail = "not running";
        return;
    }
    const std::filesystem::path profiles = *folder / L"Profiles";
    // Fortune's Eden's own profile (named after its exe) if it sets a limit, else the global one.
    std::optional<RtssLimit> limit;
    const char* where = "global profile";
    const std::filesystem::path exe = OwnExe();
    if (!exe.empty()) {
        if (auto own = ReadRtssProfile(profiles / (exe.filename().wstring() + L".cfg"));
            own && own->has_limit_key) {
            limit = own;
            where = "Fortune's Eden's profile";
        }
    }
    if (!limit) {
        limit = ReadRtssProfile(profiles / L"Global");
    }
    if (!limit) {
        // The folder tried, in the log (no user folder in it: RTSS installs under Program Files).
        caps.rtss_detail = fmt::format("running, its profiles not found in {}",
                                       Common::FS::PathToUTF8String(profiles));
        return;
    }
    if (limit->limit > 0) {
        caps.rtss = static_cast<float>(limit->limit) / static_cast<float>(limit->denominator);
        // The limiter's mode goes in the log too: in its NVIDIA Reflex mode RTSS runs Reflex's
        // sleep in the process as well, beside DLSS frame generation's own.
        caps.rtss_detail = fmt::format("{:.2f} FPS ({}, limiter mode {})", caps.rtss, where,
                                       limit->sync_limiter);
    } else if (limit->limit_time > 0) {
        // A frame time limit instead: not read (its units aren't documented).
        caps.rtss_detail = fmt::format("a frame time limit, not read ({})", where);
    } else {
        caps.rtss_detail = fmt::format("running, no limit ({})", where);
    }
}

// ---- NVIDIA driver (NvAPI driver settings) ------------------------------------------------

using NvStatus = int;
using NvQueryInterface = void* (*)(unsigned int);
using NvInitialize = NvStatus (*)();
using NvCreateSession = NvStatus (*)(void** session);
using NvDestroySession = NvStatus (*)(void* session);
using NvLoadSettings = NvStatus (*)(void* session);
using NvFindApplicationByName = NvStatus (*)(void* session, const std::uint16_t* name,
                                             void** profile, void* application);
using NvGetCurrentGlobalProfile = NvStatus (*)(void* session, void** profile);
using NvGetBaseProfile = NvStatus (*)(void* session, void** profile);
using NvGetSetting = NvStatus (*)(void* session, void* profile, std::uint32_t id, void* setting);

constexpr unsigned int NvInitializeId = 0x0150E828;
constexpr unsigned int NvCreateSessionId = 0x0694D52E;
constexpr unsigned int NvDestroySessionId = 0xDAD9CFF8;
constexpr unsigned int NvLoadSettingsId = 0x375DBD6B;
constexpr unsigned int NvFindApplicationByNameId = 0xEEE566B2;
constexpr unsigned int NvGetCurrentGlobalProfileId = 0x617BFF9F;
constexpr unsigned int NvGetBaseProfileId = 0xDA8466A0;
constexpr unsigned int NvGetSettingId = 0x73BF8338;
/// "Max Frame Rate" (Frame Rate Limiter v3): the frames a second, 0 off.
constexpr std::uint32_t MaxFrameRateSetting = 0x10835002;

constexpr std::size_t UnicodeChars = 2048;
// NVDRS_APPLICATION, newest first: V4 = V3 + commandLine; V3 = V2 + a 32-bit flags word
// (isMetro...); V2 = V1 + fileInFolder; V1 = version, isPredefined, appName, userFriendlyName,
// launcher. Drivers refuse a version they don't take, so each is tried in turn.
constexpr std::size_t ApplicationV1Size = 4 + 4 + 3 * UnicodeChars * 2;
constexpr std::size_t ApplicationV2Size = ApplicationV1Size + UnicodeChars * 2;
constexpr std::size_t ApplicationV3Size = ApplicationV2Size + 4;
constexpr std::size_t ApplicationV4Size = ApplicationV3Size + UnicodeChars * 2;
// NVDRS_SETTING_V1: version, settingName, settingId, settingType, settingLocation,
// isCurrentPredefined, isPredefinedValid, then the predefined and current values (unions of
// a 4100-byte binary value, a string and a u32).
constexpr std::size_t SettingV1Size = 4 + UnicodeChars * 2 + 5 * 4 + 2 * 4100;
constexpr std::size_t SettingCurrentOffset = 4 + UnicodeChars * 2 + 5 * 4 + 4100;

std::uint32_t Version(std::size_t size, std::uint32_t version = 1) {
    return static_cast<std::uint32_t>(size) | (version << 16);
}

void DetectNvidia(Caps& caps) {
    const HMODULE nvapi = LoadLibraryW(L"nvapi64.dll");
    if (!nvapi) {
        caps.nvidia_detail = "no NVIDIA driver";
        return;
    }
    const auto query = reinterpret_cast<NvQueryInterface>(
        reinterpret_cast<void*>(GetProcAddress(nvapi, "nvapi_QueryInterface")));
    if (!query) {
        caps.nvidia_detail = "NvAPI unavailable";
        return;
    }
    const auto initialize = reinterpret_cast<NvInitialize>(query(NvInitializeId));
    const auto create_session = reinterpret_cast<NvCreateSession>(query(NvCreateSessionId));
    const auto destroy_session = reinterpret_cast<NvDestroySession>(query(NvDestroySessionId));
    const auto load_settings = reinterpret_cast<NvLoadSettings>(query(NvLoadSettingsId));
    const auto find_application =
        reinterpret_cast<NvFindApplicationByName>(query(NvFindApplicationByNameId));
    const auto global_profile =
        reinterpret_cast<NvGetCurrentGlobalProfile>(query(NvGetCurrentGlobalProfileId));
    const auto base_profile = reinterpret_cast<NvGetBaseProfile>(query(NvGetBaseProfileId));
    const auto get_setting = reinterpret_cast<NvGetSetting>(query(NvGetSettingId));
    if (!initialize || !create_session || !destroy_session || !load_settings ||
        !find_application || !global_profile || !get_setting) {
        caps.nvidia_detail = "NvAPI driver settings unavailable";
        return;
    }
    if (initialize() != 0) {
        caps.nvidia_detail = "NvAPI didn't start";
        return;
    }
    void* session{};
    if (create_session(&session) != 0 || !session) {
        caps.nvidia_detail = "NvAPI session failed";
        return;
    }
    if (const NvStatus status = load_settings(session); status != 0) {
        caps.nvidia_detail = fmt::format("NvAPI settings not loaded ({})", status);
        destroy_session(session);
        return;
    }
    // NvAPI's answer when a profile doesn't set it (NVAPI_SETTING_NOT_FOUND); any other failure
    // goes in the log, so a wrong structure version shows as one.
    constexpr NvStatus SettingNotFound = -160;
    NvStatus last_status = SettingNotFound;
    const auto read_cap = [&](void* profile) -> std::optional<std::uint32_t> {
        std::vector<std::uint8_t> setting(SettingV1Size + 64);
        const std::uint32_t version = Version(SettingV1Size);
        std::memcpy(setting.data(), &version, sizeof(version));
        if (const NvStatus status =
                get_setting(session, profile, MaxFrameRateSetting, setting.data());
            status != 0) {
            if (status != SettingNotFound) {
                last_status = status;
            }
            return std::nullopt;
        }
        std::uint32_t value{};
        std::memcpy(&value, setting.data() + SettingCurrentOffset, sizeof(value));
        return value;
    };
    std::optional<std::uint32_t> cap;
    const char* where = "all programs";
    const std::filesystem::path exe = OwnExe();
    if (!exe.empty()) {
        std::vector<std::uint16_t> name(UnicodeChars);
        const std::wstring file = exe.filename().wstring();
        for (std::size_t i = 0; i < file.size() && i + 1 < name.size(); ++i) {
            name[i] = static_cast<std::uint16_t>(file[i]);
        }
        // An explicit value in Fortune's Eden's profile (0 included: off for it) wins over
        // the one for all programs; not set there, the one for all programs applies.
        std::vector<std::uint8_t> application(ApplicationV4Size + 64);
        constexpr std::pair<std::size_t, std::uint32_t> versions[] = {
            {ApplicationV4Size, 4}, {ApplicationV3Size, 3}, {ApplicationV2Size, 2},
            {ApplicationV1Size, 1}};
        for (const auto& [size, number] : versions) {
            std::fill(application.begin(), application.end(), std::uint8_t{0});
            const std::uint32_t version = Version(size, number);
            std::memcpy(application.data(), &version, sizeof(version));
            void* profile{};
            const NvStatus status =
                find_application(session, name.data(), &profile, application.data());
            if (status == 0 && profile) {
                cap = read_cap(profile);
                where = "Fortune's Eden's profile";
                break;
            }
            // Only a refused structure version (NVAPI_INCOMPATIBLE_STRUCT_VERSION) tries the
            // next; not found (no profile for Fortune's Eden) or anything else stops.
            if (status != -9) {
                break;
            }
        }
    }
    // For all programs: the current global profile, then the base profile (where the NVIDIA app
    // and Control Panel store global settings; usually the same one).
    if (!cap) {
        void* profile{};
        if (global_profile(session, &profile) == 0 && profile) {
            cap = read_cap(profile);
            where = "all programs";
        }
    }
    if (!cap && base_profile) {
        void* profile{};
        if (base_profile(session, &profile) == 0 && profile) {
            cap = read_cap(profile);
            where = "all programs (base profile)";
        }
    }
    destroy_session(session);
    if (!cap) {
        caps.nvidia_detail = last_status == SettingNotFound
                                 ? std::string{"Max Frame Rate not set"}
                                 : fmt::format("Max Frame Rate not read (NvAPI {})", last_status);
    } else if (*cap == 0 || *cap > 1000) {
        caps.nvidia_detail = fmt::format("Max Frame Rate off ({})", where);
    } else {
        caps.nvidia = static_cast<float>(*cap);
        caps.nvidia_detail = fmt::format("Max Frame Rate {} FPS ({})", *cap, where);
    }
}

} // Anonymous namespace
#endif

Caps Detect() {
    Caps caps;
#ifdef _WIN32
    DetectRtss(caps);
    DetectNvidia(caps);
#else
    caps.rtss_detail = "not read on this system";
    caps.nvidia_detail = "not read on this system";
#endif
    Settings::SetExternalFrameCaps(caps.rtss, caps.nvidia);
    LOG_INFO(Common, "Frame rate caps outside Fortune's Eden: RTSS {}; NVIDIA {}",
             caps.rtss_detail, caps.nvidia_detail);
    return caps;
}

} // namespace FefwExternalCaps
