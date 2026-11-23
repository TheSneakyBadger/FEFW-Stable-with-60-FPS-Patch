// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

// NVIDIA DLSS through NVIDIA Streamline. Only Streamline's MIT-licensed
// headers are built in: NVIDIA's DLLs are loaded at run time from the user's PC, and none ship
// with the emulator.

#pragma once

#include <array>
#include <filesystem>
#include <string>
#include <vector>

#include "common/common_types.h"
#include "video_core/vulkan_common/vulkan_wrapper.h"

namespace Vulkan::Streamline {

/// NVIDIA's files DLSS needs, as found on this PC. Paths are empty when missing.
struct DlssFiles {
    std::filesystem::path folder;     ///< Where the loader is, or where it should go
    std::filesystem::path interposer; ///< sl.interposer.dll (Streamline's loader)
    std::filesystem::path model;      ///< nvngx_dlss.dll next to the loader
    std::filesystem::path app_model;  ///< Newest DLSS the NVIDIA app downloaded
    std::string interposer_version;
    std::string model_version;
    std::string app_model_version;
};

[[nodiscard]] DlssFiles FindDlssFiles();

/// The files DLSS frame generation needs (besides the NVIDIA driver) that aren't in the
/// Streamline folder (FindDlssFiles().folder): sl.interposer.dll, sl.common.dll, sl.dlss_g.dll,
/// sl.reflex.dll, sl.pcl.dll and nvngx_dlssg.dll.
[[nodiscard]] std::vector<std::string> MissingFrameGenFiles();

/// The user folder's "streamline" folder.
[[nodiscard]] std::filesystem::path UserFolder();

/// The community MFG Unlocker (dashdogy, MIT licence), which lifts NVIDIA's 2x limit on DLSS
/// frame generation on GeForce RTX 40 and older cards: where Fortune's Eden keeps it (the
/// Streamline folder's "mfg-unlocker" folder). Downloaded from Settings, never shipped.
[[nodiscard]] std::filesystem::path MfgUnlockerPath();

/// Its SHA-256 (the release Fortune's Eden downloads); a file that doesn't match isn't loaded.
inline constexpr const char* MfgUnlockerSha256 =
    "5a389e6cad23b2b607e1dc224e9e86ee32d6ef46f0557b6c6f8137feadf0aa74";

/// Streamline's own modules carry a second, Streamline-only signature.
[[nodiscard]] bool IsStreamlineModule(const std::filesystem::path& path);

/// A valid Authenticode signature from NVIDIA Corporation (DLSS models).
[[nodiscard]] bool IsSignedByNvidia(const std::filesystem::path& path);

/// When the NVIDIA DLSS upscaler or DLSS frame generation is on, loads the user's sl.interposer.dll (checking NVIDIA's
/// signature) and starts Streamline for DLSS. Call before creating the renderer's instance.
bool Start();

/// False once NVIDIA DLSS frame generation has run and stopped in this process: NVIDIA's
/// Streamline throws an exception setting up the next game's device after that, so the next game
/// needs a new process to use NVIDIA DLSS.
[[nodiscard]] bool CanStartAgain();

/// Streamline's proxy for a Vulkan function it hooks (vkCreateWin32SurfaceKHR, ...) while it is
/// running, else null.
[[nodiscard]] void* HookedFunction(const char* name);

/// Whether Streamline is running and waiting for the renderer's device.
[[nodiscard]] bool WantsDevice();

/// Instance and device extensions DLSS asks for (empty when Streamline isn't running).
[[nodiscard]] std::vector<std::string> InstanceExtensions();
[[nodiscard]] std::vector<std::string> DeviceExtensions();

/// Hands the renderer's device to Streamline and routes the swapchain calls through it, as
/// Streamline's manual hooking needs. Stops Streamline and returns false when DLSS can't run.
bool AttachDevice(VkInstance instance, VkPhysicalDevice physical, VkDevice device,
                  u32 graphics_family, vk::DeviceDispatch& dld);

/// Whether DLSS super resolution can run on the attached device.
[[nodiscard]] bool IsDlssAvailable();

/// Whether DLSS frame generation (with Reflex and PC latency markers) can run on it.
[[nodiscard]] bool IsFrameGenerationAvailable();

/// Queues Streamline's features want in the graphics family, after the emulator's own.
struct ExtraQueues {
    u32 graphics{};
    u32 compute{};
};
[[nodiscard]] ExtraQueues GetExtraQueues();

/// How many DLSS evaluations Streamline has reported as failed in its log. Streamline still
/// returns success from the evaluation when NGX fails inside it (seen on an RTX 5060 Laptop GPU:
/// "NGX evaluate feature failed 0xbad00005" on every frame), leaving the output unwritten.
[[nodiscard]] u64 EvaluateFailures();

/// Shuts Streamline down; call before the device is destroyed.
void Stop();

// DLSS frame generation. NVIDIA's frame generation runs in Streamline's vkQueuePresentKHR: each
// presented frame needs its inputs tagged under its frame index, which the present then names
// with its PC latency markers.

struct FrameGenImage {
    VkImage image{};
    VkImageView view{};
    VkFormat format{VK_FORMAT_UNDEFINED};
    VkExtent2D size{};
};

struct FrameGenInputs {
    u64 index{};                  ///< The presented frame's index (one per present, increasing).
    FrameGenImage depth;          ///< Reversed device depth at the render size.
    FrameGenImage motion_vectors; ///< Motion to the previous frame in UV units, render size.
    FrameGenImage hudless;        ///< The frame without its UI (backbuffer size; GENERAL).
    FrameGenImage ui_alpha;       ///< The UI's coverage, 0 where none (backbuffer size; GENERAL).
    VkRect2D backbuffer_rect{};   ///< Where the game's picture is in the backbuffer.
    std::array<f32, 2> jitter{};  ///< In render pixels.
    bool reset{};
};

/// On the GPU thread, at the start of a frame's work: NVIDIA Reflex's sleep (its frame limit,
/// which paces DLSS frame generation) and the simulation start marker. Not on the Vulkan worker: a
/// sleep there holds up everything queued for the card (#63: the card's work backed up while
/// the card was 70-80% busy, the worker asleep in Reflex 40-50% of the time).
/// EDEN_DLSSG_REFLEX_SLEEP=0: no sleep (testing).
void FrameGenSleep(u64 index);

/// Time the presenting thread waited for a free frame (frame generation's queue full): the
/// Reflex hold guard puts Reflex's sleep back when holding it off makes these waits grow.
void NoteFreeFrameWait(s64 ns);

/// On the GPU thread, just before the frame's tags: the end of its simulation marker (its start is
/// FrameGenSleep's).
void FrameGenSimulationEnd(u64 index);

/// On the Vulkan worker, recording the frame: render markers, constants and tags for it.
void FrameGenTagFrame(VkCommandBuffer cmdbuf, const FrameGenInputs& inputs);

/// On the present thread, around the present of a frame: tagged (FrameGenTagFrame ran for this
/// index, and frame generation should run) or not (frame generation pauses).
void FrameGenPresentStart(u64 index, bool tagged);
void FrameGenPresentEnd(u64 index, bool tagged);

/// On the present thread, before the swapchain is made again (window resized, fullscreen toggled,
/// out of date): turns frame generation off and frees its resources, so its own present thread
/// stops using the old swapchain. The next tagged present turns it on again.
void FrameGenBeforeSwapchainRecreate();

} // namespace Vulkan::Streamline
