// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

// SPDX-FileCopyrightText: Copyright 2018 yuzu Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <algorithm>
#include <array>
#include <cstdlib>
#include <cstring>
#include <utility>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include <fmt/ranges.h>

#include "common/logging.h"
#include <ranges>
#include "common/scope_exit.h"
#include "common/settings.h"
#include "core/core_timing.h"
#include "core/frontend/graphics_context.h"
#include "video_core/back_thread_stats.h"
#include "video_core/capture.h"
#include "video_core/gpu.h"
#include "video_core/present.h"
#include "video_core/renderer_vulkan/present/util.h"
#ifdef HAS_RESHADE
#include "video_core/post_processing/fx_chain.h"
#endif
#include "video_core/renderer_vulkan/renderer_vulkan.h"
#include "video_core/renderer_vulkan/vk_blit_screen.h"
#include "video_core/renderer_vulkan/vk_rasterizer.h"
#include "video_core/renderer_vulkan/vk_scheduler.h"
#include "video_core/renderer_vulkan/vk_state_tracker.h"
#include "video_core/renderer_vulkan/vk_swapchain.h"
#include "video_core/textures/decoders.h"
#include "video_core/vulkan_common/vulkan_debug_callback.h"
#include "video_core/vulkan_common/vulkan_device.h"
#include "video_core/vulkan_common/vulkan_instance.h"
#include "video_core/vulkan_common/vulkan_library.h"
#include "video_core/vulkan_common/vulkan_memory_allocator.h"
#include "video_core/vulkan_common/vulkan_surface.h"
#include "video_core/vulkan_common/vulkan_wrapper.h"
#ifdef EGLANTINE_STREAMLINE
#include "video_core/vulkan_common/streamline.h"
#endif
#ifdef __ANDROID__
#include <jni.h>
#endif
namespace Vulkan {
namespace {

constexpr VkExtent2D CaptureImageSize{
    .width = VideoCore::Capture::LinearWidth,
    .height = VideoCore::Capture::LinearHeight,
};

#ifdef HAS_LSFG
[[nodiscard]] VkExtent2D GuestExtent(std::span<const Tegra::FramebufferConfig> framebuffers) {
    if (framebuffers.empty()) {
        return VkExtent2D{};
    }

    const auto& framebuffer = framebuffers.front();
    if (framebuffer.crop_rect.IsEmpty()) {
        return VkExtent2D{.width = framebuffer.width, .height = framebuffer.height};
    }
    return VkExtent2D{
        .width = static_cast<u32>(framebuffer.crop_rect.GetWidth()),
        .height = static_cast<u32>(framebuffer.crop_rect.GetHeight()),
    };
}
#endif

constexpr VkExtent3D CaptureImageExtent{
    .width = VideoCore::Capture::LinearWidth,
    .height = VideoCore::Capture::LinearHeight,
    .depth = VideoCore::Capture::LinearDepth,
};

constexpr VkFormat CaptureFormat = VK_FORMAT_A8B8G8R8_UNORM_PACK32;

std::string GetReadableVersion(u32 version) {
    return fmt::format("{}.{}.{}", VK_VERSION_MAJOR(version), VK_VERSION_MINOR(version),
                       VK_VERSION_PATCH(version));
}

std::string GetDriverVersion(const Device& device) {
    // Extracted from
    // https://github.com/SaschaWillems/vulkan.gpuinfo.org/blob/5dddea46ea1120b0df14eef8f15ff8e318e35462/functions.php#L308-L314
    const u32 version = device.GetDriverVersion();

    if (device.GetDriverID() == VK_DRIVER_ID_NVIDIA_PROPRIETARY) {
        const u32 major = (version >> 22) & 0x3ff;
        const u32 minor = (version >> 14) & 0x0ff;
        const u32 secondary = (version >> 6) & 0x0ff;
        const u32 tertiary = version & 0x003f;
        return fmt::format("{}.{}.{}.{}", major, minor, secondary, tertiary);
    }
    if (device.GetDriverID() == VK_DRIVER_ID_INTEL_PROPRIETARY_WINDOWS) {
        const u32 major = version >> 14;
        const u32 minor = version & 0x3fff;
        return fmt::format("{}.{}", major, minor);
    }
    return GetReadableVersion(version);
}

std::string BuildCommaSeparatedExtensions(
    const std::set<std::string, std::less<>>& available_extensions) {
    return fmt::format("{}", fmt::join(available_extensions, ","));
}

} // Anonymous namespace

Device CreateDevice(const vk::Instance& instance, const vk::InstanceDispatch& dld, VkSurfaceKHR surface) {
    const std::vector<VkPhysicalDevice> devices = instance.EnumeratePhysicalDevices();
    const u32 device_index = Settings::values.vulkan_device.GetValue();
    if (device_index >= u32(devices.size())) {
        LOG_ERROR(Render_Vulkan, "Invalid device index {}!", device_index);
        throw vk::Exception(VK_ERROR_INITIALIZATION_FAILED);
    }
    const vk::PhysicalDevice physical_device(devices[device_index], dld);
    return Device(*instance, physical_device, surface, dld);
}

RendererVulkan::RendererVulkan(Core::Frontend::EmuWindow& emu_window,
                               Tegra::MaxwellDeviceMemoryManager& device_memory_,
                               Tegra::GPU& gpu_,
                               std::unique_ptr<Core::Frontend::GraphicsContext> context_)
try
    : RendererBase(emu_window, std::move(context_))
    , device_memory(device_memory_)
    , gpu(gpu_)
    , library(OpenLibrary(context.get()))
    , dld()
#ifdef EGLANTINE_STREAMLINE
    , streamline_started(Streamline::Start())
#endif
    // Create raw Vulkan instance first
    , instance(CreateInstance(*library,
                            dld,
                            VK_API_VERSION_1_1,
                            render_window.GetWindowInfo().type,
                            Settings::values.renderer_debug.GetValue()))
    // Create debug messenger if debug is enabled
    , debug_messenger(Settings::values.renderer_debug ? CreateDebugUtilsCallback(instance)
                                                    : vk::DebugUtilsMessenger{})
    // Create surface
    , surface(CreateSurface(instance, render_window.GetWindowInfo()))
    , device(CreateDevice(instance, dld, *surface))
    , memory_allocator(device)
    , state_tracker()
    , scheduler(device, state_tracker)
    , swapchain(SwapchainSurface(),
                device,
                scheduler,
               render_window.GetFramebufferLayout().width,
               render_window.GetFramebufferLayout().height)
    , present_manager(instance,
                      render_window,
                      device,
                      memory_allocator,
                      scheduler,
                      swapchain,
                      surface)
    , blit_swapchain(device_memory,
                   device,
                   memory_allocator,
                   present_manager,
                   scheduler,
                   PresentFiltersForDisplay)
    , blit_capture(device_memory,
                   device,
                   memory_allocator,
                   present_manager,
                   scheduler,
                   PresentFiltersForDisplay)
    , blit_applet(device_memory,
                  device,
                  memory_allocator,
                  present_manager,
                  scheduler,
                  PresentFiltersForAppletCapture)
    , rasterizer(render_window, gpu, device_memory, device, memory_allocator, state_tracker, scheduler)
#ifdef HAS_LSFG
    , frame_gen(memory_allocator, scheduler)
#endif
{

    if (Settings::values.renderer_force_max_clock.GetValue() && device.ShouldBoostClocks()) {
        turbo_mode.emplace(instance, dld);
        scheduler.RegisterOnSubmit([this] { turbo_mode->QueueSubmitted(); });
    }

#ifdef HAS_RESHADE
    VideoCore::FxChain::Instance().LoadFromSettings();
#endif

    Report();
} catch (const vk::Exception& exception) {
    LOG_ERROR(Render_Vulkan, "Vulkan initialization failed with error: {}", exception.what());
    throw std::runtime_error{fmt::format("Vulkan initialization error {}", exception.what())};
}

RendererVulkan::~RendererVulkan() {
    scheduler.RegisterOnSubmit([] {});
    void(device.GetLogical().WaitIdle());
}

void RendererVulkan::Composite(std::span<const Tegra::FramebufferConfig> framebuffers) {
    const VideoCommon::BackStats::ScopedSharedTime composite_time{VideoCommon::BackStats::shared.composite};
    SCOPE_EXIT {
        render_window.OnFrameDisplayed();
    };

    // Swapchain::Create leaves the format undefined while the surface has a zero extent (for
    // example a minimized window). Creating the present frame image with VK_FORMAT_UNDEFINED
    // makes the NVIDIA driver divide by zero, and there is no swapchain to present to anyway.
    // This also covers the applet capture frame, which is allocated through the same present
    // manager.
    if (swapchain.GetImageFormat() == VK_FORMAT_UNDEFINED) {
        static bool logged = false;
        if (!std::exchange(logged, true)) {
            LOG_WARNING(Render_Vulkan, "Skipping composite while the swapchain has no format");
        }
        return;
    }

    RenderAppletCaptureLayer(framebuffers);

    if (!render_window.IsShown()) {
        return;
    }

    RenderScreenshot(framebuffers);
    // Testing (EDEN_FG_TRACE=1): which image is presented, around the traced frame.
    {
        static const bool trace = std::getenv("EDEN_FG_TRACE") != nullptr;
        static u32 composites = 0;
        if (trace && ++composites >= 1795 && composites <= 1815 && !framebuffers.empty()) {
            LOG_INFO(Render_Vulkan, "Draw trace: presenting {:#x} {}x{}", framebuffers[0].address,
                     framebuffers[0].width, framebuffers[0].height);
        }
    }
    const auto frame_wait_start = std::chrono::steady_clock::now();
    Frame* frame = present_manager.GetRenderFrame();
    // Frame generation's pacing doesn't count time spent here waiting for a free frame.
    fsr_fg_wait_ms += std::chrono::duration<f32, std::milli>(std::chrono::steady_clock::now() -
                                                             frame_wait_start)
                          .count();

    scheduler.RequestOutsideRenderPassOperationContext();
    blit_swapchain.DrawToFrame(device, rasterizer, frame, framebuffers,
                               render_window.GetFramebufferLayout(),
                               present_manager.SwapchainImageCount(),
                               swapchain.GetImageViewFormat());

#ifdef HAS_LSFG
    void(frame_gen.WantedGenerations(present_manager.MaxExtraFrames()));

    frame_gen.Process(device, frame, present_manager.SwapchainImageFormat(),
                      GuestExtent(framebuffers));

    const size_t generated_frames = frame_gen.GeneratedFrameCount();
    for (size_t generation = 0; generation < generated_frames; ++generation) {
        Frame* generated = present_manager.GetRenderFrame();
        blit_swapchain.PrepareFrame(device, generated, render_window.GetFramebufferLayout());
        frame_gen.GenerateInto(device, generated, generation);
        scheduler.Flush(*generated->render_ready);
        present_manager.Present(generated);
    }
#endif

    if (framebuffers.empty()) {
        GenerateFsrFrame(frame, DAddr{}, 0, 0);
    } else {
        GenerateFsrFrame(frame, framebuffers[0].address + framebuffers[0].offset,
                         framebuffers[0].width, framebuffers[0].height);
    }

    scheduler.Flush(*frame->render_ready);

    present_manager.Present(frame);
    // With asynchronous presentation the hand-over to the present thread is a recorded command:
    // send it to the worker now, not with whatever is recorded next. (Was only done with frame
    // generation built in; found missing on Linux by a player's investigation.)
    scheduler.DispatchWork();

    gpu.RendererFrameEndNotify();
    rasterizer.TickFrame();
}

VkSurfaceKHR RendererVulkan::SwapchainSurface() {
#ifdef EGLANTINE_STREAMLINE
    if (Streamline::IsFrameGenerationAvailable()) {
        surface = vk::SurfaceKHR{};
        surface = CreateSurface(instance, render_window.GetWindowInfo(), true);
    }
#endif
    return *surface;
}

bool RendererVulkan::FrameGenHeldOff(std::chrono::steady_clock::time_point now) {
    // EDEN_FG_BACKOFF=0: frame generation on whatever the backlog (as before).
    static const bool disabled = [] {
        const char* const value = std::getenv("EDEN_FG_BACKOFF");
        return value && value[0] == '0';
    }();
    if (disabled) {
        return false;
    }
    // Normally under 1 chunk on average, single spikes to ~16; a backlog grows to 50-300.
    constexpr size_t LARGE = 24;
    constexpr size_t CLEARED = 4;
    FrameGenBackoff& b = fg_backoff;
    const size_t queued = scheduler.QueuedChunks();
    if (b.held) {
        ++b.skipped;
        ++b.held_frames;
        b.held_peak = (std::max)(b.held_peak, queued);
        b.clear_frames = queued <= CLEARED ? b.clear_frames + 1 : 0;
        if (b.clear_frames < b.clear_needed) {
            return true;
        }
        if (b.logged < 30) {
            ++b.logged;
            LOG_INFO(Render_Vulkan,
                     "Frame generation back on: the backlog cleared after {} game frames (up to "
                     "{} chunks)",
                     b.held_frames, b.held_peak);
        }
        b.held = false;
        b.resumed_at = now;
        b.backlog.fill(0);
        b.backlog_count = 0;
        return false;
    }
    const u8 large = queued >= LARGE ? 1 : 0;
    b.backlog_count = b.backlog_count + large - b.backlog[b.backlog_at];
    b.backlog[b.backlog_at] = large;
    b.backlog_at = (b.backlog_at + 1) % static_cast<u32>(b.backlog.size());
    if (b.backlog_count < 4) {
        // A while without a backlog: the next hold ends as soon as it has cleared.
        if (b.resumed_at && now - *b.resumed_at > std::chrono::seconds{30}) {
            b.clear_needed = 15;
            b.resumed_at.reset();
        }
        return false;
    }
    // Back again soon after: wait longer before coming back on (up to 4 s clear).
    if (b.resumed_at && now - *b.resumed_at < std::chrono::seconds{3}) {
        b.clear_needed = (std::min)(b.clear_needed * 2, 240U);
    }
    b.held = true;
    b.clear_frames = 0;
    b.held_frames = 0;
    b.held_peak = queued;
    ++b.holds;
    ++b.skipped;
    if (b.logged < 30) {
        ++b.logged;
        LOG_INFO(Render_Vulkan,
                 "Frame generation held off: {} chunks of the card's work waiting ({} of the last "
                 "{} game frames over {}); back on after {} game frames under {}",
                 queued, b.backlog_count, b.backlog.size(), LARGE, b.clear_needed, CLEARED + 1);
    }
    return true;
}

void RendererVulkan::GenerateFsrFrame(Frame* frame, DAddr presented, u32 presented_width,
                                      u32 presented_height) {
    frame->present_delay = {};
    frame->sl_frame_gen = false;
    frame->sl_tagged = false;
    Fsr3FrameGenerator* const generator = rasterizer.FrameGenerator();
    if (!generator) {
        fsr_fg_last_composite.reset();
        return;
    }
    // The game's frame interval (smoothed): the generated frame goes halfway through it.
    const auto now = std::chrono::steady_clock::now();
    if (fsr_fg_last_composite) {
        // The game's own interval: without the time this thread waited for free frames, which
        // the pacing itself causes (counted, it fed back: longer waits, slower game, longer
        // waits; battles settled at 35-45 FPS).
        const f32 interval =
            (std::max)(std::chrono::duration<f32, std::milli>(now - *fsr_fg_last_composite)
                               .count() -
                           fsr_fg_wait_ms,
                       1.0f);
        fsr_fg_minute_wait_ms += fsr_fg_wait_ms;
        fsr_fg_interval_ms = fsr_fg_interval_ms == 0.0f || interval > 4.0f * fsr_fg_interval_ms
                                 ? interval
                                 : fsr_fg_interval_ms * 0.9f + interval * 0.1f;
    }
    fsr_fg_last_composite = now;
    fsr_fg_wait_ms = 0.0f;
    // About every 10 s: how many frames were made, and the game's frame interval.
    if (!fsr_fg_report_start) {
        fsr_fg_report_start = now;
    } else if (now - *fsr_fg_report_start >= std::chrono::seconds{10}) {
        LOG_INFO(Render_Vulkan,
                 "Frame generation, last 10 s: {} frames made ({} with the UI layer kept still), "
                 "{} game frames without one; game frames {:.1f} ms apart; {:.0f} ms waiting "
                 "for free frames; {:.2f} UI layers a frame; {} frames presented after the game "
                 "started the next, {} not matched to a game frame, {} given the newest motion; UI "
                 "copies made again {}; held off for {} game frames ({} times) with a backlog",

                 fsr_fg_made, fsr_fg_ui, fsr_fg_skipped, fsr_fg_interval_ms,
                 fsr_fg_minute_wait_ms,
                 fsr_fg_made ? static_cast<f32>(fsr_fg_ui_layers) / static_cast<f32>(fsr_fg_made)
                             : 0.0f,
                 fsr_fg_late, fsr_fg_unknown, fsr_fg_fallback,
                 generator->TakeUiCopiesRecreated(), fg_backoff.skipped, fg_backoff.holds);
        fg_backoff.skipped = 0;
        fg_backoff.holds = 0;
        fsr_fg_fallback = 0;
        fsr_fg_ui_layers = 0;
        fsr_fg_late = 0;
        fsr_fg_unknown = 0;
        fsr_fg_minute_wait_ms = 0.0f;
        fsr_fg_ui = 0;
        fsr_fg_report_start = now;
        fsr_fg_made = 0;
        fsr_fg_skipped = 0;
    }
    // Only with the game's motion for this frame (3D scenes), storage-capable frames, and a frame
    // rate where it helps: not above ~120 FPS (already smooth) or below ~15 (too far apart).
    // Which of the game's frames this is: the game may present a frame after it has started
    // the next (its motion and UI then come from what was kept for it).
    RasterizerVulkan::FrameGenFrameInfo game_frame = rasterizer.FrameGenFrame(presented, presented_width, presented_height);
    if (!generator->HasPrepared(game_frame.seq)) {
        // Its frame wasn't prepared (or was used already): the newest prepared frame, as before
        // frames were matched, rather than none.
        const u64 newest = generator->NewestPrepared();
        static const bool trace = std::getenv("EDEN_FG_TRACE") != nullptr;
        static u32 traced = 0;
        if (trace && fsr_fg_generated > 1500 && traced++ < 40) {
            LOG_INFO(Render_Vulkan, "FG order: frame {} not prepared, newest {}",
                     game_frame.seq, newest);
        }
        if (newest != 0) {
            ++fsr_fg_fallback;
            game_frame.seq = newest;
            game_frame.previous = false;
            game_frame.layers = rasterizer.FrameGenFrame(0, 0, 0).layers;
        }
    }
    const bool prepared = generator->HasPrepared(game_frame.seq);
    // Frames the game had motion for count towards whether the card is behind.
    const bool held = prepared && FrameGenHeldOff(now);
    if (prepared) {
        fsr_fg_late += game_frame.previous ? 1 : 0;
        fsr_fg_unknown += game_frame.known ? 0 : 1;
    }
#ifdef EGLANTINE_STREAMLINE
    // NVIDIA DLSS frame generation: its present makes and paces the frame; this frame's inputs
    // are tagged for it (the frame without its UI, the UI's coverage, depth and motion).
    if (Settings::values.frame_generation.GetValue() == Settings::FrameGeneration::NvidiaDlss &&
        Streamline::IsFrameGenerationAvailable()) {
        frame->sl_frame_gen = true;
        frame->sl_index = ++dlssg_index;
        if (!prepared || !frame->storage_view || held) {
            ++fsr_fg_skipped;
            generator->Skip(game_frame.seq);
            return;
        }
        const Layout::FramebufferLayout layout = render_window.GetFramebufferLayout();
        const s32 left = static_cast<s32>((std::min)(layout.screen.left, frame->width));
        const s32 top = static_cast<s32>((std::min)(layout.screen.top, frame->height));
        const u32 width =
            (std::min)(layout.screen.GetWidth(), frame->width - static_cast<u32>(left));
        const u32 height =
            (std::min)(layout.screen.GetHeight(), frame->height - static_cast<u32>(top));
        const VkFormat format = present_manager.SwapchainImageFormat();
        const u32 transfer_function = format == VK_FORMAT_A2B10G10R10_UNORM_PACK32   ? 1U
                                      : format == VK_FORMAT_R16G16B16A16_SFLOAT ? 2U
                                                                                : 0U;
        const bool late = game_frame.previous;
        generator->Generate({
            .current_image = *frame->image,
            .current_view = *frame->storage_view,
            .output_image = VK_NULL_HANDLE,
            .output_view = VK_NULL_HANDLE,
            .format = format,
            .display_size = {frame->width, frame->height},
            .interpolation_rect = {.offset = {left, top}, .extent = {width, height}},
            .frame_time_ms = fsr_fg_interval_ms,
            .reset = false,
            .transfer_function = transfer_function,
            .min_max_luminance = {0.0f, 1000.0f},
            .ui_layers = game_frame.layers,
            .show_ui_mask = false,
            .seq = game_frame.seq,
            .late = late,
            .show_ui_less = false,
            .dlss = true,
        });
        const Fsr3FrameGenerator::DlssImages& images = generator->LastDlssImages();
        const auto image = [](const Fsr3FrameGenerator::Image& in) {
            return Streamline::FrameGenImage{in.image, in.view, in.format, in.size};
        };
        const Streamline::FrameGenInputs inputs{
            .index = frame->sl_index,
            .depth = image(images.depth),
            .motion_vectors = image(images.motion_vectors),
            .hudless = image(images.hudless),
            .ui_alpha = image(images.ui_alpha),
            .backbuffer_rect = {.offset = {left, top}, .extent = {width, height}},
            .jitter = images.jitter,
            .reset = images.reset,
        };
        scheduler.Record([inputs](vk::CommandBuffer cmdbuf) {
            Streamline::FrameGenTagFrame(*cmdbuf, inputs);
        });
        // Streamline records its own copies and state on the command buffer.
        scheduler.InvalidateState();
        frame->sl_tagged = true;
        Settings::SetActiveFrameGeneration("NVIDIA DLSS");
        ++fsr_fg_made;
        if (++fsr_fg_generated == 1) {
            LOG_INFO(Render_Vulkan, "NVIDIA DLSS frame generation: first frame tagged");
        }
        return;
    }
#endif
    const bool wanted = prepared && !held && frame->storage_view &&
                        fsr_fg_interval_ms > 7.5f && fsr_fg_interval_ms < 70.0f;
    if (!wanted) {
        // Once a session: why no frame was made while the game had motion.
        static bool logged{};
        if (prepared && !held && !logged) {
            logged = true;
            LOG_INFO(Render_Vulkan,
                     "AMD FSR 3.1 frame generation skipped: frame storage {}, {:.1f} ms apart",
                     static_cast<bool>(frame->storage_view), fsr_fg_interval_ms);
        }
        generator->Skip(game_frame.seq);
        ++fsr_fg_skipped;
        return;
    }
    const Layout::FramebufferLayout layout = render_window.GetFramebufferLayout();
    const auto generated_wait_start = std::chrono::steady_clock::now();
    Frame* const generated = present_manager.GetRenderFrame();
    fsr_fg_wait_ms += std::chrono::duration<f32, std::milli>(std::chrono::steady_clock::now() -
                                                             generated_wait_start)
                          .count();
    blit_swapchain.PrepareFrame(device, generated, layout);
    const VkFormat format = present_manager.SwapchainImageFormat();
    const u32 transfer_function = format == VK_FORMAT_A2B10G10R10_UNORM_PACK32   ? 1U
                                  : format == VK_FORMAT_R16G16B16A16_SFLOAT ? 2U
                                                                            : 0U;
    const s32 left = static_cast<s32>((std::min)(layout.screen.left, frame->width));
    const s32 top = static_cast<s32>((std::min)(layout.screen.top, frame->height));
    const u32 width = (std::min)(layout.screen.GetWidth(), frame->width - static_cast<u32>(left));
    const u32 height =
        (std::min)(layout.screen.GetHeight(), frame->height - static_cast<u32>(top));
    // Testing: EDEN_FG_DEBUG=1 presents a copy of the real frame in the generated one's place
    // (the pacing alone), 2 shows only generated frames (the real one is overwritten with it), 3
    // shows the UI mask that way, 4 generated frames without the UI mask, 5 the frame without
    // its UI (what frame generation moves) that way.
    static const int debug_mode = [] {
        const char* value = std::getenv("EDEN_FG_DEBUG");
        return value != nullptr ? std::atoi(value) : 0;
    }();
    if (!generated->storage_view || generated->width != frame->width ||
        generated->height != frame->height || width == 0 || height == 0 || debug_mode == 1) {
        // Shouldn't happen (the frames are made alike); show the real frame twice.
        generator->Skip(game_frame.seq);
        scheduler.Record([source = *frame->image, destination = *generated->image,
                          w = frame->width, h = frame->height](vk::CommandBuffer cmdbuf) {
            const VkImageMemoryBarrier barrier{
                .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
                .pNext = nullptr,
                .srcAccessMask = 0,
                .dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,
                .oldLayout = VK_IMAGE_LAYOUT_UNDEFINED,
                .newLayout = VK_IMAGE_LAYOUT_GENERAL,
                .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                .image = destination,
                .subresourceRange{VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1},
            };
            cmdbuf.PipelineBarrier(VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
                                   VK_PIPELINE_STAGE_TRANSFER_BIT, 0, {}, {}, barrier);
            cmdbuf.CopyImage(source, VK_IMAGE_LAYOUT_GENERAL, destination, VK_IMAGE_LAYOUT_GENERAL,
                             VkImageCopy{
                                 .srcSubresource{VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1},
                                 .srcOffset{},
                                 .dstSubresource{VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1},
                                 .dstOffset{},
                                 .extent{w, h, 1},
                             });
        });
        scheduler.Flush(*generated->render_ready);
        present_manager.Present(generated);
        return;
    }
    std::array<VkImageView, 4> ui_layers{};
    if (debug_mode != 4) {
        ui_layers = game_frame.layers;
    }
    // A late frame's UI layer is no longer there: the copy taken when it was combined is used
    // (the generator prefers that copy whenever it has one).
    const bool late = game_frame.previous && debug_mode != 4;
    if (ui_layers[0] || late) {
        ++fsr_fg_ui;
    }
    for (const VkImageView layer : ui_layers) {
        fsr_fg_ui_layers += layer ? 1 : 0;
    }
    fsr_fg_ui_layers += late ? 1 : 0;
    generator->Generate({
        .current_image = *frame->image,
        .current_view = *frame->storage_view,
        .output_image = *generated->image,
        .output_view = *generated->storage_view,
        .format = format,
        .display_size = {frame->width, frame->height},
        .interpolation_rect = {.offset = {left, top}, .extent = {width, height}},
        .frame_time_ms = fsr_fg_interval_ms,
        .reset = false,
        .transfer_function = transfer_function,
        .min_max_luminance = {0.0f, 1000.0f},
        .ui_layers = ui_layers,
        .show_ui_mask = debug_mode == 3,
        .seq = game_frame.seq,
        .late = late,
        .show_ui_less = debug_mode == 5,
    });
    if (debug_mode >= 2) {
        scheduler.Record([source = *generated->image, destination = *frame->image,
                          w = frame->width, h = frame->height](vk::CommandBuffer cmdbuf) {
            cmdbuf.CopyImage(source, VK_IMAGE_LAYOUT_GENERAL, destination, VK_IMAGE_LAYOUT_GENERAL,
                             VkImageCopy{
                                 .srcSubresource{VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1},
                                 .srcOffset{},
                                 .dstSubresource{VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1},
                                 .dstOffset{},
                                 .extent{w, h, 1},
                             });
            cmdbuf.PipelineBarrier(VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
                                   0, VkMemoryBarrier{
                                          .sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER,
                                          .pNext = nullptr,
                                          .srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,
                                          .dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT,
                                      });
        });
    }
    // The generated frame goes up as soon as it is ready, the real one about half the game's
    // interval later (a little less, so a late frame catches up). Only the real one waits: when
    // both did, the emulated GPU waited for free frames behind the waits, the game slowed, the
    // measured interval grew and the waits with it (60 FPS settled at 30).
    generated->present_delay = {};
    frame->present_delay = std::chrono::duration_cast<std::chrono::nanoseconds>(
        std::chrono::duration<f32, std::milli>(fsr_fg_interval_ms * 0.45f));
    scheduler.Flush(*generated->render_ready);
    present_manager.Present(generated);
    Settings::SetActiveFrameGeneration("AMD FSR 3.1");
    ++fsr_fg_made;
    if (++fsr_fg_generated == 1) {
        LOG_INFO(Render_Vulkan, "AMD FSR 3.1 frame generation: first frame made ({:.1f} ms apart)",
                 fsr_fg_interval_ms);
    }
}

void RendererVulkan::Report() const {
    using namespace Common::Literals;
    const std::string vendor_name{device.GetVendorName()};
    const std::string model_name{device.GetModelName()};
    const std::string driver_version = GetDriverVersion(device);
    const std::string driver_name = fmt::format("{} {}", vendor_name, driver_version);

    const std::string api_version = GetReadableVersion(device.ApiVersion());

    const std::string extensions = BuildCommaSeparatedExtensions(device.GetAvailableExtensions());

    const auto available_vram = static_cast<f64>(device.GetDeviceLocalMemory()) / f64{1_GiB};

    LOG_INFO(Render_Vulkan, "Driver: {}", driver_name);
    LOG_INFO(Render_Vulkan, "Device: {}", model_name);
    LOG_INFO(Render_Vulkan, "Vulkan: {}", api_version);
    LOG_INFO(Render_Vulkan, "Available VRAM: {:.2f} GiB", available_vram);
}

vk::Buffer RendererVulkan::RenderToBuffer(std::span<const Tegra::FramebufferConfig> framebuffers,
                                          const Layout::FramebufferLayout& layout, VkFormat format,
                                          VkDeviceSize buffer_size) {
    auto frame = [&]() {
        Frame f{};
        f.image =
            CreateWrappedImage(memory_allocator, VkExtent2D{layout.width, layout.height}, format);
        f.image_view = CreateWrappedImageView(device, f.image, format);
        f.framebuffer = blit_capture.CreateFramebuffer(device, layout, *f.image_view, format);
        return f;
    }();

    auto dst_buffer = CreateWrappedBuffer(memory_allocator, buffer_size, MemoryUsage::Download);
    blit_capture.DrawToFrame(device, rasterizer, &frame, framebuffers, layout, 1, format);

    scheduler.RequestOutsideRenderPassOperationContext();
    scheduler.Record([&](vk::CommandBuffer cmdbuf) {
        DownloadColorImage(cmdbuf, *frame.image, *dst_buffer,
                           VkExtent3D{layout.width, layout.height, 1});
    });

    // Ensure the copy is fully completed before saving the capture
    scheduler.Finish();

    // Copy backing image data to the capture buffer
    dst_buffer.Invalidate();
    return dst_buffer;
}

void RendererVulkan::RenderScreenshot(std::span<const Tegra::FramebufferConfig> framebuffers) {
    if (!renderer_settings.screenshot_requested) {
        return;
    }

    const auto screenshot_layers = Tegra::FilterLayerStack(
        framebuffers, renderer_settings.screenshot_layer_stack, screenshot_layer_scratch);

    const auto& layout{renderer_settings.screenshot_framebuffer_layout};
    const auto dst_buffer = RenderToBuffer(screenshot_layers, layout, VK_FORMAT_B8G8R8A8_UNORM,
                                           layout.width * layout.height * 4);

    std::memcpy(renderer_settings.screenshot_bits, dst_buffer.Mapped().data(),
                dst_buffer.Mapped().size());
    renderer_settings.screenshot_complete_callback(false);
    renderer_settings.screenshot_requested = false;
}

std::vector<u8> RendererVulkan::GetAppletCaptureBuffer() {
    using namespace VideoCore::Capture;

    std::vector<u8> out(VideoCore::Capture::TiledSize);

    if (!applet_frame.image) {
        return out;
    }

    const auto dst_buffer =
        CreateWrappedBuffer(memory_allocator, VideoCore::Capture::TiledSize, MemoryUsage::Download);

    scheduler.RequestOutsideRenderPassOperationContext();
    scheduler.Record([&](vk::CommandBuffer cmdbuf) {
        DownloadColorImage(cmdbuf, *applet_frame.image, *dst_buffer, CaptureImageExtent);
    });

    // Ensure the copy is fully completed before writing the capture
    scheduler.Finish();

    // Swizzle image data to the capture buffer
    dst_buffer.Invalidate();
    Tegra::Texture::SwizzleTexture(out, dst_buffer.Mapped(), BytesPerPixel, LinearWidth,
                                   LinearHeight, LinearDepth, BlockHeight, BlockDepth);

    return out;
}

void RendererVulkan::RenderAppletCaptureLayer(
    std::span<const Tegra::FramebufferConfig> framebuffers) {
    const auto capture_layers = Tegra::FilterLayerStack(
        framebuffers, Service::Nvnflinger::LayerStackId::LastFrame, applet_capture_layers);

    if (capture_layers.empty())
        return;

    if (!applet_frame.image) {
        applet_frame.image = CreateWrappedImage(memory_allocator, CaptureImageSize, CaptureFormat);
        applet_frame.image_view = CreateWrappedImageView(device, applet_frame.image, CaptureFormat);
        applet_frame.framebuffer = blit_applet.CreateFramebuffer(device,
            VideoCore::Capture::Layout, *applet_frame.image_view, CaptureFormat);
    }

    // The capture is drawn every frame. With one set of layer resources each frame waited for
    // the host GPU to finish the previous one; rotating through three lets the GPU thread stay
    // ahead like the swapchain blit does.
    constexpr size_t CaptureResourceSets = 3;
    scheduler.RequestOutsideRenderPassOperationContext();
    blit_applet.DrawToFrame(device, rasterizer, &applet_frame, capture_layers, VideoCore::Capture::Layout,
                            CaptureResourceSets, CaptureFormat);
}

} // namespace Vulkan
