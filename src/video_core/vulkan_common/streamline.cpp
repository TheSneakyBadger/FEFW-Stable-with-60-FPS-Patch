// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

// NVIDIA DLSS through NVIDIA Streamline, with Streamline's manual hooking.

#include <algorithm>
#include <atomic>
#include <cmath>
#include <utility>
#include <array>
#include <chrono>
#include <mutex>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <optional>
#include <string>
#include <string_view>
#include <type_traits>
#include <vector>

#include <fmt/format.h>

#include "common/fs/fs_util.h"
#include "common/fs/path_util.h"
#include "common/logging.h"
#include "common/fefw_frame_gen_hook.h"
#include "common/settings.h"
#include "video_core/vulkan_common/streamline.h"
#include "video_core/renderer_vulkan/fefw_present_timing.h"
#include "video_core/vulkan_common/streamline_api.h"

#include <sl_security.h>

#include <bcrypt.h>
#include <softpub.h>
#include <wintrust.h>

namespace Vulkan::Streamline {
namespace {

// A custom engine names itself with its own project id; an application id from NVIDIA is only
// needed without an engine type. Keep it fixed: the driver may key behaviour off it.
constexpr const char* PROJECT_ID = "8d3e2f9a-6b41-4c7e-9a52-1f0c3b7d5e64";
constexpr const wchar_t* INTERPOSER = L"sl.interposer.dll";
constexpr const wchar_t* MODEL = L"nvngx_dlss.dll";
// The first DLSS with preset K.
constexpr u32 MIN_DLSS_MAJOR = 310;

using Version = std::array<u32, 4>;

struct State {
    HMODULE module{};
    Api api{};
    std::wstring plugin_dir;
    std::wstring model_dir;
    std::vector<std::string> instance_extensions;
    std::vector<std::string> device_extensions;
    u32 extra_graphics_queues{};
    u32 extra_compute_queues{};
    bool want_dlss{};        ///< DLSS super resolution was asked for and its plugin loaded.
    bool want_frame_gen{};   ///< DLSS frame generation was asked for and its plugins loaded.
    bool dlss_available{};
    bool frame_gen_available{};
    bool started{};
    bool attached{};
};

State& GetState() {
    static State state;
    return state;
}

int LogLevel() {
    const char* const value = std::getenv("EDEN_NX2_DLSS_LOG");
    return value ? std::atoi(value) : 0;
}

std::atomic<u64> evaluate_failures{0};

void LogFromStreamline(sl::LogType type, const char* message) {
    std::string_view text{message ? message : ""};
    if (type == sl::LogType::eError && text.find("evaluate feature failed") != std::string_view::npos) {
        evaluate_failures.fetch_add(1, std::memory_order_relaxed);
    }
    while (!text.empty() && (text.back() == '\n' || text.back() == '\r')) {
        text.remove_suffix(1);
    }
    switch (type) {
    case sl::LogType::eError:
        LOG_ERROR(Render_Vulkan, "Streamline: {}", text);
        break;
    case sl::LogType::eWarn:
        LOG_WARNING(Render_Vulkan, "Streamline: {}", text);
        break;
    default:
        if (LogLevel() > 0) {
            LOG_INFO(Render_Vulkan, "Streamline: {}", text);
        }
        break;
    }
}

/// Where the user may put sl.interposer.dll: EDEN_STREAMLINE_DIR, the user folder's "streamline"
/// folder, then a "streamline" folder next to the executable.
std::vector<std::filesystem::path> SearchDirs() {
    std::vector<std::filesystem::path> dirs;
    if (const char* const dir = std::getenv("EDEN_STREAMLINE_DIR"); dir && *dir) {
        dirs.emplace_back(dir);
    }
    dirs.push_back(UserFolder());
    dirs.push_back(Common::FS::GetExeDirectory() / "streamline");
    return dirs;
}

template <typename T>
bool Resolve(HMODULE module, const char* name, T*& function) {
    function = reinterpret_cast<T*>(GetProcAddress(module, name));
    if (!function) {
        LOG_ERROR(Render_Vulkan, "sl.interposer.dll has no {}", name);
    }
    return function != nullptr;
}

std::optional<Version> ReadVersion(const std::filesystem::path& path) {
    DWORD handle = 0;
    const DWORD size = GetFileVersionInfoSizeW(path.c_str(), &handle);
    if (size == 0) {
        return std::nullopt;
    }
    std::vector<u8> data(size);
    VS_FIXEDFILEINFO* info{};
    UINT length = 0;
    if (!GetFileVersionInfoW(path.c_str(), 0, size, data.data()) ||
        !VerQueryValueW(data.data(), L"\\", reinterpret_cast<void**>(&info), &length) || !info) {
        return std::nullopt;
    }
    return Version{HIWORD(info->dwFileVersionMS), LOWORD(info->dwFileVersionMS),
                   HIWORD(info->dwFileVersionLS), LOWORD(info->dwFileVersionLS)};
}

std::string VersionText(const std::filesystem::path& path) {
    const std::optional<Version> version = path.empty() ? std::nullopt : ReadVersion(path);
    if (!version) {
        return {};
    }
    const Version& v = *version;
    return v[3] == 0 ? fmt::format("{}.{}.{}", v[0], v[1], v[2])
                     : fmt::format("{}.{}.{}.{}", v[0], v[1], v[2], v[3]);
}

/// The NVIDIA app keeps every DLSS it downloads under ProgramData; pick the newest.
std::filesystem::path FindAppModel() {
    const wchar_t* const program_data = _wgetenv(L"ProgramData");
    if (!program_data) {
        return {};
    }
    const std::filesystem::path root =
        std::filesystem::path{program_data} / L"NVIDIA" / L"NGX" / L"models" / L"dlss" / L"versions";
    std::filesystem::path best_path;
    Version best{};
    std::error_code ec;
    for (const auto& version_dir : std::filesystem::directory_iterator(root, ec)) {
        std::error_code files_ec;
        for (const auto& file :
             std::filesystem::directory_iterator(version_dir.path() / L"files", files_ec)) {
            if (file.path().extension() != L".bin") {
                continue;
            }
            const std::optional<Version> version = ReadVersion(file.path());
            if (version && (*version)[0] >= MIN_DLSS_MAJOR && *version > best) {
                best = *version;
                best_path = file.path();
            }
        }
    }
    return best_path;
}

/// Copies the NVIDIA app's DLSS into our folder as nvngx_dlss.dll, the name Streamline loads.
std::filesystem::path UseAppModel(const DlssFiles& files) {
    const std::filesystem::path dir = UserFolder() / L"nvidia-app";
    const std::filesystem::path target = dir / MODEL;
    std::error_code ec;
    const bool current = std::filesystem::is_regular_file(target, ec) &&
                         std::filesystem::file_size(target, ec) ==
                             std::filesystem::file_size(files.app_model, ec) &&
                         ReadVersion(target) == ReadVersion(files.app_model);
    if (!current) {
        if (!IsSignedByNvidia(files.app_model)) {
            LOG_WARNING(Render_Vulkan, "The NVIDIA app's DLSS {} is not signed by NVIDIA",
                        Common::FS::PathToUTF8String(files.app_model));
            return {};
        }
        std::filesystem::create_directories(dir, ec);
        std::filesystem::copy_file(files.app_model, target,
                                   std::filesystem::copy_options::overwrite_existing, ec);
        if (ec) {
            LOG_WARNING(Render_Vulkan, "Could not copy the NVIDIA app's DLSS to {}: {}",
                        Common::FS::PathToUTF8String(target), ec.message());
            return {};
        }
    }
    return dir;
}

/// SHA-256 of a file, in lower-case hex; empty when it can't be read.
std::string FileSha256(const std::filesystem::path& path) {
    BCRYPT_ALG_HANDLE algorithm{};
    if (BCryptOpenAlgorithmProvider(&algorithm, BCRYPT_SHA256_ALGORITHM, nullptr, 0) != 0) {
        return {};
    }
    BCRYPT_HASH_HANDLE hash{};
    std::string result;
    if (BCryptCreateHash(algorithm, &hash, nullptr, 0, nullptr, 0, 0) == 0) {
        std::ifstream file(path, std::ios::binary);
        std::vector<char> buffer(1 << 16);
        bool ok = static_cast<bool>(file);
        while (ok && file) {
            file.read(buffer.data(), static_cast<std::streamsize>(buffer.size()));
            const auto read = file.gcount();
            if (read > 0 && BCryptHashData(hash, reinterpret_cast<PUCHAR>(buffer.data()),
                                           static_cast<ULONG>(read), 0) != 0) {
                ok = false;
            }
        }
        std::array<UCHAR, 32> digest{};
        if (ok && BCryptFinishHash(hash, digest.data(), static_cast<ULONG>(digest.size()), 0) == 0) {
            for (const UCHAR byte : digest) {
                result += fmt::format("{:02x}", byte);
            }
        }
        BCryptDestroyHash(hash);
    }
    BCryptCloseAlgorithmProvider(algorithm, 0);
    return result;
}

void LoadMfgUnlocker() {
    static bool tried = false;
    if (tried) {
        return;
    }
    tried = true;
    const std::filesystem::path path = MfgUnlockerPath();
    std::error_code ec;
    if (!std::filesystem::exists(path, ec)) {
        LOG_INFO(Render_Vulkan, "DLSS multi frame generation: the MFG Unlocker isn't installed "
                                "(Settings, Graphics, Frame generation can download it)");
        return;
    }
    if (FileSha256(path) != MfgUnlockerSha256) {
        LOG_ERROR(Render_Vulkan, "{} isn't the MFG Unlocker release Fortune's Eden downloads; not "
                                 "loading it",
                  Common::FS::PathToUTF8String(path));
        return;
    }
    if (LoadLibraryW(path.c_str()) == nullptr) {
        LOG_ERROR(Render_Vulkan, "Could not load the MFG Unlocker (error {})", GetLastError());
        return;
    }
    LOG_INFO(Render_Vulkan, "DLSS multi frame generation: MFG Unlocker loaded");
}

bool Load(State& state, const std::filesystem::path& path) {
    if (!IsStreamlineModule(path)) {
        LOG_ERROR(Render_Vulkan, "{} is not signed by NVIDIA; not loading it",
                  Common::FS::PathToUTF8String(path));
        return false;
    }
    state.module = LoadLibraryW(path.c_str());
    if (!state.module) {
        LOG_ERROR(Render_Vulkan, "Could not load {} (error {})", Common::FS::PathToUTF8String(path),
                  GetLastError());
        return false;
    }
    Api& api = state.api;
    const HMODULE m = state.module;
    if (Resolve(m, "slInit", api.init) && Resolve(m, "slShutdown", api.shutdown) &&
        Resolve(m, "slIsFeatureSupported", api.is_feature_supported) &&
        Resolve(m, "slGetFeatureRequirements", api.get_feature_requirements) &&
        Resolve(m, "slGetFeatureVersion", api.get_feature_version) &&
        Resolve(m, "slGetFeatureFunction", api.get_feature_function) &&
        Resolve(m, "slGetNewFrameToken", api.get_new_frame_token) &&
        Resolve(m, "slSetConstants", api.set_constants) &&
        Resolve(m, "slEvaluateFeature", api.evaluate_feature) &&
        Resolve(m, "slFreeResources", api.free_resources) &&
        Resolve(m, "slSetVulkanInfo", api.set_vulkan_info) &&
        Resolve(m, "slSetTagForFrame", api.set_tag_for_frame)) {
        return true;
    }
    FreeLibrary(state.module);
    state.module = nullptr;
    state.api = {};
    return false;
}

std::vector<std::string> Names(uint32_t count, const char** names) {
    std::vector<std::string> result;
    for (uint32_t i = 0; i < count; ++i) {
        result.emplace_back(names[i]);
    }
    return result;
}

} // Anonymous namespace

std::filesystem::path UserFolder() {
    return Common::FS::GetEdenPath(Common::FS::EdenPath::StreamlineDir);
}

std::filesystem::path MfgUnlockerPath() {
    return UserFolder() / L"mfg-unlocker" / L"MFGUnlocker.dll";
}

bool IsStreamlineModule(const std::filesystem::path& path) {
    return sl::security::verifyEmbeddedSignature(path.c_str());
}

bool IsSignedByNvidia(const std::filesystem::path& path) {
    const std::wstring file_path = std::filesystem::absolute(path).wstring();
    WINTRUST_FILE_INFO file{};
    file.cbStruct = sizeof(file);
    file.pcwszFilePath = file_path.c_str();
    WINTRUST_DATA data{};
    data.cbStruct = sizeof(data);
    data.dwUIChoice = WTD_UI_NONE;
    data.fdwRevocationChecks = WTD_REVOKE_NONE;
    data.dwUnionChoice = WTD_CHOICE_FILE;
    data.pFile = &file;
    data.dwStateAction = WTD_STATEACTION_VERIFY;
    data.dwProvFlags = WTD_CACHE_ONLY_URL_RETRIEVAL;
    GUID action = WINTRUST_ACTION_GENERIC_VERIFY_V2;
    bool valid = WinVerifyTrust(nullptr, &action, &data) == ERROR_SUCCESS;
    if (valid) {
        CRYPT_PROVIDER_DATA* const provider = WTHelperProvDataFromStateData(data.hWVTStateData);
        CRYPT_PROVIDER_SGNR* const signer =
            provider ? WTHelperGetProvSignerFromChain(provider, 0, FALSE, 0) : nullptr;
        CRYPT_PROVIDER_CERT* const cert = signer ? WTHelperGetProvCertFromChain(signer, 0) : nullptr;
        wchar_t name[128]{};
        valid = cert &&
                CertGetNameStringW(cert->pCert, CERT_NAME_SIMPLE_DISPLAY_TYPE, 0, nullptr, name,
                                   static_cast<DWORD>(std::size(name))) > 1 &&
                std::wstring_view{name} == L"NVIDIA Corporation";
    }
    data.dwStateAction = WTD_STATEACTION_CLOSE;
    WinVerifyTrust(nullptr, &action, &data);
    return valid;
}

DlssFiles FindDlssFiles() {
    DlssFiles files;
    files.folder = UserFolder();
    std::error_code ec;
    for (const std::filesystem::path& dir : SearchDirs()) {
        if (std::filesystem::is_regular_file(dir / INTERPOSER, ec)) {
            files.folder = dir;
            files.interposer = dir / INTERPOSER;
            break;
        }
    }
    if (std::filesystem::is_regular_file(files.folder / MODEL, ec)) {
        files.model = files.folder / MODEL;
    }
    files.app_model = FindAppModel();
    files.interposer_version = VersionText(files.interposer);
    files.model_version = VersionText(files.model);
    files.app_model_version = VersionText(files.app_model);
    return files;
}

std::vector<std::string> MissingFrameGenFiles() {
    const std::filesystem::path folder = FindDlssFiles().folder;
    std::vector<std::string> missing;
    std::error_code ec;
    for (const char* name : {"sl.interposer.dll", "sl.common.dll", "sl.dlss_g.dll",
                             "sl.reflex.dll", "sl.pcl.dll", "nvngx_dlssg.dll"}) {
        if (!std::filesystem::is_regular_file(folder / name, ec)) {
            missing.emplace_back(name);
        }
    }
    return missing;
}

bool Start() {
    State& state = GetState();
    if (state.started) {
        return true;
    }
    const bool dlss_wanted = Settings::values.upscaler.GetValue() == Settings::Upscaler::NvidiaDlss;
    const bool frame_gen_wanted =
        Settings::values.frame_generation.GetValue() == Settings::FrameGeneration::NvidiaDlss;
    if (!dlss_wanted && !frame_gen_wanted) {
        return false;
    }
    if (!state.module) {
        const DlssFiles files = FindDlssFiles();
        if (files.interposer.empty()) {
            LOG_WARNING(Render_Vulkan,
                        "NVIDIA DLSS is on, but sl.interposer.dll (NVIDIA Streamline) is not in "
                        "{} (the Graphics settings can download it); the game's own DLSS (and no "
                        "DLSS frame generation) runs",
                        Common::FS::PathToUTF8String(files.folder));
            return false;
        }
        // 3x or 4x with DLSS frame generation: the MFG Unlocker, when it was downloaded, goes in
        // before Streamline so it can lift NVIDIA's 2x limit on RTX 40 and older (on RTX 50 it
        // changes nothing). Only the release Fortune's Eden downloads is loaded.
        if (frame_gen_wanted && Settings::values.mfg_multiplier.GetValue() != Settings::MfgMultiplier::X2) {
            LoadMfgUnlocker();
        }
        if (!Load(state, files.interposer)) {
            return false;
        }
        state.plugin_dir = files.folder.wstring();
        state.model_dir.clear();
        if (files.model.empty() && !files.app_model.empty()) {
            if (const std::filesystem::path dir = UseAppModel(files); !dir.empty()) {
                state.model_dir = dir.wstring();
                LOG_INFO(Render_Vulkan, "Using the NVIDIA app's DLSS {}", files.app_model_version);
            }
        } else if (files.model.empty()) {
            LOG_WARNING(Render_Vulkan, "No nvngx_dlss.dll in {} and no DLSS from the NVIDIA app",
                        Common::FS::PathToUTF8String(files.folder));
        }
    }

    // Streamline's plugins come from the loader's folder or the NVIDIA app's downloads (OTA); the
    // DLSS model from the loader's folder, else the NVIDIA app's copy.
    const wchar_t* plugin_paths[] = {state.plugin_dir.c_str(), state.model_dir.c_str()};
    // DLSS super resolution when it is the upscaler; DLSS frame generation with what it needs
    // (Reflex and PC latency markers) when that is on.
    std::vector<sl::Feature> features;
    if (dlss_wanted) {
        features.push_back(sl::kFeatureDLSS);
    }
    if (frame_gen_wanted) {
        features.push_back(sl::kFeatureDLSS_G);
        features.push_back(sl::kFeatureReflex);
        features.push_back(sl::kFeaturePCL);
    }
    sl::Preferences preferences{};
    preferences.logLevel = LogLevel() >= 2 ? sl::LogLevel::eVerbose : sl::LogLevel::eDefault;
    preferences.logMessageCallback = LogFromStreamline;
    preferences.pathsToPlugins = plugin_paths;
    preferences.numPathsToPlugins = state.model_dir.empty() ? 1 : 2;
    preferences.flags = sl::PreferenceFlags::eDisableCLStateTracking |
                        sl::PreferenceFlags::eUseManualHooking | sl::PreferenceFlags::eAllowOTA |
                        sl::PreferenceFlags::eLoadDownloadedPlugins;
    if (frame_gen_wanted) {
        // Frame generation's inputs are tagged per frame (slSetTagForFrame).
        preferences.flags |= sl::PreferenceFlags::eUseFrameBasedResourceTagging;
    }
    preferences.featuresToLoad = features.data();
    preferences.numFeaturesToLoad = static_cast<u32>(features.size());
    preferences.engine = sl::EngineType::eCustom;
    preferences.engineVersion = "1.0";
    preferences.projectId = PROJECT_ID;
    preferences.renderAPI = sl::RenderAPI::eVulkan;
    const Api& api = state.api;
    if (const sl::Result result = api.init(preferences, sl::kSDKVersion);
        result != sl::Result::eOk) {
        LOG_ERROR(Render_Vulkan, "NVIDIA Streamline failed to start (result {}); the game's own DLSS runs",
                  static_cast<int>(result));
        return false;
    }
    // Each feature's needs (extensions, extra queues), merged; a feature whose plugin is missing
    // is left out.
    state.instance_extensions.clear();
    state.device_extensions.clear();
    state.extra_graphics_queues = 0;
    state.extra_compute_queues = 0;
    const auto add_requirements = [&](sl::Feature feature, const char* name) {
        sl::FeatureRequirements requirements{};
        if (const sl::Result result = api.get_feature_requirements(feature, requirements);
            result != sl::Result::eOk) {
            LOG_WARNING(Render_Vulkan,
                        "Streamline found no {} plugin (result {}): put its sl.*.dll next to "
                        "sl.interposer.dll",
                        name, static_cast<int>(result));
            return false;
        }
        const auto merge = [](std::vector<std::string>& into, std::vector<std::string> names) {
            for (std::string& entry : names) {
                if (std::ranges::find(into, entry) == into.end()) {
                    into.push_back(std::move(entry));
                }
            }
        };
        merge(state.instance_extensions,
              Names(requirements.vkNumInstanceExtensions, requirements.vkInstanceExtensions));
        merge(state.device_extensions,
              Names(requirements.vkNumDeviceExtensions, requirements.vkDeviceExtensions));
        state.extra_graphics_queues += requirements.vkNumGraphicsQueuesRequired;
        state.extra_compute_queues += requirements.vkNumComputeQueuesRequired;
        std::string features12;
        for (uint32_t i = 0; i < requirements.vkNumFeatures12; ++i) {
            features12 += fmt::format(" {}", requirements.vkFeatures12[i]);
        }
        std::string features13;
        for (uint32_t i = 0; i < requirements.vkNumFeatures13; ++i) {
            features13 += fmt::format(" {}", requirements.vkFeatures13[i]);
        }
        LOG_INFO(Render_Vulkan,
                 "Streamline {}: {} graphics and {} compute queues, {} optical flow queues; "
                 "Vulkan 1.2 features:{}; 1.3 features:{}",
                 name, requirements.vkNumGraphicsQueuesRequired,
                 requirements.vkNumComputeQueuesRequired,
                 requirements.vkNumOpticalFlowQueuesRequired, features12.empty() ? " none" : features12,
                 features13.empty() ? " none" : features13);
        return true;
    };
    state.want_dlss = dlss_wanted && add_requirements(sl::kFeatureDLSS, "DLSS");
    state.want_frame_gen = frame_gen_wanted && add_requirements(sl::kFeatureDLSS_G, "DLSS frame generation") &&
                           add_requirements(sl::kFeatureReflex, "Reflex") &&
                           add_requirements(sl::kFeaturePCL, "PC latency");
    if (!state.want_dlss && !state.want_frame_gen) {
        LOG_WARNING(Render_Vulkan, "No NVIDIA Streamline feature can run; the game's own DLSS runs");
        api.shutdown();
        return false;
    }
    if (state.want_dlss && (state.extra_graphics_queues != 0 || state.extra_compute_queues != 0) &&
        !state.want_frame_gen) {
        LOG_WARNING(Render_Vulkan, "This DLSS wants extra Vulkan queues ({} graphics, {} compute)",
                    state.extra_graphics_queues, state.extra_compute_queues);
    }
    // The KHR and EXT buffer device address extensions can't both be enabled.
    const auto& device_extensions = state.device_extensions;
    if (std::ranges::find(device_extensions, VK_KHR_BUFFER_DEVICE_ADDRESS_EXTENSION_NAME) !=
        device_extensions.end()) {
        std::erase(state.device_extensions, VK_EXT_BUFFER_DEVICE_ADDRESS_EXTENSION_NAME);
    }
    sl::FeatureVersion version{};
    api.get_feature_version(sl::kFeatureDLSS, version);
    LOG_INFO(Render_Vulkan, "NVIDIA Streamline {} started from {} (DLSS {})",
             version.versionSL.toStr(),
             Common::FS::PathToUTF8String(std::filesystem::path{state.plugin_dir}),
             version.versionNGX.toStr());
    state.started = true;
    return true;
}

void* HookedFunction(const char* name) {
    const State& state = GetState();
    if (!state.started || !state.module) {
        return nullptr;
    }
    return reinterpret_cast<void*>(GetProcAddress(state.module, name));
}

bool WantsDevice() {
    const State& state = GetState();
    return state.started && !state.attached;
}

std::vector<std::string> InstanceExtensions() {
    const State& state = GetState();
    return state.started ? state.instance_extensions : std::vector<std::string>{};
}

std::vector<std::string> DeviceExtensions() {
    const State& state = GetState();
    return state.started ? state.device_extensions : std::vector<std::string>{};
}

bool AttachDevice(VkInstance instance, VkPhysicalDevice physical, VkDevice device,
                  u32 graphics_family, vk::DeviceDispatch& dld) {
    State& state = GetState();
    if (!WantsDevice()) {
        return false;
    }
    sl::VulkanInfo info{};
    info.device = device;
    info.instance = instance;
    info.physicalDevice = physical;
    info.computeQueueFamily = graphics_family;
    info.graphicsQueueFamily = graphics_family;
    // Streamline's own queues come after the emulator's one in the graphics family (see
    // ExtraQueues): graphics first, then compute.
    info.graphicsQueueIndex = 1;
    info.computeQueueIndex = 1 + state.extra_graphics_queues;
    if (const sl::Result result = state.api.set_vulkan_info(info); result != sl::Result::eOk) {
        LOG_ERROR(Render_Vulkan, "Streamline did not take the Vulkan device (result {})",
                  static_cast<int>(result));
        Stop();
        return false;
    }
    sl::AdapterInfo adapter{};
    adapter.vkPhysicalDevice = physical;
    state.dlss_available = false;
    state.frame_gen_available = false;
    if (state.want_dlss) {
        const sl::Result result = state.api.is_feature_supported(sl::kFeatureDLSS, adapter);
        state.dlss_available = result == sl::Result::eOk;
        if (!state.dlss_available) {
            LOG_WARNING(Render_Vulkan,
                        "NVIDIA DLSS is not supported on this GPU or driver (Streamline result "
                        "{}); the game's own DLSS runs",
                        static_cast<int>(result));
        }
    }
    if (state.want_frame_gen) {
        state.frame_gen_available = true;
        if (const std::vector<std::string> missing = MissingFrameGenFiles(); !missing.empty()) {
            std::string names;
            for (const std::string& name : missing) {
                names += (names.empty() ? "" : ", ") + name;
            }
            LOG_WARNING(Render_Vulkan,
                        "NVIDIA DLSS frame generation needs NVIDIA's files, and {} are not in {}: "
                        "Settings > Graphics > Frame generation can download them",
                        names, Common::FS::PathToUTF8String(FindDlssFiles().folder));
        }
        for (const auto& [feature, name] :
             {std::pair{sl::kFeatureDLSS_G, "DLSS frame generation"},
              std::pair{sl::kFeatureReflex, "Reflex"}, std::pair{sl::kFeaturePCL, "PC latency"}}) {
            if (const sl::Result result = state.api.is_feature_supported(feature, adapter);
                result != sl::Result::eOk) {
                LOG_WARNING(Render_Vulkan,
                            "NVIDIA {} is not supported on this GPU, driver or Windows setup "
                            "(Streamline result {}); DLSS frame generation is off (it needs a "
                            "GeForce RTX 40 or 50 and hardware-accelerated GPU scheduling)",
                            name, static_cast<int>(result));
                state.frame_gen_available = false;
            }
        }
    }
    if (!state.dlss_available && !state.frame_gen_available) {
        Stop();
        return false;
    }
    // Manual hooking: Streamline does its per-frame work in these.
    const auto hook = [&](auto& function, const char* name) {
        if (const FARPROC proxy = GetProcAddress(state.module, name)) {
            function = reinterpret_cast<std::remove_reference_t<decltype(function)>>(proxy);
        } else {
            LOG_WARNING(Render_Vulkan, "sl.interposer.dll has no {}", name);
        }
    };
    // EDEN_DLSS_SWAPCHAIN_HOOKS=0 leaves the swapchain calls to the driver (crash diagnosis).
    static const bool swapchain_hooks = [] {
        const char* const value = std::getenv("EDEN_DLSS_SWAPCHAIN_HOOKS");
        return value == nullptr || value[0] != '0';
    }();
    if (swapchain_hooks) {
        hook(dld.vkCreateSwapchainKHR, "vkCreateSwapchainKHR");
        hook(dld.vkDestroySwapchainKHR, "vkDestroySwapchainKHR");
        hook(dld.vkGetSwapchainImagesKHR, "vkGetSwapchainImagesKHR");
        hook(dld.vkAcquireNextImageKHR, "vkAcquireNextImageKHR");
        hook(dld.vkQueuePresentKHR, "vkQueuePresentKHR");
        hook(dld.vkDeviceWaitIdle, "vkDeviceWaitIdle");
    } else {
        LOG_INFO(Render_Vulkan, "NVIDIA Streamline: swapchain calls not hooked");
    }
    state.attached = true;
    LOG_INFO(Render_Vulkan, "NVIDIA Streamline ready: DLSS {}, DLSS frame generation {}",
             state.dlss_available ? "available" : "off",
             state.frame_gen_available ? "available" : "off");
    return true;
}

bool IsDlssAvailable() {
    const State& state = GetState();
    return state.started && state.attached && state.dlss_available;
}

bool IsFrameGenerationAvailable() {
    const State& state = GetState();
    return state.started && state.attached && state.frame_gen_available;
}

ExtraQueues GetExtraQueues() {
    const State& state = GetState();
    if (!state.started) {
        return {};
    }
    return {state.extra_graphics_queues, state.extra_compute_queues};
}

u64 EvaluateFailures() {
    return evaluate_failures.load(std::memory_order_relaxed);
}

namespace {
void ResetFrameGen();
}

namespace {
std::atomic<bool> frame_gen_stopped{false};
}

bool CanStartAgain() {
    return !frame_gen_stopped.load(std::memory_order_relaxed);
}

void Stop() {
    // A game started again in the same session starts Streamline again: its plugins (and the
    // functions taken from them, and the Reflex options set on them) are new then.
    ResetFrameGen();
    State& state = GetState();
    if (!state.started) {
        return;
    }
    if (state.attached && state.frame_gen_available) {
        frame_gen_stopped.store(true, std::memory_order_relaxed);
    }
    const sl::Result result = state.api.shutdown();
    state.started = false;
    state.attached = false;
    LOG_INFO(Render_Vulkan, "NVIDIA Streamline stopped (result {})", static_cast<int>(result));
}

const Api& GetApi() {
    return GetState().api;
}

namespace {

struct FrameGenRuntime {
    PFun_slDLSSGSetOptions* set_options{};
    PFun_slDLSSGGetState* get_state{};
    PFun_slReflexSetOptions* reflex_set_options{};
    PFun_slReflexSleep* reflex_sleep{};
    PFun_slPCLSetMarker* set_marker{};
    bool loaded{};
    bool failed{};
    bool on{};
    u64 presents{};
    sl::ReflexMode reflex_mode{sl::ReflexMode::eLowLatency};
    u32 frame_limit_us{};
};

FrameGenRuntime& GetFrameGen() {
    static FrameGenRuntime runtime;
    return runtime;
}

s64 NowNs() {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(
               std::chrono::steady_clock::now().time_since_epoch())
        .count();
}

/// The last frame index whose tags were recorded, and when (GPU thread).
std::atomic<u64> last_sim_index{0};
std::atomic<s64> last_tag_ns{0};
/// When Reflex last slept at the game's vsync (FefwFrameGenHook).
std::atomic<s64> vsync_paced_ns{0};
std::mutex reflex_options_mutex;
/// Held while the vsync thread uses the runtime, so ResetFrameGen never clears it under it.
std::mutex vsync_sleep_mutex;

constexpr s64 FRAME_GEN_IDLE_NS = 100'000'000;

bool VsyncPaced() {
    return NowNs() - vsync_paced_ns.load(std::memory_order_relaxed) < FRAME_GEN_IDLE_NS;
}

void ApplyFrameLimit(FrameGenRuntime& fg, u32 limit_us) {
    std::scoped_lock lock{reflex_options_mutex};
    if (limit_us == fg.frame_limit_us) {
        return;
    }
    sl::ReflexOptions reflex{};
    reflex.mode = fg.reflex_mode;
    reflex.frameLimitUs = limit_us;
    const sl::Result result = fg.reflex_set_options(reflex);
    LOG_INFO(Render_Vulkan, "NVIDIA Reflex frame limit: {} (result {})",
             limit_us ? fmt::format("{:.1f} FPS shown", 1e6 / limit_us) : std::string{"off"},
             static_cast<int>(result));
    fg.frame_limit_us = limit_us;
}

sl::FrameToken* Token(u64 index);
void Marker(sl::PCLMarker marker, const sl::FrameToken& token);

/// The vsync thread, before each vsync it signals: NVIDIA Reflex's sleep, where a native game
/// calls it (the start of its frame), with Reflex's frame limit as the only pacer: exactly the
/// frame generation rate, as a native game runs Reflex to its cap. Nothing while frame
/// generation isn't making frames. Not for the first 8 vsyncs after it starts again (Reflex's
/// first sleeps then were 65-90 ms). EDEN_DLSSG_VSYNC_SLEEP=0: Reflex sleeps on the GPU thread
/// with the vsync timer pacing the game, as before.
bool VsyncSleep() {
    std::scoped_lock vsync_lock{vsync_sleep_mutex};
    FrameGenRuntime& fg = GetFrameGen();
    if (!fg.loaded || fg.failed || frame_gen_stopped.load(std::memory_order_relaxed)) {
        return false;
    }
    const s64 now = NowNs();
    if (now - last_tag_ns.load(std::memory_order_relaxed) > FRAME_GEN_IDLE_NS) {
        return false;
    }
    static s64 last_sleep_ns = 0;
    static u32 skip_left = 0;
    if (now - last_sleep_ns > FRAME_GEN_IDLE_NS) {
        skip_left = 8;
    }
    last_sleep_ns = now;
    // The frame the game starts now. This vsync first composes the frame the game finished
    // (index last + 1, tagged on the GPU thread shortly after), so the game's new one is last + 2
    // (88c used last + 1: the start of the frame about to be composited, marked just before its
    // end, so Reflex still saw almost no simulation time). Never the same index twice.
    static u64 last_vsync_index = 0;
    const u64 index =
        (std::max)(last_sim_index.load(std::memory_order_relaxed) + 2, last_vsync_index + 1);
    last_vsync_index = index;
    sl::FrameToken* const token = Token(index);
    if (!token) {
        return false;
    }
    // With the vsync timer pacing (as with AMD FSR 3.1: its fixed schedule kept the game's frames
    // 16.8 ms apart, where Reflex's sleeps varied them 16.4-17.0 ms), Reflex's limit is 2% above
    // it, so it never holds a frame back; with Reflex pacing, exactly the rate.
    const bool timer_paces = FefwFrameGenHook::timer_paces.load(std::memory_order_relaxed);
    const s64 shown = static_cast<s64>(Settings::values.mfg_multiplier.GetValue()) + 2;
    const s64 period_ns = Settings::FrameGenerationPeriodNs();
    ApplyFrameLimit(fg,
                    static_cast<u32>(period_ns / 1000 / shown * (timer_paces ? 98 : 100) / 100));
    // Reflex stuck holding the game back: after a loading screen (game frames 51 ms apart),
    // Reflex kept sleeping the whole 51.5 ms of every vsync for 100 s, the game at 19.4 FPS with
    // the card and CPU nearly idle, until a 100 ms gap made Streamline reset its frame timer
    // (#63, an RTX 4080 Laptop entering the Dagsion sewers). Its own sleeps fed the frame time it
    // paced to. After 30 sleeps in a row of over 40 ms (2.4 vsyncs; the game is never that slow
    // at a steady 60 Hz), its options are set again and 8 vsyncs go without a sleep, so it measures
    // the game's real frames again.
    // It also got stuck at a gentler rate: on a long battle map Reflex's sleeps grew over a few
    // minutes from under 0.5 ms on average to 7, then 17 ms (the game at 54-58 FPS), then to
    // 20-23 ms at every vsync (9.99 s of every 10), the game at exactly 44.2 FPS with the card
    // half busy, and the 40 ms rule never saw it (#63, an RTX 4080 Laptop, v1.7.5.13). With the
    // vsync timer pacing, Reflex's limit is well above the game's rate and it should hardly sleep
    // (under 0.5 ms on average in 170 healthy 10 s spans of that log, a rare single sleep longer);
    // sleeps over 10 ms more than the frame rate cap asks for (a display that caps the game below
    // 60, say 60 Hz with 2x, has Reflex sleep the difference every frame) mean it is the one
    // pacing the game. Those count too, and a short sleep takes one off the count instead of
    // clearing it, so a stuck run with a few short sleeps in it is still caught while a lone long
    // sleep never adds up.
    static u32 long_sleeps = 0;
    static u32 unstuck_count = 0;
    if (skip_left > 0) {
        --skip_left;
    } else {
        const s64 sleep_start = NowNs();
        {
            FefwPresentTiming::Scope timing{FefwPresentTiming::ReflexSleep};
            fg.reflex_sleep(*token);
        }
        static constexpr s64 STUCK_SLEEP_NS = 40'000'000;
        static constexpr s64 TIMER_PACED_STUCK_SLEEP_NS = 10'000'000;
        static constexpr s64 VSYNC_NS = 16'666'667;
        const s64 capped_sleep_ns = (std::max<s64>)(period_ns - VSYNC_NS, 0);
        const s64 stuck_ns =
            timer_paces ? capped_sleep_ns + TIMER_PACED_STUCK_SLEEP_NS : STUCK_SLEEP_NS;
        if (NowNs() - sleep_start > stuck_ns) {
            ++long_sleeps;
        } else if (long_sleeps > 0) {
            --long_sleeps;
        }
        if (long_sleeps >= 30) {
            long_sleeps = 0;
            skip_left = 8;
            ++unstuck_count;
            {
                std::scoped_lock lock{reflex_options_mutex};
                sl::ReflexOptions reflex{};
                reflex.mode = fg.reflex_mode;
                reflex.frameLimitUs = fg.frame_limit_us;
                fg.reflex_set_options(reflex);
            }
            if (unstuck_count <= 5 || unstuck_count % 50 == 0) {
                LOG_WARNING(Render_Vulkan,
                            "NVIDIA Reflex slept over {:.1f} ms at 30 more vsyncs than not "
                            "(holding the game back): its options set again and 8 vsyncs "
                            "without a sleep ({} so far)",
                            static_cast<double>(stuck_ns) / 1e6, unstuck_count);
            }
        }
    }
    // The game's frame starts now: its simulation runs from here until the GPU thread has its
    // frame (FrameGenSimulationEnd). Marked on the GPU thread instead, Reflex saw almost no
    // simulation time, took the GPU to be the bottleneck and slept up to 30 ms a frame with the
    // card far from busy (test 88: the game at 53-55 with the vsync thread asleep 8-9 s in 10 s).
    Marker(sl::PCLMarker::eSimulationStart, *token);
    vsync_paced_ns.store(NowNs(), std::memory_order_relaxed);
    return true;
}

void ResetFrameGen() {
    FefwFrameGenHook::vsync_sleep.store(nullptr, std::memory_order_release);
    std::scoped_lock vsync_lock{vsync_sleep_mutex};
    GetFrameGen() = {};
}

bool LoadFrameGen() {
    FrameGenRuntime& fg = GetFrameGen();
    if (fg.loaded || fg.failed) {
        return fg.loaded;
    }
    if (!IsFrameGenerationAvailable()) {
        fg.failed = true;
        return false;
    }
    const Api& api = GetApi();
    const auto get = [&](sl::Feature feature, const char* name, auto*& function) {
        void* address{};
        const sl::Result result = api.get_feature_function(feature, name, address);
        if (result != sl::Result::eOk || !address) {
            LOG_ERROR(Render_Vulkan, "Streamline has no {} (result {})", name,
                      static_cast<int>(result));
            return false;
        }
        function = reinterpret_cast<std::remove_reference_t<decltype(function)>>(address);
        return true;
    };
    if (!get(sl::kFeatureDLSS_G, "slDLSSGSetOptions", fg.set_options) ||
        !get(sl::kFeatureDLSS_G, "slDLSSGGetState", fg.get_state) ||
        !get(sl::kFeatureReflex, "slReflexSetOptions", fg.reflex_set_options) ||
        !get(sl::kFeatureReflex, "slReflexSleep", fg.reflex_sleep) ||
        !get(sl::kFeaturePCL, "slPCLSetMarker", fg.set_marker)) {
        fg.failed = true;
        return false;
    }
    // Frame generation needs Reflex on ("sl.reflex must be enabled and active");
    // EDEN_DLSSG_REFLEX=0 turns it off (testing).
    sl::ReflexOptions reflex{};
    const char* const reflex_env = std::getenv("EDEN_DLSSG_REFLEX");
    reflex.mode = reflex_env && reflex_env[0] == '0' ? sl::ReflexMode::eOff
                                                      : sl::ReflexMode::eLowLatency;
    if (const sl::Result result = fg.reflex_set_options(reflex); result != sl::Result::eOk) {
        LOG_WARNING(Render_Vulkan, "Streamline rejected the Reflex options (result {})",
                    static_cast<int>(result));
    }
    fg.reflex_mode = reflex.mode;
    fg.loaded = true;
    LOG_INFO(Render_Vulkan, "NVIDIA DLSS frame generation ready");
    if (const char* const value = std::getenv("EDEN_DLSSG_VSYNC_SLEEP");
        !(value && value[0] == '0')) {
        const char* const pacer = std::getenv("EDEN_DLSSG_PACER");
        FefwFrameGenHook::timer_paces.store(!(pacer && std::string_view{pacer} == "reflex"),
                                            std::memory_order_relaxed);
        FefwFrameGenHook::vsync_sleep.store(&VsyncSleep, std::memory_order_release);
        LOG_INFO(Render_Vulkan, "NVIDIA Reflex: sleeping at the game's vsync, paced by {}",
                 FefwFrameGenHook::timer_paces.load(std::memory_order_relaxed)
                     ? "the vsync timer"
                     : "Reflex's frame limit");
    }
    return true;
}

sl::FrameToken* Token(u64 index) {
    sl::FrameToken* token{};
    uint32_t frame_index = static_cast<uint32_t>(index);
    if (GetApi().get_new_frame_token(token, &frame_index) != sl::Result::eOk) {
        return nullptr;
    }
    return token;
}

void Marker(sl::PCLMarker marker, const sl::FrameToken& token) {
    GetFrameGen().set_marker(marker, token);
}

sl::float4x4 Matrix(std::array<std::array<f32, 4>, 4> rows) {
    sl::float4x4 m;
    for (int i = 0; i < 4; ++i) {
        m[i] = sl::float4{rows[i][0], rows[i][1], rows[i][2], rows[i][3]};
    }
    return m;
}

sl::Resource FrameGenResource(const FrameGenImage& image) {
    sl::Resource resource{sl::ResourceType::eTex2d, image.image, nullptr, image.view,
                          static_cast<uint32_t>(VK_IMAGE_LAYOUT_GENERAL)};
    resource.width = image.size.width;
    resource.height = image.size.height;
    resource.nativeFormat = static_cast<uint32_t>(image.format);
    resource.mipLevels = 1;
    resource.arrayLayers = 1;
    resource.flags = 0;
    resource.usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_STORAGE_BIT |
                     VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    return resource;
}

} // Anonymous namespace

void FrameGenSleep(u64 index) {
    if (!LoadFrameGen()) {
        return;
    }
    FrameGenRuntime& fg = GetFrameGen();
    sl::FrameToken* const token = Token(index);
    if (!token) {
        return;
    }
    // Frame generation's pacing also as Reflex's frame limit, so the driver spaces the frames it
    // makes evenly (NVIDIA's frame generation is made to work with Reflex's limiter, not
    // others). The limit is on the frames shown, made ones included (test 75o set it to the
    // game's frames: 52 shown a second, the game at 17-20). EDEN_DLSSG_REFLEX_LIMIT=0 leaves it
    // off (testing).
    static const bool limit_off = [] {
        const char* const value = std::getenv("EDEN_DLSSG_REFLEX_LIMIT");
        return value && value[0] == '0';
    }();
    // A little above the game's own pacing (2% by default: 109 shown becomes about 111): at the
    // same rate the two limiters took turns, Reflex holding back each frame that came a fraction
    // early (1-5 s asleep in every 10 s at the cap), so the frames came unevenly. Reflex still
    // catches a game running ahead of its pacing. EDEN_DLSSG_REFLEX_MARGIN=<percent> (0: same rate).
    static const s64 margin_percent = [] {
        const char* const value = std::getenv("EDEN_DLSSG_REFLEX_MARGIN");
        return value ? static_cast<s64>(std::clamp(std::atoi(value), 0, 20)) : s64{2};
    }();
    const s64 shown = static_cast<s64>(Settings::values.mfg_multiplier.GetValue()) + 2;
    const u32 limit_us =
        limit_off ? 0U
                  : static_cast<u32>(Settings::FrameGenerationPeriodNs() / 1000 / shown *
                                     (100 - margin_percent) / 100);
    // With Reflex sleeping at the game's vsync, only the vsync thread sets the limit (both
    // setting it, it flipped between 119 and 121.4 FPS shown each time frame generation started),
    // sleeps and marks the simulation's start.
    const bool vsync_paced = VsyncPaced();
    if (vsync_paced) {
        return;
    }
    if (FefwFrameGenHook::vsync_sleep.load(std::memory_order_relaxed) == nullptr) {
        ApplyFrameLimit(fg, limit_us);
    }
    static const bool sleep_off = [] {
        const char* const value = std::getenv("EDEN_DLSSG_REFLEX_SLEEP");
        return value && value[0] == '0';
    }();
    // Not for the first frames after frame generation comes back on (menus, cutscenes and
    // standing still pause it): Reflex's first sleeps then were 65-90 ms, a hitch on each resume
    // (8 in 4 minutes on an RTX 4090). A sleep for a frame index that doesn't follow the last
    // one marks a resume. EDEN_DLSSG_RESUME_SKIP=<frames> (default 8; 0: sleep as before).
    static const u32 resume_skip = [] {
        const char* const value = std::getenv("EDEN_DLSSG_RESUME_SKIP");
        return value ? static_cast<u32>(std::clamp(std::atoi(value), 0, 120)) : 8U;
    }();
    static u64 last_index = 0;
    static u32 skip_left = 0;
    if (last_index != 0 && index != last_index + 1 && index != last_index) {
        skip_left = resume_skip;
    }
    last_index = index;
    // The frame's "simulation" is the GPU thread's work on it, from here to its tags.
    if (skip_left > 0) {
        --skip_left;
    } else if (!sleep_off && !vsync_paced) {
        FefwPresentTiming::Scope timing{FefwPresentTiming::ReflexSleep};
        fg.reflex_sleep(*token);
    }
    Marker(sl::PCLMarker::eSimulationStart, *token);
}

void FrameGenSimulationEnd(u64 index) {
    if (!LoadFrameGen()) {
        return;
    }
    sl::FrameToken* const token = Token(index);
    if (!token) {
        return;
    }
    Marker(sl::PCLMarker::eSimulationEnd, *token);
    last_sim_index.store(index, std::memory_order_relaxed);
    last_tag_ns.store(NowNs(), std::memory_order_relaxed);
}

void FrameGenTagFrame(VkCommandBuffer cmdbuf, const FrameGenInputs& in) {
    if (!LoadFrameGen()) {
        return;
    }
    sl::FrameToken* const token = Token(in.index);
    if (!token) {
        return;
    }
    const sl::ViewportHandle viewport{0u};
    Marker(sl::PCLMarker::eRenderSubmitStart, *token);

    // The camera isn't known: a plausible perspective (reversed, infinite depth) and motion that
    // includes the camera's.
    const f32 rw = static_cast<f32>(in.depth.size.width);
    const f32 rh = static_cast<f32>((std::max)(in.depth.size.height, 1U));
    const f32 aspect = rw / rh;
    constexpr f32 FOV = 1.0f; // vertical, radians
    constexpr f32 NEAR_Z = 0.1f;
    const f32 t = std::tan(FOV * 0.5f);
    sl::Constants constants{};
    constants.cameraViewToClip = Matrix({{{1.0f / (aspect * t), 0, 0, 0},
                                          {0, 1.0f / t, 0, 0},
                                          {0, 0, 0, 1},
                                          {0, 0, NEAR_Z, 0}}});
    constants.clipToCameraView = Matrix({{{aspect * t, 0, 0, 0},
                                          {0, t, 0, 0},
                                          {0, 0, 0, 1.0f / NEAR_Z},
                                          {0, 0, 1, 0}}});
    const sl::float4x4 identity = Matrix({{{1, 0, 0, 0}, {0, 1, 0, 0}, {0, 0, 1, 0}, {0, 0, 0, 1}}});
    constants.clipToLensClip = identity;
    constants.clipToPrevClip = identity;
    constants.prevClipToClip = identity;
    constants.jitterOffset = {in.jitter[0], in.jitter[1]};
    constants.mvecScale = {1.0f, 1.0f};
    constants.cameraPinholeOffset = {0.0f, 0.0f};
    constants.cameraPos = {0.0f, 0.0f, 0.0f};
    constants.cameraUp = {0.0f, 1.0f, 0.0f};
    constants.cameraRight = {1.0f, 0.0f, 0.0f};
    constants.cameraFwd = {0.0f, 0.0f, 1.0f};
    constants.cameraNear = NEAR_Z;
    constants.cameraFar = 100000.0f;
    constants.cameraFOV = FOV;
    constants.cameraAspectRatio = aspect;
    constants.depthInverted = sl::eTrue;
    constants.cameraMotionIncluded = sl::eTrue;
    constants.motionVectors3D = sl::eFalse;
    constants.reset = in.reset ? sl::eTrue : sl::eFalse;
    constants.motionVectorsJittered = sl::eFalse;
    const Api& api = GetApi();
    if (const sl::Result result = api.set_constants(constants, *token, viewport);
        result != sl::Result::eOk) {
        static bool logged{};
        if (!std::exchange(logged, true)) {
            LOG_ERROR(Render_Vulkan, "Streamline rejected the frame constants (result {})",
                      static_cast<int>(result));
        }
    }

    sl::Resource depth = FrameGenResource(in.depth);
    sl::Resource motion = FrameGenResource(in.motion_vectors);
    sl::Resource hudless = FrameGenResource(in.hudless);
    sl::Resource ui_alpha = FrameGenResource(in.ui_alpha);
    const sl::Extent render_area{0, 0, in.depth.size.width, in.depth.size.height};
    const sl::Extent display_area{0, 0, in.hudless.size.width, in.hudless.size.height};
    const sl::Extent picture{static_cast<uint32_t>(in.backbuffer_rect.offset.y),
                             static_cast<uint32_t>(in.backbuffer_rect.offset.x),
                             in.backbuffer_rect.extent.width, in.backbuffer_rect.extent.height};
    const sl::ResourceTag tags[] = {
        sl::ResourceTag{&depth, sl::kBufferTypeDepth, sl::ResourceLifecycle::eValidUntilPresent,
                        &render_area},
        sl::ResourceTag{&motion, sl::kBufferTypeMotionVectors,
                        sl::ResourceLifecycle::eValidUntilPresent, &render_area},
        sl::ResourceTag{&hudless, sl::kBufferTypeHUDLessColor,
                        sl::ResourceLifecycle::eValidUntilPresent, &display_area},
        sl::ResourceTag{&ui_alpha, sl::kBufferTypeUIAlpha,
                        sl::ResourceLifecycle::eValidUntilPresent, &display_area},
        sl::ResourceTag{nullptr, sl::kBufferTypeBackbuffer, sl::ResourceLifecycle{}, &picture},
    };
    if (const sl::Result result =
            api.set_tag_for_frame(*token, viewport, tags, static_cast<uint32_t>(std::size(tags)),
                                  reinterpret_cast<sl::CommandBuffer*>(cmdbuf));
        result != sl::Result::eOk) {
        static bool logged{};
        if (!std::exchange(logged, true)) {
            LOG_ERROR(Render_Vulkan, "Streamline rejected the frame generation inputs (result {})",
                      static_cast<int>(result));
        }
    }
    Marker(sl::PCLMarker::eRenderSubmitEnd, *token);
}

void FrameGenPresentStart(u64 index, bool tagged) {
    if (!LoadFrameGen()) {
        return;
    }
    FrameGenRuntime& fg = GetFrameGen();
    const sl::ViewportHandle viewport{0u};
    // Frame generation runs while presented frames are tagged; off (keeping its memory) when not
    // (menus, movies: no game motion).
    if (tagged != fg.on) {
        sl::DLSSGOptions options{};
        options.mode = tagged ? sl::DLSSGMode::eOn : sl::DLSSGMode::eOff;
        // Multi frame generation (Settings: 2x, 3x or 4x; EDEN_DLSSG_FRAMES=1-3 for testing): that
        // many generated frames per game frame minus one. If Streamline refuses (RTX 40 and older
        // without the MFG Unlocker), one (2x).
        const u32 frames_wanted = [] {
            if (const char* const value = std::getenv("EDEN_DLSSG_FRAMES")) {
                return static_cast<u32>(std::clamp(std::atoi(value), 1, 3));
            }
            return static_cast<u32>(Settings::values.mfg_multiplier.GetValue()) + 1;
        }();
        options.numFramesToGenerate = frames_wanted;
        options.flags = sl::DLSSGFlags::eRetainResourcesWhenOff;
        const char* const recompose = std::getenv("EDEN_DLSSG_UI_RECOMPOSITION");
        options.enableUserInterfaceRecomposition =
            recompose && recompose[0] == '1' ? sl::Boolean::eTrue : sl::Boolean::eFalse;
        sl::Result result = fg.set_options(viewport, options);
        if (result != sl::Result::eOk && options.numFramesToGenerate > 1) {
            LOG_WARNING(Render_Vulkan,
                        "Streamline refused {} generated frames per game frame ({}); using 1",
                        options.numFramesToGenerate, static_cast<int>(result));
            options.numFramesToGenerate = 1;
            result = fg.set_options(viewport, options);
        }
        if (result != sl::Result::eOk) {
            LOG_ERROR(Render_Vulkan, "Streamline rejected the DLSS frame generation options ({})",
                      static_cast<int>(result));
        } else {
            LOG_INFO(Render_Vulkan, "NVIDIA DLSS frame generation {} ({} generated a frame asked)",
                     tagged ? "on" : "paused", options.numFramesToGenerate);
        }
        fg.on = tagged;
    }
    if (sl::FrameToken* const token = Token(index)) {
        Marker(sl::PCLMarker::ePresentStart, *token);
    }
}

void FrameGenPresentEnd(u64 index, bool tagged) {
    if (!LoadFrameGen()) {
        return;
    }
    FrameGenRuntime& fg = GetFrameGen();
    if (sl::FrameToken* const token = Token(index)) {
        Marker(sl::PCLMarker::ePresentEnd, *token);
    }
    // About every 10 s at 60 FPS: how it is doing.
    if (tagged && ++fg.presents % 600 == 1) {
        sl::DLSSGState state{};
        if (fg.get_state(sl::ViewportHandle{0u}, state, nullptr) == sl::Result::eOk) {
            // For the settings: whether 3x and 4x were available the last time.
            Settings::values.dlss_mfg_max_seen.SetValue(state.numFramesToGenerateMax);
            LOG_INFO(Render_Vulkan,
                     "NVIDIA DLSS frame generation: status {:#x}, {} frames presented for the "
                     "last, up to {} generated a frame, multi frame generation {}",
                     static_cast<u32>(state.status), state.numFramesActuallyPresented,
                     state.numFramesToGenerateMax,
                     state.bIsDynamicMFGSupported == sl::Boolean::eTrue ? "dynamic" : "fixed");
        }
    }
}

void FrameGenBeforeSwapchainRecreate() {
    FrameGenRuntime& fg = GetFrameGen();
    if (!fg.loaded || !fg.on) {
        return;
    }
    const sl::ViewportHandle viewport{0u};
    sl::DLSSGOptions options{};
    options.mode = sl::DLSSGMode::eOff;
    options.numFramesToGenerate = 1;
    if (const sl::Result result = fg.set_options(viewport, options); result != sl::Result::eOk) {
        LOG_ERROR(Render_Vulkan, "Streamline rejected turning DLSS frame generation off ({})",
                  static_cast<int>(result));
    }
    if (const sl::Result result = GetApi().free_resources(sl::kFeatureDLSS_G, viewport);
        result != sl::Result::eOk) {
        LOG_WARNING(Render_Vulkan, "Streamline could not free DLSS frame generation ({})",
                    static_cast<int>(result));
    }
    fg.on = false;
    LOG_INFO(Render_Vulkan, "NVIDIA DLSS frame generation off while the swapchain is made again");
}

} // namespace Vulkan::Streamline
