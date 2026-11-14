// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

// SPDX-FileCopyrightText: Copyright 2018 yuzu Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <array>
#include <chrono>
#include <memory>
#include <optional>
#include <string>
#include <variant>

#include "common/dynamic_library.h"
#include "video_core/host1x/gpu_device_memory_manager.h"
#include "video_core/renderer_base.h"
#ifdef HAS_LSFG
#include "video_core/renderer_vulkan/present/frame_gen.h"
#endif
#include "video_core/renderer_vulkan/vk_blit_screen.h"
#include "video_core/renderer_vulkan/vk_present_manager.h"
#include "video_core/renderer_vulkan/vk_rasterizer.h"
#include "video_core/renderer_vulkan/vk_scheduler.h"
#include "video_core/renderer_vulkan/vk_state_tracker.h"
#include "video_core/renderer_vulkan/vk_swapchain.h"
#include "video_core/renderer_vulkan/vk_turbo_mode.h"
#include "video_core/vulkan_common/vulkan_device.h"
#include "video_core/vulkan_common/vulkan_memory_allocator.h"
#include "video_core/vulkan_common/vulkan_wrapper.h"

namespace Core::Memory {
class Memory;
}

namespace Tegra {
class GPU;
}

namespace Vulkan {

Device CreateDevice(const vk::Instance& instance, const vk::InstanceDispatch& dld,
                    VkSurfaceKHR surface);

class RendererVulkan final : public VideoCore::RendererBase {
public:
    explicit RendererVulkan(Core::Frontend::EmuWindow& emu_window,
                            Tegra::MaxwellDeviceMemoryManager& device_memory_, Tegra::GPU& gpu_,
                            std::unique_ptr<Core::Frontend::GraphicsContext> context_);
    ~RendererVulkan() override;

    void Composite(std::span<const Tegra::FramebufferConfig> framebuffers) override;

    std::vector<u8> GetAppletCaptureBuffer() override;

    VideoCore::RasterizerInterface* ReadRasterizer() override {
        return &rasterizer;
    }

    [[nodiscard]] std::string GetDeviceVendor() const override {
        return device.GetDriverName();
    }

    // Enhanced platform-specific initialization
    void InitializePlatformSpecific();

private:
    void InterpolateFrames(Frame* prev_frame, Frame* curr_frame);
    Frame* previous_frame = nullptr;  // Store the previous frame for interpolation
    VkCommandBuffer BeginSingleTimeCommands();
    void EndSingleTimeCommands(VkCommandBuffer command_buffer);
    void Report() const;

    vk::Buffer RenderToBuffer(std::span<const Tegra::FramebufferConfig> framebuffers,
                              const Layout::FramebufferLayout& layout, VkFormat format,
                              VkDeviceSize buffer_size);
    void RenderScreenshot(std::span<const Tegra::FramebufferConfig> framebuffers);
    void RenderAppletCaptureLayer(std::span<const Tegra::FramebufferConfig> framebuffers);
    /// AMD FSR 3.1 frame generation: presents a frame between the last one and `frame` (when on
    /// and this frame has the game's motion), and paces the two.
    /// The surface the swapchain is made on: with DLSS frame generation, the window's surface
    /// made again through Streamline now that it has the device.
    VkSurfaceKHR SwapchainSurface();

    /// Whether frame generation is held off for this game frame (the card is behind).
    bool FrameGenHeldOff(std::chrono::steady_clock::time_point now);
    void GenerateFsrFrame(Frame* frame, DAddr presented, u32 presented_width,
                          u32 presented_height);

    std::vector<Tegra::FramebufferConfig> applet_capture_layers;
    std::vector<Tegra::FramebufferConfig> screenshot_layer_scratch;

    Tegra::MaxwellDeviceMemoryManager& device_memory;
    Tegra::GPU& gpu;

    std::shared_ptr<Common::DynamicLibrary> library;
    vk::InstanceDispatch dld;
#ifdef EGLANTINE_STREAMLINE
    bool streamline_started; ///< NVIDIA Streamline starts before the instance.
#endif

    // Keep original handles for compatibility with existing code
    vk::Instance instance;

    vk::DebugUtilsMessenger debug_messenger;

    vk::SurfaceKHR surface;

    Device device;
    MemoryAllocator memory_allocator;
    StateTracker state_tracker;
    Scheduler scheduler;
    Swapchain swapchain;
    PresentManager present_manager;
    BlitScreen blit_swapchain;
    BlitScreen blit_capture;
    BlitScreen blit_applet;
    RasterizerVulkan rasterizer;
#ifdef HAS_LSFG
    FrameGen frame_gen;
#endif
    std::optional<TurboMode> turbo_mode;
    std::optional<std::chrono::steady_clock::time_point> fsr_fg_last_composite;
    f32 fsr_fg_interval_ms{};
    u64 fsr_fg_generated{};
    std::optional<std::chrono::steady_clock::time_point> fsr_fg_report_start;
    u32 fsr_fg_made{};
    u32 fsr_fg_skipped{};
    u32 fsr_fg_ui{};
    u32 fsr_fg_ui_layers{};
    u32 fsr_fg_late{};    ///< Presented after the game had started its next frame.
    u32 fsr_fg_unknown{}; ///< The presented buffer wasn't seen being drawn.
    u32 fsr_fg_fallback{}; ///< Its frame wasn't prepared: the newest prepared one was used.
    u64 dlssg_index{};     ///< NVIDIA DLSS frame generation: the last frame index presented.
    f32 fsr_fg_wait_ms{};
    f32 fsr_fg_minute_wait_ms{};
    /// Frame generation held off while the worker is behind with the card's work: with many lit
    /// effects (a battle with torches and magic floor tiles) a stall left a backlog it couldn't
    /// clear with frame generation's work added, and frames reached the screen late.
    struct FrameGenBackoff {
        std::array<u8, 8> backlog{}; ///< The last 8 game frames: 1 where the backlog was large.
        u32 backlog_at{};
        u32 backlog_count{};
        bool held{};
        u32 clear_frames{};       ///< Game frames in a row with the backlog cleared.
        u32 clear_needed{15};     ///< ... before frame generation comes back on.
        u32 held_frames{};        ///< In this hold.
        size_t held_peak{};       ///< The largest backlog in this hold.
        std::optional<std::chrono::steady_clock::time_point> resumed_at;
        u32 holds{};              ///< For the 10 s report.
        u32 skipped{};
        u32 logged{};
    } fg_backoff;

    Frame applet_frame;
};

} // namespace Vulkan
