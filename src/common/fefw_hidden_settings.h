// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <algorithm>
#include <array>
#include <string_view>

namespace Common {

// Fortune's Eden runs one game: settings that don't apply to Fortune's Weave, duplicate another
// one, or only tune the emulator's internals aren't shown in Settings. These are the settings'
// labels (the config file keys), not their C++ names.
inline constexpr std::array<std::string_view, 53> FefwHiddenSettings{
    "cabinet_applet_mode",
    "controller_applet_mode",
    "data_erase_applet_mode",
    "error_applet_mode",
    "net_connect_applet_mode",
    "player_select_applet_mode",
    "swkbd_applet_mode",
    "mii_edit_applet_mode",
    "web_applet_mode",
    "shop_applet_mode",
    "photo_viewer_applet_mode",
    "offline_web_applet_mode",
    "login_share_applet_mode",
    "wifi_web_auth_applet_mode",
    "my_page_applet_mode",
    "scaling_filter",
    "anti_aliasing",
    "frame_pacing_mode",
    "use_disk_shader_cache",
    "use_asynchronous_gpu_emulation",
    "skip_cpu_inner_invalidation",
    "sync_memory_operations",
    "gpu_fence_behavior",
    "dma_accuracy",
    "fast_gpu_time",
    "gpu_unswizzle_enabled",
    "gpu_unswizzle_texture_size",
    "gpu_unswizzle_stream_size",
    "gpu_unswizzle_chunk_size",
    "use_vulkan_driver_pipeline_cache",
    "use_video_framerate",
    "barrier_feedback_loops",
    "enable_buffer_history",
    "fix_bloom_effects",
    "fast_cpu_time",
    "use_custom_cpu_ticks",
    "cpu_ticks",
    "memory_layout_mode",
    "use_multi_core",
    "sync_core_speed",
    "enable_overlay",
    "current_speed_mode",
    "turbo_speed_limit",
    "slow_speed_limit",
    "time_zone_index",
    "program_args",
    "nvdec_emulation",
    "accelerate_astc",
    "gpu_accuracy",
    "astc_recompression",
    "rng_seed",
    "rng_seed_enabled",
    "dump_audio_commands",
};

// Shown nowhere either, but their widgets are still built (other settings' code uses them).
inline constexpr std::array<std::string_view, 9> FefwHiddenBuiltSettings{
    "backend", "aspect_ratio", "bg_red", "bg_green", "bg_blue", "region_index",
    "custom_rtc_enabled", "custom_rtc", "custom_rtc_offset",
};

/// Not shown in Settings, and its widget isn't built.
inline bool IsFefwHiddenSetting(std::string_view label) {
    return std::ranges::find(FefwHiddenSettings, label) != FefwHiddenSettings.end();
}

/// Not shown in Settings: reset to its default whenever the config is read, so a value set in
/// an older version (OpenGL, multi-core off, FXAA...) can't stay stuck with no way to change it.
inline bool IsFefwResetSetting(std::string_view label) {
    return IsFefwHiddenSetting(label) ||
           std::ranges::find(FefwHiddenBuiltSettings, label) != FefwHiddenBuiltSettings.end();
}

} // namespace Common
