// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

// SPDX-FileCopyrightText: 2023 yuzu Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <algorithm>
#include <array>
#include <filesystem>
#include <optional>
#include "common/fefw_hidden_settings.h"
#include "common/fs/fs.h"
#include "common/fs/fs_paths.h"
#include "common/fs/path_util.h"
#include "common/logging.h"
#include "common/settings.h"
#include "common/settings_common.h"
#include "common/settings_enums.h"
#include "config.h"

#include "common/string_util.h"

#include <boost/algorithm/string/replace.hpp>

#include "common/assert.h"

namespace FS = Common::FS;

namespace {

#ifdef _WIN32
/// The portable "user" folder next to the exe, when this build runs portable.
std::optional<std::filesystem::path> OwnPortableUserDir() {
    auto user_dir = FS::GetExeDirectory() / "user";
    if (!FS::IsDir(user_dir)) {
        return std::nullopt;
    }
    return user_dir.lexically_normal();
}

/// Case-insensitive (ASCII) comparison of two paths, ignoring separators' style and a trailing
/// one. A false "different" only makes this build use its own default folder.
bool SamePath(const std::filesystem::path& a, const std::filesystem::path& b) {
    auto text = [](const std::filesystem::path& path) {
        const auto u8 = path.lexically_normal().generic_u8string();
        std::string out(u8.begin(), u8.end());
        while (!out.empty() && out.back() == '/') {
            out.pop_back();
        }
        std::transform(out.begin(), out.end(), out.begin(), [](char c) {
            return c >= 'A' && c <= 'Z' ? static_cast<char>(c - 'A' + 'a') : c;
        });
        return out;
    };
    return text(a) == text(b);
}

std::filesystem::path PathFromUTF8(const std::string& text) {
    return std::filesystem::path{std::u8string(text.begin(), text.end())};
}
#endif

/// Portable builds write their absolute folder paths into qt-config.ini, so a "user" folder
/// copied into a new build (as its README says to do when updating) kept using the NAND, SD,
/// mod and dump folders of the build it came from, and Open Save Data Location opened the old
/// build's saves. A stored path that is another portable build's own folder of the same name
/// (...\user\nand and so on) is ignored, and paths inside this build's own "user" folder are
/// stored empty (the default), so the folder keeps working wherever it is copied. Paths
/// anywhere else (a folder the player chose) are kept as before.
std::string PortableDataPath(const std::string& stored, const char* leaf, bool* from_other_build = nullptr) {
    if (from_other_build != nullptr) {
        *from_other_build = false;
    }
#ifdef _WIN32
    if (stored.empty()) {
        return stored;
    }
    const auto own = OwnPortableUserDir();
    if (!own) {
        return stored;
    }
    const std::filesystem::path path = PathFromUTF8(stored);
    const auto normal = path.lexically_normal();
    auto parent = normal.parent_path();
    auto name = normal.filename();
    if (name.empty()) { // Trailing separator
        name = parent.filename();
        parent = parent.parent_path();
    }
    const bool other_portable = SamePath(name, std::filesystem::path{leaf}) &&
                                SamePath(parent.filename(), std::filesystem::path{"user"}) &&
                                !SamePath(parent, *own);
    if (other_portable) {
        LOG_WARNING(Config, "Ignoring {} from another build's user folder: {} (using {})", leaf,
                    stored, FS::PathToUTF8String(*own / leaf));
        if (from_other_build != nullptr) {
            *from_other_build = true;
        }
        return {};
    }
#endif
    return stored;
}

/// What to store for a folder: empty when it is this portable build's own default folder.
std::string PortableStoredPath(const std::string& current, const char* leaf) {
#ifdef _WIN32
    if (const auto own = OwnPortableUserDir();
        own && SamePath(PathFromUTF8(current), *own / leaf)) {
        return {};
    }
#endif
    return current;
}

} // Anonymous namespace

Config::Config(const ConfigType config_type)
    : type(config_type), global{config_type == ConfigType::GlobalConfig} {}

void Config::Initialize(const std::string& config_name) {
    const std::filesystem::path fs_config_loc = FS::GetEdenPath(FS::EdenPath::ConfigDir);
    const auto config_file = fmt::format("{}.ini", config_name);

    switch (type) {
    case ConfigType::GlobalConfig:
        config_loc = FS::PathToUTF8String(fs_config_loc / config_file);
        void(FS::CreateParentDir(config_loc));
        SetUpIni();
        Reload();
        break;
    case ConfigType::PerGameConfig:
        config_loc = FS::PathToUTF8String(fs_config_loc / "custom" / FS::ToU8String(config_file));
        void(FS::CreateParentDir(config_loc));
        SetUpIni();
        Reload();
        break;
    case ConfigType::InputProfile:
        config_loc = FS::PathToUTF8String(fs_config_loc / "input" / config_file);
        void(FS::CreateParentDir(config_loc));
        SetUpIni();
        break;
    }
}

void Config::Initialize(const std::optional<std::string> config_path) {
    const std::filesystem::path default_sdl_config_path =
        FS::GetEdenPath(FS::EdenPath::ConfigDir) / "sdl2-config.ini";
    config_loc = config_path.value_or(FS::PathToUTF8String(default_sdl_config_path));
    void(FS::CreateParentDir(config_loc));
    SetUpIni();
    Reload();
}

void Config::WriteToIni() const {
    std::string config_type;
    switch (type) {
    case ConfigType::GlobalConfig:
        config_type = "Global";
        break;
    case ConfigType::PerGameConfig:
        config_type = "Game Specific";
        break;
    case ConfigType::InputProfile:
        config_type = "Input Profile";
        break;
    }
    LOG_INFO(Config, "Writing {} configuration to: {}", config_type, config_loc);
    FILE* fp = nullptr;
#ifdef _WIN32
    fp = _wfopen(Common::UTF8ToUTF16W(config_loc).data(), L"wb");
#else
    fp = fopen(config_loc.c_str(), "wb");
#endif

    if (fp == nullptr) {
        LOG_ERROR(Frontend, "Config file could not be saved!");
        return;
    }

    CSimpleIniA::FileWriter writer(fp);
    const SI_Error rc = config->Save(writer, false);
    if (rc < 0) {
        LOG_ERROR(Frontend, "Config file could not be saved!");
    }
    fclose(fp);
}

void Config::SetUpIni() {
    config = std::make_unique<CSimpleIniA>();
    config->SetUnicode(true);
    config->SetSpaces(false);

    FILE* fp = nullptr;
#ifdef _WIN32
    _wfopen_s(&fp, Common::UTF8ToUTF16W(config_loc).data(), L"rb, ccs=UTF-8");
    if (fp == nullptr) {
        fp = _wfopen(Common::UTF8ToUTF16W(config_loc).data(), L"wb, ccs=UTF-8");
    }
#else
    fp = fopen(config_loc.c_str(), "rb");
    if (fp == nullptr) {
        fp = fopen(config_loc.c_str(), "wb");
    }
#endif

    if (fp == nullptr) {
        LOG_ERROR(Frontend, "Config file could not be loaded!");
        return;
    }

    if (SI_Error rc = config->LoadFile(fp); rc < 0) {
        LOG_ERROR(Frontend, "Config file could not be loaded!");
    }
    fclose(fp);
}

bool Config::IsCustomConfig() const {
    return type == ConfigType::PerGameConfig;
}

void Config::ReadPlayerValues(const std::size_t player_index) {
    std::string player_prefix;
    if (type != ConfigType::InputProfile) {
        player_prefix.append("player_").append(ToString(player_index)).append("_");
    }

    const auto profile_name = ReadStringSetting(std::string(player_prefix).append("profile_name"));

    auto& player = Settings::values.players.GetValue()[player_index];
    if (IsCustomConfig()) {
        if (profile_name.empty()) {
            // Use the global input config
            player = Settings::values.players.GetValue(true)[player_index];
            player.profile_name = "";
            return;
        }
        player.profile_name = profile_name;
    }

    if (player_prefix.empty() && Settings::IsConfiguringGlobal()) {
        const auto controller = Settings::ControllerType(
            ReadIntegerSetting(std::string(player_prefix).append("type"),
                               u8(Settings::ControllerType::ProController)));

        if (controller == Settings::ControllerType::LeftJoycon ||
            controller == Settings::ControllerType::RightJoycon) {
            player.controller_type = controller;
        }
    } else {
        if (global) {
            auto& player_global = Settings::values.players.GetValue(true)[player_index];
            player_global.profile_name = profile_name;
        }
        std::string connected_key = player_prefix;
        player.connected = ReadBooleanSetting(connected_key.append("connected"),
                                              std::make_optional(player_index == 0));

        player.controller_type = Settings::ControllerType(
            ReadIntegerSetting(std::string(player_prefix).append("type"),
                               u8(Settings::ControllerType::ProController)));

        player.vibration_enabled = ReadBooleanSetting(
            std::string(player_prefix).append("vibration_enabled"), std::make_optional(true));

        player.vibration_strength =
            int(ReadIntegerSetting(std::string(player_prefix).append("vibration_strength"), 100));

        player.body_color_left = u32(ReadIntegerSetting(
            std::string(player_prefix).append("body_color_left"), Settings::JOYCON_BODY_NEON_BLUE));
        player.body_color_right = u32(ReadIntegerSetting(
            std::string(player_prefix).append("body_color_right"), Settings::JOYCON_BODY_NEON_RED));
        player.button_color_left =
            u32(ReadIntegerSetting(std::string(player_prefix).append("button_color_left"),
                                   Settings::JOYCON_BUTTONS_NEON_BLUE));
        player.button_color_right =
            u32(ReadIntegerSetting(std::string(player_prefix).append("button_color_right"),
                                   Settings::JOYCON_BUTTONS_NEON_RED));
    }
}

void Config::ReadTouchscreenValues() {
    Settings::values.touchscreen.enabled =
        ReadBooleanSetting(std::string("touchscreen_enabled"), std::make_optional(true));
    Settings::values.touchscreen.rotation_angle =
        u32(ReadIntegerSetting(std::string("touchscreen_angle"), 0));
    Settings::values.touchscreen.diameter_x =
        u32(ReadIntegerSetting(std::string("touchscreen_diameter_x"), 90));
    Settings::values.touchscreen.diameter_y =
        u32(ReadIntegerSetting(std::string("touchscreen_diameter_y"), 90));
}

void Config::ReadAudioValues() {
    BeginGroup(Settings::TranslateCategory(Settings::Category::Audio));

    ReadCategory(Settings::Category::Audio);
    ReadCategory(Settings::Category::UiAudio);

    EndGroup();
}

void Config::ReadControlValues() {
    BeginGroup(Settings::TranslateCategory(Settings::Category::Controls));

    ReadCategory(Settings::Category::Controls);

    Settings::values.players.SetGlobal(!IsCustomConfig());
    for (std::size_t p = 0; p < Settings::values.players.GetValue().size(); ++p) {
        ReadPlayerValues(p);
    }

    // Disable docked mode if handheld is selected
    const auto controller_type = Settings::values.players.GetValue()[0].controller_type;
    if (controller_type == Settings::ControllerType::Handheld) {
        Settings::values.use_docked_mode.SetGlobal(!IsCustomConfig());
        Settings::values.use_docked_mode.SetValue(Settings::ConsoleMode::Handheld);
    }

    if (IsCustomConfig()) {
        EndGroup();
        return;
    }
    ReadTouchscreenValues();
    ReadMotionTouchValues();

    EndGroup();
}

void Config::ReadMotionTouchValues() {
    Settings::values.touch_from_button_maps.clear();
    int num_touch_from_button_maps = BeginArray(std::string("touch_from_button_maps"));
    if (num_touch_from_button_maps > 0) {
        for (int i = 0; i < num_touch_from_button_maps; ++i) {
            SetArrayIndex(i);
            Settings::TouchFromButtonMap map;
            map.name = ReadStringSetting(std::string("name"), std::string("default"));
            int const num_touch_maps = BeginArray(std::string("entries"));
            map.buttons.resize(num_touch_maps);
            for (int j = 0; j < num_touch_maps; j++) {
                SetArrayIndex(j);
                map.buttons[j] = ReadStringSetting(std::string("bind"));
            }
            EndArray(); // entries
            Settings::values.touch_from_button_maps.emplace_back(std::move(map));
        }
    } else {
        Settings::values.touch_from_button_maps.emplace_back(Settings::TouchFromButtonMap{"default", {}});
        num_touch_from_button_maps = 1;
    }
    EndArray(); // touch_from_button_maps

    Settings::values.touch_from_button_map_index = (std::min)(Settings::values.touch_from_button_map_index.GetValue(), u32(num_touch_from_button_maps - 1));
}

void Config::ReadCoreValues() {
    BeginGroup(Settings::TranslateCategory(Settings::Category::Core));

    ReadCategory(Settings::Category::Core);

    EndGroup();
}

void Config::ReadDataStorageValues() {
    BeginGroup(Settings::TranslateCategory(Settings::Category::DataStorage));

    using namespace Common::FS;

    const auto setPath = [this](const EdenPath& path, const char* setting, const char* leaf) {
        const auto value = PortableDataPath(ReadStringSetting(std::string(setting)), leaf);
        if (!value.empty()) { // Empty: the default folder
            SetEdenPath(path, value);
        }
    };

    setPath(EdenPath::NANDDir, "nand_directory", NAND_DIR);
    setPath(EdenPath::SDMCDir, "sdmc_directory", SDMC_DIR);
    setPath(EdenPath::LoadDir, "load_directory", LOAD_DIR);
    setPath(EdenPath::DumpDir, "dump_directory", DUMP_DIR);
    setPath(EdenPath::TASDir, "tas_directory", TAS_DIR);

    bool save_from_other_build = false;
    const auto save_dir_setting = PortableDataPath(
        ReadStringSetting(std::string("save_directory")), NAND_DIR, &save_from_other_build);
#ifdef _WIN32
    // The save folder is stored absolute whenever it differs from the NAND folder, so another
    // build's user\nand here means "the portable save folder": use this build's own one, not a
    // custom NAND folder the player may also have set.
    if (save_from_other_build) {
        if (const auto own = OwnPortableUserDir()) {
            SetEdenPath(EdenPath::SaveDir, FS::PathToUTF8String(*own / NAND_DIR));
        } else {
            SetEdenPath(EdenPath::SaveDir, GetEdenPathString(EdenPath::NANDDir));
        }
    } else
#endif
    if (save_dir_setting.empty()) {
        SetEdenPath(EdenPath::SaveDir, GetEdenPathString(EdenPath::NANDDir));
    } else {
        SetEdenPath(EdenPath::SaveDir, save_dir_setting);
    }

    ReadCategory(Settings::Category::DataStorage);

    EndGroup();
}

void Config::ReadDebuggingValues() {
    BeginGroup(Settings::TranslateCategory(Settings::Category::Debugging));

    if (global) {
        // Intentionally not using the QT default setting as this is intended to be changed in the ini
        Settings::values.record_frame_times =
            ReadBooleanSetting(std::string("record_frame_times"), std::make_optional(false));

        ReadCategory(Settings::Category::Debugging);
        ReadCategory(Settings::Category::DebuggingGraphics);
    } else {
        ReadSettingGeneric(&Settings::values.program_args);
        ReadSettingGeneric(&Settings::values.debug_knobs);
    }

    EndGroup();
}

void Config::ReadServiceValues() {
    BeginGroup(Settings::TranslateCategory(Settings::Category::Services));

    ReadCategory(Settings::Category::Services);

    EndGroup();
}

void Config::ReadDisabledAddOnValues() {
    // Custom config section
    BeginGroup(std::string("DisabledAddOns"));

    const int size = BeginArray(std::string(""));
    for (int i = 0; i < size; ++i) {
        SetArrayIndex(i);
        const auto title_id = ReadUnsignedIntegerSetting(std::string("title_id"), 0);
        std::vector<std::string> out;
        const int d_size = BeginArray("disabled");
        for (int j = 0; j < d_size; ++j) {
            SetArrayIndex(j);
            out.push_back(ReadStringSetting(std::string("d"), std::string("")));
        }
        EndArray(); // d
        Settings::values.disabled_addons.insert_or_assign(title_id, out);
    }
    EndArray(); // Base disabled addons array - Has no base key

    EndGroup();
}

void Config::ReadMiscellaneousValues() {
    BeginGroup(Settings::TranslateCategory(Settings::Category::Miscellaneous));

    ReadCategory(Settings::Category::Miscellaneous);

    EndGroup();
}

void Config::ReadCpuValues() {
    BeginGroup(Settings::TranslateCategory(Settings::Category::Cpu));

    ReadCategory(Settings::Category::Cpu);
    ReadCategory(Settings::Category::CpuDebug);
    ReadCategory(Settings::Category::CpuUnsafe);

    EndGroup();
}

void Config::ReadRendererValues() {
    BeginGroup(Settings::TranslateCategory(Settings::Category::Renderer));

    ReadCategory(Settings::Category::Renderer);
    ReadCategory(Settings::Category::RendererAdvanced);
    ReadCategory(Settings::Category::RendererHacks);
    ReadCategory(Settings::Category::RendererExtensions);
    ReadCategory(Settings::Category::RendererDebug);

    // v1.6.8.1 and earlier: "fefw_ambient_occlusion", on by default. While there is no
    // fefw_no_ambient_occlusion entry yet, a saved "off" there (this file's own, or a game's own
    // choice) carries over to the new tick box.
    if (config->GetValue(GetSection().c_str(),
                         GetFullKey("fefw_no_ambient_occlusion", false).c_str(), nullptr) ==
            nullptr &&
        config->GetValue(GetSection().c_str(), GetFullKey("fefw_ambient_occlusion", false).c_str(),
                         nullptr) != nullptr) {
        auto& no_ambient_occlusion = Settings::values.fefw_no_ambient_occlusion;
        bool use_global = true;
        if (!global) {
            use_global = ReadBooleanSetting(std::string("fefw_ambient_occlusion\\use_global"),
                                            std::make_optional(true));
            no_ambient_occlusion.SetGlobal(use_global);
        }
        if (global || !use_global) {
            const bool ambient_occlusion =
                ReadBooleanSetting(std::string("fefw_ambient_occlusion"), std::make_optional(true));
            no_ambient_occlusion.SetValue(!ambient_occlusion);
            if (!ambient_occlusion) {
                LOG_INFO(Config, "Ambient occlusion stays off (from the old setting)");
            }
        }
    }

    // v1.6.2 and earlier had a tick box per upscaler. Keep a player's choice when there is no
    // upscaler entry yet: NVIDIA DLSS, AMD FSR 4 or AMD FSR 3.1 when ticked, else AMD FSR 3.1,
    // the default since v1.6.3.
    const bool from_tick_boxes =
        config->GetValue(GetSection().c_str(), GetFullKey("nvidia_dlss", false).c_str(),
                         nullptr) != nullptr;
    if (global && config->GetValue(GetSection().c_str(), GetFullKey("upscaler", false).c_str(),
                                   nullptr) == nullptr &&
        from_tick_boxes) {
        Settings::Upscaler migrated = Settings::Upscaler::Fsr3;
        if (ReadBooleanSetting(std::string("nvidia_dlss"), std::make_optional(false))) {
            migrated = Settings::Upscaler::NvidiaDlss;
        } else if (ReadBooleanSetting(std::string("amd_fsr4"), std::make_optional(false))) {
            migrated = Settings::Upscaler::Fsr4;
        } else if (ReadBooleanSetting(std::string("amd_fsr3"), std::make_optional(false))) {
            migrated = Settings::Upscaler::Fsr3;
        }
        Settings::values.upscaler.SetValue(migrated);
        LOG_INFO(Config, "Upscaler set from the old tick boxes: {}",
                 Settings::CanonicalizeEnum(migrated));
    }

    // v1.6.3 to v1.6.7.1 turned no ticked box into the game's own DLSS (the old default), so
    // everyone who updated from v1.6.2 or earlier was left on it instead of AMD FSR 3.1. Move
    // them to AMD FSR 3.1 once; choosing the game's own DLSS again after that is kept.
    if (global && from_tick_boxes &&
        !ReadBooleanSetting(std::string("fefw_upscaler_fsr3_applied"),
                            std::make_optional(false)) &&
        Settings::values.upscaler.GetValue() == Settings::Upscaler::GameDlss) {
        Settings::values.upscaler.SetValue(Settings::Upscaler::Fsr3);
        LOG_INFO(Config, "Upscaler moved from the game's own DLSS (set by an old migration) to "
                         "AMD FSR 3.1");
    }

    // v1.6.8.6: reactive flushing on. Off (the package's old setting) the game read stale copies
    // of what the GPU drew, and effects that follow it jumped (the dust behind the torches in
    // Leda's chapter 1 part 2 snapped every second or so); on, frame pacing was as smooth. Every
    // download's config saves it as chosen, so it is turned on here once; turned off after this,
    // it stays off.
    if (global && !ReadBooleanSetting(std::string("fefw_reactive_flushing_applied"),
                                      std::make_optional(false))) {
        if (!Settings::values.use_reactive_flushing.GetValue()) {
            Settings::values.use_reactive_flushing.SetValue(true);
            LOG_INFO(Config, "Reactive flushing turned on (new default)");
        }
    }

    EndGroup();
}

void Config::ReadScreenshotValues() {
    BeginGroup(Settings::TranslateCategory(Settings::Category::Screenshots));

    ReadCategory(Settings::Category::Screenshots);
    FS::SetEdenPath(FS::EdenPath::ScreenshotsDir,
                    ReadStringSetting(std::string("screenshot_path")));

    EndGroup();
}

void Config::ReadSystemValues() {
    BeginGroup(Settings::TranslateCategory(Settings::Category::System));

    ReadCategory(Settings::Category::System);
    ReadCategory(Settings::Category::SystemAudio);

    EndGroup();
}

void Config::ReadWebServiceValues() {
    BeginGroup(Settings::TranslateCategory(Settings::Category::WebService));

    ReadCategory(Settings::Category::WebService);

    EndGroup();
}

void Config::ReadNetworkValues() {
    BeginGroup(Settings::TranslateCategory(Settings::Category::Services));

    ReadCategory(Settings::Category::Network);

    EndGroup();
}

void Config::ReadLibraryAppletValues() {
    BeginGroup(Settings::TranslateCategory(Settings::Category::LibraryApplet));

    ReadCategory(Settings::Category::LibraryApplet);

    EndGroup();
}

void Config::ReadValues() {
    if (global) {
        ReadDataStorageValues();
        ReadDisabledAddOnValues();
        ReadServiceValues();
        ReadWebServiceValues();
        ReadMiscellaneousValues();
    }
    ReadDebuggingValues();
    ReadLibraryAppletValues();
    ReadNetworkValues();
    ReadControlValues();
    ReadCoreValues();
    ReadCpuValues();
    ReadRendererValues();
    ReadAudioValues();
    ReadSystemValues();

    // Fortune's Eden: settings no longer shown that have one right value for Fortune's Weave.
    // Always those, whatever an older config file or a game's own config says.
    if (global) {
        MigrateFefwSixtyAddon();
        // Settings that aren't shown go back to their defaults (Vulkan, multi-core, 16:9, no
        // anti-aliasing, Bilinear, Auto pacing, 12 GB, GPU mode Fast, ASTC recompression Auto...):
        // a value set in an older version can't stay stuck where nobody can change it.
        for (const auto& [category, settings] : Settings::values.linkage.by_category) {
            for (Settings::BasicSetting* const setting : settings) {
                if (Common::IsFefwResetSetting(setting->GetLabel())) {
                    setting->LoadString(setting->DefaultToString());
                }
            }
        }
    } else if (type != ConfigType::InputProfile) {
        // One game, one set of settings: a game's own config (an older version's Game Settings)
        // can't override Settings any more, except its controls (input profiles).
        for (const auto& [category, settings] : Settings::values.linkage.by_category) {
            if (category == Settings::Category::Controls) {
                continue;
            }
            for (Settings::BasicSetting* const setting : settings) {
                if (setting->Switchable()) {
                    setting->SetGlobal(true);
                }
            }
        }
    }
}

// v1.6.7.4 built the 60 FPS mode in (setting fefw_60fps). The old way was an add-on folder
// named "60fps" holding a cheat (load/04007EF01F8AE000/60fps/cheats/*.txt), which then showed
// as a second 60 FPS add-on next to the setting. The setting takes over its state (on unless the
// add-on was unticked), the folder is deleted, and its entry leaves the disabled add-ons list.
// Only a folder of small cheat text files is deleted: anything else in it is left alone.
void Config::MigrateFefwSixtyAddon() {
    static constexpr u64 TitleId = 0x04007EF01F8AE000;
    namespace fs = std::filesystem;
    std::error_code ec;
    const fs::path title_dir =
        FS::GetEdenPath(FS::EdenPath::LoadDir) / fmt::format("{:016X}", TitleId);
    if (!fs::is_directory(title_dir, ec)) {
        return;
    }
    for (const auto& entry : fs::directory_iterator(title_dir, ec)) {
        const std::string name = FS::PathToUTF8String(entry.path().filename());
        if (!entry.is_directory(ec) || Common::ToLower(name) != "60fps") {
            continue;
        }
        bool only_cheats = true;
        for (const auto& file : fs::recursive_directory_iterator(entry.path(), ec)) {
            if (file.is_directory(ec)) {
                continue;
            }
            const std::string extension =
                Common::ToLower(FS::PathToUTF8String(file.path().extension()));
            if ((extension != ".txt" && extension != ".pchtxt") || file.file_size(ec) > 64 * 1024) {
                only_cheats = false;
                break;
            }
        }
        if (ec || !only_cheats) {
            LOG_INFO(Config, "Old 60 FPS add-on folder {} has other files; left as it is", name);
            continue;
        }
        auto& disabled = Settings::values.disabled_addons[TitleId];
        const auto it = std::find(disabled.begin(), disabled.end(), name);
        const bool was_on = it == disabled.end();
        if (it != disabled.end()) {
            disabled.erase(it);
        }
        if (was_on) {
            Settings::values.fefw_60fps.SetValue(true);
        }
        fs::remove_all(entry.path(), ec);
        LOG_INFO(Config, "Old 60 FPS add-on removed ({}); 60 FPS mode {}",
                 ec ? "the folder couldn't be deleted" : "folder deleted",
                 was_on ? "turned on, as the add-on was" : "left off, as the add-on was");
        break;
    }
}

void Config::SavePlayerValues(const std::size_t player_index) {
    std::string player_prefix;
    if (type != ConfigType::InputProfile) {
        player_prefix = std::string("player_").append(ToString(player_index)).append("_");
    }

    const auto& player = Settings::values.players.GetValue()[player_index];
    if (IsCustomConfig()) {
        if (player.profile_name.empty()) {
            // No custom profile selected
            return;
        }
        WriteStringSetting(std::string(player_prefix).append("profile_name"), player.profile_name,
                           std::make_optional(std::string("")));
    }

    WriteIntegerSetting(std::string(player_prefix).append("type"), u8(player.controller_type),
                        std::make_optional(u8(Settings::ControllerType::ProController)));

    if (!player_prefix.empty() || !Settings::IsConfiguringGlobal()) {
        if (global) {
            const auto& player_global = Settings::values.players.GetValue(true)[player_index];
            WriteStringSetting(std::string(player_prefix).append("profile_name"),
                               player_global.profile_name, std::make_optional(std::string("")));
        }
        WriteBooleanSetting(std::string(player_prefix).append("connected"), player.connected,
                            std::make_optional(player_index == 0));
        WriteIntegerSetting(std::string(player_prefix).append("vibration_enabled"),
                            player.vibration_enabled, std::make_optional(true));
        WriteIntegerSetting(std::string(player_prefix).append("vibration_strength"),
                            player.vibration_strength, std::make_optional(100));
        WriteIntegerSetting(std::string(player_prefix).append("body_color_left"),
                            player.body_color_left,
                            std::make_optional(Settings::JOYCON_BODY_NEON_BLUE));
        WriteIntegerSetting(std::string(player_prefix).append("body_color_right"),
                            player.body_color_right,
                            std::make_optional(Settings::JOYCON_BODY_NEON_RED));
        WriteIntegerSetting(std::string(player_prefix).append("button_color_left"),
                            player.button_color_left,
                            std::make_optional(Settings::JOYCON_BUTTONS_NEON_BLUE));
        WriteIntegerSetting(std::string(player_prefix).append("button_color_right"),
                            player.button_color_right,
                            std::make_optional(Settings::JOYCON_BUTTONS_NEON_RED));
    }
}

void Config::SaveTouchscreenValues() {
    const auto& touchscreen = Settings::values.touchscreen;

    WriteBooleanSetting(std::string("touchscreen_enabled"), touchscreen.enabled,
                        std::make_optional(true));

    WriteIntegerSetting(std::string("touchscreen_angle"), touchscreen.rotation_angle,
                        std::make_optional(u32(0)));
    WriteIntegerSetting(std::string("touchscreen_diameter_x"), touchscreen.diameter_x,
                        std::make_optional(u32(90)));
    WriteIntegerSetting(std::string("touchscreen_diameter_y"), touchscreen.diameter_y,
                        std::make_optional(u32(90)));
}

void Config::SaveMotionTouchValues() {
    BeginArray(std::string("touch_from_button_maps"));
    for (std::size_t p = 0; p < Settings::values.touch_from_button_maps.size(); ++p) {
        SetArrayIndex(int(p));
        WriteStringSetting(std::string("name"), Settings::values.touch_from_button_maps[p].name, std::make_optional(std::string("default")));
        BeginArray(std::string("entries"));
        for (std::size_t q = 0; q < Settings::values.touch_from_button_maps[p].buttons.size();
             ++q) {
            SetArrayIndex(int(q));
            WriteStringSetting(std::string("bind"), Settings::values.touch_from_button_maps[p].buttons[q]);
        }
        EndArray(); // entries
    }
    EndArray(); // touch_from_button_maps
}

void Config::SaveValues() {
    if (global) {
        LOG_DEBUG(Config, "Saving global generic configuration values");
        SaveDataStorageValues();
        SaveDisabledAddOnValues();
        SaveWebServiceValues();
        SaveMiscellaneousValues();
    } else {
        LOG_DEBUG(Config, "Saving only generic configuration values");
    }
    SaveDebuggingValues();
    SaveLibraryAppletValues();
    SaveNetworkValues();
    SaveControlValues();
    SaveCoreValues();
    SaveCpuValues();
    SaveRendererValues();
    SaveAudioValues();
    SaveSystemValues();

    WriteToIni();
}

void Config::SaveAudioValues() {
    BeginGroup(Settings::TranslateCategory(Settings::Category::Audio));

    WriteCategory(Settings::Category::Audio);
    WriteCategory(Settings::Category::UiAudio);

    EndGroup();
}

void Config::SaveControlValues() {
    BeginGroup(Settings::TranslateCategory(Settings::Category::Controls));

    WriteCategory(Settings::Category::Controls);

    Settings::values.players.SetGlobal(!IsCustomConfig());
    for (std::size_t p = 0; p < Settings::values.players.GetValue().size(); ++p) {
        SavePlayerValues(p);
    }
    if (IsCustomConfig()) {
        EndGroup();
        return;
    }
    SaveTouchscreenValues();
    SaveMotionTouchValues();

    EndGroup();
}

void Config::SaveCoreValues() {
    BeginGroup(Settings::TranslateCategory(Settings::Category::Core));

    WriteCategory(Settings::Category::Core);

    EndGroup();
}

void Config::SaveDataStorageValues() {
    BeginGroup(Settings::TranslateCategory(Settings::Category::DataStorage));

    using namespace Common::FS;

    const auto writePath = [this](const char* setting, const EdenPath& path, const char* leaf) {
        const auto value = PortableStoredPath(FS::GetEdenPathString(path), leaf);
        WriteStringSetting(std::string(setting), value, std::make_optional(value));
    };

    writePath("nand_directory", EdenPath::NANDDir, NAND_DIR);
    writePath("sdmc_directory", EdenPath::SDMCDir, SDMC_DIR);
    writePath("load_directory", EdenPath::LoadDir, LOAD_DIR);
    writePath("dump_directory", EdenPath::DumpDir, DUMP_DIR);
    writePath("tas_directory", EdenPath::TASDir, TAS_DIR);

    const auto save_path = FS::GetEdenPathString(EdenPath::SaveDir);
    const auto nand_path = FS::GetEdenPathString(EdenPath::NANDDir);
    if (save_path == nand_path) {
        WriteStringSetting(std::string("save_directory"), std::string(""),
                           std::make_optional(std::string("")));
    } else {
        WriteStringSetting(std::string("save_directory"), save_path,
                           std::make_optional(std::string("")));
    }

    WriteCategory(Settings::Category::DataStorage);

    EndGroup();
}

void Config::SaveDebuggingValues() {
    BeginGroup(Settings::TranslateCategory(Settings::Category::Debugging));

    if (global) {
        // Intentionally not using the QT default setting as this is intended to be changed in the ini
        WriteBooleanSetting(std::string("record_frame_times"), Settings::values.record_frame_times);

        WriteCategory(Settings::Category::Debugging);
        WriteCategory(Settings::Category::DebuggingGraphics);
    } else {
        WriteSettingGeneric(&Settings::values.program_args);
        WriteSettingGeneric(&Settings::values.debug_knobs);
    }

    EndGroup();
}

void Config::SaveNetworkValues() {
    BeginGroup(Settings::TranslateCategory(Settings::Category::Services));

    WriteCategory(Settings::Category::Network);

    EndGroup();
}

void Config::SaveDisabledAddOnValues() {
    // Custom config section
    BeginGroup(std::string("DisabledAddOns"));

    int i = 0;
    BeginArray(std::string(""));
    for (const auto& elem : Settings::values.disabled_addons) {
        SetArrayIndex(i);
        WriteIntegerSetting(std::string("title_id"), elem.first, std::make_optional(u64(0)));
        BeginArray(std::string("disabled"));
        for (std::size_t j = 0; j < elem.second.size(); ++j) {
            SetArrayIndex(int(j));
            WriteStringSetting(std::string("d"), elem.second[j], std::make_optional(std::string("")));
        }
        EndArray(); // disabled
        ++i;
    }
    EndArray(); // Base disabled addons array - Has no base key

    EndGroup();
}

void Config::SaveMiscellaneousValues() {
    BeginGroup(Settings::TranslateCategory(Settings::Category::Miscellaneous));

    WriteCategory(Settings::Category::Miscellaneous);

    EndGroup();
}

void Config::SaveCpuValues() {
    BeginGroup(Settings::TranslateCategory(Settings::Category::Cpu));

    WriteCategory(Settings::Category::Cpu);
    WriteCategory(Settings::Category::CpuDebug);
    WriteCategory(Settings::Category::CpuUnsafe);

    EndGroup();
}

void Config::SaveRendererValues() {
    BeginGroup(Settings::TranslateCategory(Settings::Category::Renderer));

    WriteCategory(Settings::Category::Renderer);
    WriteCategory(Settings::Category::RendererAdvanced);
    WriteCategory(Settings::Category::RendererHacks);
    WriteCategory(Settings::Category::RendererExtensions);
    WriteCategory(Settings::Category::RendererDebug);
    if (global) {
        WriteBooleanSetting(std::string("fefw_upscaler_fsr3_applied"), true,
                            std::make_optional(false));
        WriteBooleanSetting(std::string("fefw_reactive_flushing_applied"), true,
                            std::make_optional(false));
    }

    EndGroup();
}

void Config::SaveScreenshotValues() {
    BeginGroup(Settings::TranslateCategory(Settings::Category::Screenshots));

    WriteStringSetting(std::string("screenshot_path"),
                       FS::GetEdenPathString(FS::EdenPath::ScreenshotsDir));
    WriteCategory(Settings::Category::Screenshots);

    EndGroup();
}

void Config::SaveSystemValues() {
    BeginGroup(Settings::TranslateCategory(Settings::Category::System));

    WriteCategory(Settings::Category::System);
    WriteCategory(Settings::Category::SystemAudio);

    EndGroup();
}

void Config::SaveWebServiceValues() {
    BeginGroup(Settings::TranslateCategory(Settings::Category::WebService));

    WriteCategory(Settings::Category::WebService);

    EndGroup();
}

void Config::SaveLibraryAppletValues() {
    BeginGroup(Settings::TranslateCategory(Settings::Category::LibraryApplet));

    WriteCategory(Settings::Category::LibraryApplet);

    EndGroup();
}

bool Config::ReadBooleanSetting(const std::string& key, const std::optional<bool> default_value) {
    std::string full_key = GetFullKey(key, false);
    if (!default_value.has_value()) {
        return config->GetBoolValue(GetSection().c_str(), full_key.c_str(), false);
    }

    if (config->GetBoolValue(GetSection().c_str(),
                             std::string(full_key).append("\\default").c_str(), false)) {
        return bool(default_value.value());
    } else {
        return config->GetBoolValue(GetSection().c_str(), full_key.c_str(),
                                    bool(default_value.value()));
    }
}

s64 Config::ReadIntegerSetting(const std::string& key, const std::optional<s64> default_value) {
    std::string full_key = GetFullKey(key, false);
    if (!default_value.has_value()) {
        try {
            return std::stoll(std::string(config->GetValue(GetSection().c_str(), full_key.c_str(), "0")));
        } catch (...) {
            return 0;
        }
    }

    s64 result = 0;
    if (config->GetBoolValue(GetSection().c_str(), std::string(full_key).append("\\default").c_str(), true)) {
        result = default_value.value();
    } else {
        try {
            result = std::stoll(std::string(config->GetValue(GetSection().c_str(), full_key.c_str(), ToString(default_value.value()).c_str())));
        } catch (...) {
            result = default_value.value();
        }
    }
    return result;
}

u64 Config::ReadUnsignedIntegerSetting(const std::string& key,
                                       const std::optional<u64> default_value) {
    std::string full_key = GetFullKey(key, false);
    if (!default_value.has_value()) {
        try {
            return std::stoull(
                std::string(config->GetValue(GetSection().c_str(), full_key.c_str(), "0")));
        } catch (...) {
            return 0;
        }
    }

    u64 result = 0;
    if (config->GetBoolValue(GetSection().c_str(),
                             std::string(full_key).append("\\default").c_str(), true)) {
        result = default_value.value();
    } else {
        try {
            result = std::stoull(std::string(config->GetValue(
                GetSection().c_str(), full_key.c_str(), ToString(default_value.value()).c_str())));
        } catch (...) {
            result = default_value.value();
        }
    }
    return result;
}

double Config::ReadDoubleSetting(const std::string& key,
                                 const std::optional<double> default_value) {
    std::string full_key = GetFullKey(key, false);
    if (!default_value.has_value()) {
        return config->GetDoubleValue(GetSection().c_str(), full_key.c_str(), 0);
    }

    double result;
    if (config->GetBoolValue(GetSection().c_str(),
                             std::string(full_key).append("\\default").c_str(), true)) {
        result = default_value.value();
    } else {
        result =
            config->GetDoubleValue(GetSection().c_str(), full_key.c_str(), default_value.value());
    }
    return result;
}

std::string Config::ReadStringSetting(const std::string& key,
                                      const std::optional<std::string> default_value) {
    std::string result;
    std::string full_key = GetFullKey(key, false);
    if (!default_value.has_value()) {
        result = config->GetValue(GetSection().c_str(), full_key.c_str(), "");
        boost::replace_all(result, "\"", "");
        return result;
    }

    if (config->GetBoolValue(GetSection().c_str(),
                             std::string(full_key).append("\\default").c_str(), true)) {
        result = default_value.value();
    } else {
        result =
            config->GetValue(GetSection().c_str(), full_key.c_str(), default_value.value().c_str());
    }
    boost::replace_all(result, "\"", "");
    boost::replace_all(result, "//", "/");
    return result;
}

bool Config::Exists(const std::string& section, const std::string& key) const {
    const std::string value = config->GetValue(section.c_str(), key.c_str(), "");
    return !value.empty();
}

void Config::WriteBooleanSetting(const std::string& key, const bool& value,
                                 const std::optional<bool>& default_value,
                                 const std::optional<bool>& use_global) {
    std::optional<std::string> string_default = std::nullopt;
    if (default_value.has_value()) {
        string_default = std::make_optional(ToString(default_value.value()));
    }
    WritePreparedSetting(key, AdjustOutputString(ToString(value)), string_default, use_global);
}

void Config::WriteDoubleSetting(const std::string& key, const double& value,
                                const std::optional<double>& default_value,
                                const std::optional<bool>& use_global) {
    std::optional<std::string> string_default = std::nullopt;
    if (default_value.has_value()) {
        string_default = std::make_optional(ToString(default_value.value()));
    }
    WritePreparedSetting(key, AdjustOutputString(ToString(value)), string_default, use_global);
}

void Config::WriteStringSetting(const std::string& key, const std::string& value,
                                const std::optional<std::string>& default_value,
                                const std::optional<bool>& use_global) {
    std::optional string_default = default_value;
    if (default_value.has_value()) {
        string_default.value().append(AdjustOutputString(default_value.value()));
    }
    WritePreparedSetting(key, AdjustOutputString(value), string_default, use_global);
}

void Config::WritePreparedSetting(const std::string& key, const std::string& adjusted_value,
                                  const std::optional<std::string>& adjusted_default_value,
                                  const std::optional<bool>& use_global) {
    std::string full_key = GetFullKey(key, false);
    if (adjusted_default_value.has_value() && use_global.has_value()) {
        if (!global) {
            WriteString(std::string(full_key).append("\\global"), ToString(use_global.value()));
        }
        if (global || use_global.value() == false) {
            WriteString(std::string(full_key).append("\\default"),
                        ToString(adjusted_default_value == adjusted_value));
            WriteString(full_key, adjusted_value);
        }
    } else if (adjusted_default_value.has_value() && !use_global.has_value()) {
        WriteString(std::string(full_key).append("\\default"),
                    ToString(adjusted_default_value == adjusted_value));
        WriteString(full_key, adjusted_value);
    } else {
        WriteString(full_key, adjusted_value);
    }
}

void Config::WriteString(const std::string& key, const std::string& value) {
    config->SetValue(GetSection().c_str(), key.c_str(), value.c_str());
}

void Config::Reload() {
    ReadValues();
    // To apply default value changes
    SaveValues();
}

void Config::ClearControlPlayerValues() const {
    // Removes the entire [Controls] section
    const char* section = Settings::TranslateCategory(Settings::Category::Controls);
    config->Delete(section, nullptr, true);
}

const std::string& Config::GetConfigFilePath() const {
    return config_loc;
}

void Config::ReadCategory(const Settings::Category category) {
    const auto& settings = FindRelevantList(category);
    std::ranges::for_each(settings, [&](const auto& setting) { ReadSettingGeneric(setting); });
}

void Config::WriteCategory(const Settings::Category category) {
    const auto& settings = FindRelevantList(category);
    std::ranges::for_each(settings, [&](const auto& setting) { WriteSettingGeneric(setting); });
}

void Config::ReadSettingGeneric(Settings::BasicSetting* const setting) {
    if (!setting->Save() || (!setting->Switchable() && !global)) {
        return;
    }

    const std::string key = AdjustKey(setting->GetLabel());
    const std::string default_value(setting->DefaultToString());

    bool use_global = true;
    if (setting->Switchable() && !global) {
        use_global = ReadBooleanSetting(std::string(key).append("\\use_global"), std::make_optional(true));
        setting->SetGlobal(use_global);
    }

    if (global || !use_global) {
        const bool is_default = ReadBooleanSetting(std::string(key).append("\\default"), std::make_optional(true));
        if (!is_default) {
            setting->LoadString(ReadStringSetting(key, default_value));
        } else {
            // Empty string resets the Setting to default
            setting->LoadString("");
        }
    }
}

void Config::WriteSettingGeneric(const Settings::BasicSetting* const setting) {
    if (!setting->Save()) {
        return;
    }

    std::string key = AdjustKey(setting->GetLabel());
    if (setting->Switchable()) {
        if (!global) {
            WriteBooleanSetting(std::string(key).append("\\use_global"), setting->UsingGlobal());
        }
        if (global || !setting->UsingGlobal()) {
            auto value = global ? setting->ToStringGlobal() : setting->ToString();
            WriteBooleanSetting(std::string(key).append("\\default"),
                                value == setting->DefaultToString());
            WriteStringSetting(key, value);
        }
    } else if (global) {
        WriteBooleanSetting(std::string(key).append("\\default"),
                            setting->ToString() == setting->DefaultToString());
        WriteStringSetting(key, setting->ToString());
    }
}

void Config::BeginGroup(const std::string& group) {
    // You can't begin a group while reading/writing from a config array
    ASSERT(array_stack.empty());

    key_stack.push_back(AdjustKey(group));
}

void Config::EndGroup() {
    // You can't end a group if you haven't started one yet
    ASSERT(!key_stack.empty());

    // You can't end a group when reading/writing from a config array
    ASSERT(array_stack.empty());

    key_stack.pop_back();
}

std::string Config::GetSection() {
    if (key_stack.empty()) {
        return std::string{""};
    }

    return key_stack.front();
}

std::string Config::GetGroup() const {
    if (key_stack.size() <= 1) {
        return std::string{""};
    }

    std::string key;
    for (size_t i = 1; i < key_stack.size(); ++i) {
        key.append(key_stack[i]).append("\\");
    }
    return key;
}

std::string Config::AdjustKey(const std::string& key) {
    std::string adjusted_key(key);
    boost::replace_all(adjusted_key, "/", "\\");
    boost::replace_all(adjusted_key, " ", "%20");
    return adjusted_key;
}

std::string Config::AdjustOutputString(const std::string& string) {
    std::string adjusted_string(string);
    boost::replace_all(adjusted_string, "\\", "/");

    // Windows requires that two forward slashes are used at the start of a path for unmapped
    // network drives so we have to watch for that here
#ifndef __ANDROID__
    if (string.substr(0, 2) == "//") {
        boost::replace_all(adjusted_string, "//", "/");
        adjusted_string.insert(0, "/");
    } else {
        boost::replace_all(adjusted_string, "//", "/");
    }
#endif

    // Needed for backwards compatibility with QSettings deserialization
    for (const auto& special_character : special_characters) {
        if (adjusted_string.find(special_character) != std::string::npos) {
            adjusted_string.insert(0, "\"");
            adjusted_string.append("\"");
            break;
        }
    }
    return adjusted_string;
}

std::string Config::GetFullKey(const std::string& key, bool skipArrayIndex) {
    if (array_stack.empty()) {
        return std::string(GetGroup()).append(AdjustKey(key));
    }

    std::string array_key;
    for (size_t i = 0; i < array_stack.size(); ++i) {
        if (!array_stack[i].name.empty()) {
            array_key.append(array_stack[i].name).append("\\");
        }

        if (!skipArrayIndex || (array_stack.size() - 1 != i && array_stack.size() > 1)) {
            array_key.append(ToString(array_stack[i].index)).append("\\");
        }
    }
    std::string final_key = std::string(GetGroup()).append(array_key).append(AdjustKey(key));
    return final_key;
}

int Config::BeginArray(const std::string& array) {
    array_stack.push_back(ConfigArray{AdjustKey(array), 0, 0});
    const int size = config->GetLongValue(GetSection().c_str(), GetFullKey(std::string("size"), true).c_str(), 0);
    array_stack.back().size = (std::max)(0, size);
    return array_stack.back().size;
}

void Config::EndArray() {
    // You can't end a config array before starting one
    ASSERT(!array_stack.empty());

    // Set the array size to 0 if the array is ended without changing the index
    int size = 0;
    if (array_stack.back().index != 0) {
        size = array_stack.back().size;
    }

    // Write out the size to config
    if (key_stack.size() == 1 && array_stack.back().name.empty()) {
        // Edge-case where the first array created doesn't have a name
        config->SetValue(GetSection().c_str(), std::string("size").c_str(), ToString(size).c_str());
    } else {
        auto const key = GetFullKey(std::string("size"), true);
        config->SetValue(GetSection().c_str(), key.c_str(), ToString(size).c_str());
    }

    array_stack.pop_back();
}

void Config::SetArrayIndex(const int index) {
    // You can't set the array index if you haven't started one yet
    ASSERT(!array_stack.empty());

    const int array_index = index + 1;

    // You can't exceed the known max size of the array by more than 1
    ASSERT(array_stack.front().size + 1 >= array_index);

    // Change the config array size to the current index since you may want
    // to reduce the number of elements that you read back from the config
    // in the future.
    array_stack.back().size = array_index;
    array_stack.back().index = array_index;
}
