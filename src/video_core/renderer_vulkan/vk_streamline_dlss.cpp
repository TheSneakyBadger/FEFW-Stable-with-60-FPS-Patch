// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

// NVIDIA DLSS (through NVIDIA Streamline) in place of a game's DLSS evaluation.

#include <array>
#include <cstdlib>
#include <string_view>

#include "common/logging.h"
#include "video_core/renderer_vulkan/vk_scheduler.h"
#include "video_core/renderer_vulkan/vk_streamline_dlss.h"
#include "video_core/vulkan_common/streamline.h"
#include "video_core/vulkan_common/streamline_api.h"
#include "video_core/vulkan_common/vulkan_device.h"
#include "video_core/vulkan_common/vulkan_memory_allocator.h"

namespace Vulkan {
namespace {

constexpr VkFormat OUTPUT_FORMAT = VK_FORMAT_R16G16B16A16_SFLOAT;
// Streamline clears the output before DLSS writes it.
constexpr VkImageUsageFlags OUTPUT_USAGE = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_STORAGE_BIT |
                                           VK_IMAGE_USAGE_TRANSFER_SRC_BIT |
                                           VK_IMAGE_USAGE_TRANSFER_DST_BIT;

int EnvInt(const char* name, int fallback) {
    const char* const value = std::getenv(name);
    return value ? std::atoi(value) : fallback;
}

VkImageMemoryBarrier Barrier(const StreamlineDlssUpscaler::Image& image, VkAccessFlags src_access,
                             VkAccessFlags dst_access, VkImageLayout old_layout,
                             VkImageLayout new_layout) {
    VkImageSubresourceRange range = image.range;
    range.aspectMask = image.barrier_aspect;
    return VkImageMemoryBarrier{
        .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
        .pNext = nullptr,
        .srcAccessMask = src_access,
        .dstAccessMask = dst_access,
        .oldLayout = old_layout,
        .newLayout = new_layout,
        .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .image = image.image,
        .subresourceRange = range,
    };
}

#ifdef _WIN32
// DLSS validates the render size against the mode's range: pick the mode for this ratio.
sl::DLSSMode ModeFor(VkExtent2D render, VkExtent2D upscale) {
    const f32 ratio = static_cast<f32>(render.width) / static_cast<f32>(upscale.width);
    if (ratio >= 0.99f) {
        return sl::DLSSMode::eDLAA;
    }
    // Ultra Quality (0.77) runs as Quality: DLSS turns down its own Ultra Quality mode, and every
    // mode here uses the same preset, so only the accepted range matters.
    if (ratio >= 0.65f) {
        return sl::DLSSMode::eMaxQuality;
    }
    if (ratio >= 0.57f) {
        return sl::DLSSMode::eBalanced;
    }
    if (ratio >= 0.49f) {
        return sl::DLSSMode::eMaxPerformance;
    }
    return sl::DLSSMode::eUltraPerformance;
}

// Preset K for every mode: NVIDIA's defaults for Performance/Ultra Performance (M, L) draw thin
// horizontal lines in Fire Emblem at 2x resolution scale. EDEN_NX2_DLSS_PRESET=J|K|L|M picks
// another model, EDEN_NX2_DLSS_PRESET=default leaves the choice to NVIDIA.
sl::DLSSPreset Preset() {
    const char* const value = std::getenv("EDEN_NX2_DLSS_PRESET");
    if (value && value[0] >= 'J' && value[0] <= 'M') {
        return static_cast<sl::DLSSPreset>(static_cast<u32>(sl::DLSSPreset::ePresetJ) +
                                           static_cast<u32>(value[0] - 'J'));
    }
    if (value && std::string_view{value} == "default") {
        return sl::DLSSPreset::eDefault;
    }
    return sl::DLSSPreset::ePresetK;
}

sl::float4x4 Identity() {
    sl::float4x4 matrix;
    matrix.setRow(0, {1.0f, 0.0f, 0.0f, 0.0f});
    matrix.setRow(1, {0.0f, 1.0f, 0.0f, 0.0f});
    matrix.setRow(2, {0.0f, 0.0f, 1.0f, 0.0f});
    matrix.setRow(3, {0.0f, 0.0f, 0.0f, 1.0f});
    return matrix;
}

sl::Resource Resource(VkImage image, VkImageView view, VkFormat format, VkImageLayout layout,
                      VkExtent2D size, VkImageUsageFlags usage) {
    sl::Resource resource{sl::ResourceType::eTex2d, image, nullptr, view,
                          static_cast<uint32_t>(layout)};
    resource.width = size.width;
    resource.height = size.height;
    resource.nativeFormat = static_cast<uint32_t>(format);
    resource.mipLevels = 1;
    resource.arrayLayers = 1;
    resource.flags = 0;
    resource.usage = usage;
    return resource;
}
#else
Streamline::NgxPerfQuality NgxQualityFor(VkExtent2D render, VkExtent2D upscale) {
    const f32 ratio = static_cast<f32>(render.width) / static_cast<f32>(upscale.width);
    if (ratio >= 0.99f) {
        return Streamline::NgxPerfQuality::DLAA;
    }
    if (ratio >= 0.65f) {
        return Streamline::NgxPerfQuality::MaxQuality;
    }
    if (ratio >= 0.57f) {
        return Streamline::NgxPerfQuality::Balanced;
    }
    if (ratio >= 0.49f) {
        return Streamline::NgxPerfQuality::MaxPerf;
    }
    return Streamline::NgxPerfQuality::UltraPerformance;
}

Streamline::NgxDlssPreset NgxPreset() {
    const char* const value = std::getenv("EDEN_NX2_DLSS_PRESET");
    if (value && value[0] >= 'J' && value[0] <= 'M') {
        return static_cast<Streamline::NgxDlssPreset>(
            static_cast<u32>(Streamline::NgxDlssPreset::J) + static_cast<u32>(value[0] - 'J'));
    }
    if (value && std::string_view{value} == "default") {
        return Streamline::NgxDlssPreset::Default;
    }
    return Streamline::NgxDlssPreset::K;
}
#endif

} // Anonymous namespace

StreamlineDlssUpscaler::StreamlineDlssUpscaler(const Device& device_, Scheduler& scheduler_,
                                               MemoryAllocator& memory_allocator_)
    : device{device_}, scheduler{scheduler_}, memory_allocator{memory_allocator_} {
    if (!Streamline::IsDlssAvailable()) {
        throw vk::Exception(VK_ERROR_FEATURE_NOT_PRESENT);
    }
#ifdef _WIN32
    const sl::Result result = Streamline::GetApi().get_feature_function(
        sl::kFeatureDLSS, "slDLSSSetOptions", set_options);
    if (result != sl::Result::eOk || !set_options) {
        LOG_ERROR(Render_Vulkan, "Streamline's DLSS has no slDLSSSetOptions (result {})",
                  static_cast<int>(result));
        throw vk::Exception(VK_ERROR_FEATURE_NOT_PRESENT);
    }
#endif
    point_sampler = device.GetLogical().CreateSampler(VkSamplerCreateInfo{
        .sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO,
        .pNext = nullptr,
        .flags = 0,
        .magFilter = VK_FILTER_NEAREST,
        .minFilter = VK_FILTER_NEAREST,
        .mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST,
        .addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
        .addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
        .addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
        .mipLodBias = 0.0f,
        .anisotropyEnable = VK_FALSE,
        .maxAnisotropy = 1.0f,
        .compareEnable = VK_FALSE,
        .compareOp = VK_COMPARE_OP_NEVER,
        .minLod = 0.0f,
        .maxLod = VK_LOD_CLAMP_NONE,
        .borderColor = VK_BORDER_COLOR_FLOAT_TRANSPARENT_BLACK,
        .unnormalizedCoordinates = VK_FALSE,
    });
}

StreamlineDlssUpscaler::~StreamlineDlssUpscaler() {
    // Not scheduler.Finish(): this runs when the renderer goes away, after the game's channels are
    // released, and a submit would touch their state. Nothing more gets recorded by then.
    void(device.GetLogical().WaitIdle());
    if (Streamline::IsDlssAvailable()) {
#ifdef _WIN32
        Streamline::GetApi().free_resources(sl::kFeatureDLSS, sl::ViewportHandle{0u});
#else
        if (set_options) {
            Streamline::GetNgxApi().release_feature(
                reinterpret_cast<Streamline::NgxHandle*>(set_options));
            set_options = nullptr;
        }
#endif
    }
}

void StreamlineDlssUpscaler::CreateOutput(VkExtent2D upscale_size) {
    if (output_image) {
        scheduler.Finish(); // the old output may still be in use
    }
    output_image = memory_allocator.CreateImage(VkImageCreateInfo{
        .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
        .pNext = nullptr,
        .flags = 0,
        .imageType = VK_IMAGE_TYPE_2D,
        .format = OUTPUT_FORMAT,
        .extent = {upscale_size.width, upscale_size.height, 1},
        .mipLevels = 1,
        .arrayLayers = 1,
        .samples = VK_SAMPLE_COUNT_1_BIT,
        .tiling = VK_IMAGE_TILING_OPTIMAL,
        .usage = OUTPUT_USAGE,
        .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
        .queueFamilyIndexCount = 0,
        .pQueueFamilyIndices = nullptr,
        .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
    });
    output_view = device.GetLogical().CreateImageView(VkImageViewCreateInfo{
        .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
        .pNext = nullptr,
        .flags = 0,
        .image = *output_image,
        .viewType = VK_IMAGE_VIEW_TYPE_2D,
        .format = OUTPUT_FORMAT,
        .components{},
        .subresourceRange{
            .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
            .baseMipLevel = 0,
            .levelCount = 1,
            .baseArrayLayer = 0,
            .layerCount = 1,
        },
    });
    scheduler.RequestOutsideRenderPassOperationContext();
    scheduler.Record([image = *output_image](vk::CommandBuffer cmdbuf) {
        cmdbuf.PipelineBarrier(VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                               VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0,
                               VkImageMemoryBarrier{
                                   .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
                                   .pNext = nullptr,
                                   .srcAccessMask = 0,
                                   .dstAccessMask = VK_ACCESS_SHADER_WRITE_BIT,
                                   .oldLayout = VK_IMAGE_LAYOUT_UNDEFINED,
                                   .newLayout = VK_IMAGE_LAYOUT_GENERAL,
                                   .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                                   .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                                   .image = image,
                                   .subresourceRange{
                                       .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
                                       .baseMipLevel = 0,
                                       .levelCount = 1,
                                       .baseArrayLayer = 0,
                                       .layerCount = 1,
                                   },
                               });
    });
}

bool StreamlineDlssUpscaler::Upscale(const Inputs& in) {
    if (failed) {
        return false;
    }
    // Streamline logs NGX failing inside an evaluation but returns success, and the output is
    // never written (black picture, missing characters; issue #34). A few such failures since
    // the last frame count as a failed evaluation, so the caller's fallback takes over.
    const u64 evaluate_failures = Streamline::EvaluateFailures();
    if (!seen_evaluate_failures) {
        seen_evaluate_failures = evaluate_failures; // failures before this upscaler don't count
    }
    if (evaluate_failures >= *seen_evaluate_failures + 3) {
        seen_evaluate_failures = evaluate_failures;
        if (!failed.exchange(true)) {
            LOG_ERROR(Render_Vulkan,
                      "NVIDIA DLSS evaluations fail inside NGX (see the Streamline errors above)");
        }
        return false;
    }
    bool reset = in.reset;
    bool options_changed = false;
    if (!output_image || in.render_size.width != render_extent.width ||
        in.render_size.height != render_extent.height ||
        in.upscale_size.width != upscale_extent.width ||
        in.upscale_size.height != upscale_extent.height) {
        CreateOutput(in.upscale_size);
        render_extent = in.render_size;
        upscale_extent = in.upscale_size;
        options_changed = true;
        reset = true;
    }

#ifdef _WIN32
    // The game's own setup (Fortune's Weave: render-size motion vectors, reversed depth). Its
    // colour is linear and goes above 1 with no exposure passed, so by default DLSS gets the HDR
    // path with its own auto exposure. EDEN_NX2_DLSS_HDR=0 / EDEN_NX2_DLSS_AUTO_EXPOSURE=0 turn
    // those off.
    sl::DLSSOptions options{};
    options.mode = ModeFor(in.render_size, in.upscale_size);
    options.outputWidth = in.upscale_size.width;
    options.outputHeight = in.upscale_size.height;
    options.colorBuffersHDR = EnvInt("EDEN_NX2_DLSS_HDR", 1) != 0 ? sl::eTrue : sl::eFalse;
    options.useAutoExposure =
        EnvInt("EDEN_NX2_DLSS_AUTO_EXPOSURE", 1) != 0 ? sl::eTrue : sl::eFalse;
    const sl::DLSSPreset preset = Preset();
    options.dlaaPreset = preset;
    options.qualityPreset = preset;
    options.balancedPreset = preset;
    options.performancePreset = preset;
    options.ultraPerformancePreset = preset;
    options.ultraQualityPreset = preset;
    if (options_changed) {
        LOG_INFO(Render_Vulkan, "NVIDIA DLSS (Streamline): {}x{} -> {}x{}, mode {}, preset {}",
                 in.render_size.width, in.render_size.height, in.upscale_size.width,
                 in.upscale_size.height, static_cast<int>(options.mode),
                 static_cast<int>(preset));
    }

    scheduler.RequestOutsideRenderPassOperationContext();
    scheduler.Record([this, in, reset, options, options_changed, index = frame_index++,
                      output_image_handle = *output_image,
                      output_view_handle = *output_view](vk::CommandBuffer cmdbuf) {
        const Streamline::Api& api = Streamline::GetApi();
        const sl::ViewportHandle viewport{0u};
        // The caller draws the output this frame even when DLSS writes nothing to it: clear it
        // then, rather than show undefined contents (the fallback takes over from the next frame).
        const auto clear_output = [&] {
            const VkImageSubresourceRange range{
                .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
                .baseMipLevel = 0,
                .levelCount = 1,
                .baseArrayLayer = 0,
                .layerCount = 1,
            };
            const auto barrier = [&](VkAccessFlags src_access, VkAccessFlags dst_access) {
                return VkImageMemoryBarrier{
                    .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
                    .pNext = nullptr,
                    .srcAccessMask = src_access,
                    .dstAccessMask = dst_access,
                    .oldLayout = VK_IMAGE_LAYOUT_GENERAL,
                    .newLayout = VK_IMAGE_LAYOUT_GENERAL,
                    .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                    .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                    .image = output_image_handle,
                    .subresourceRange = range,
                };
            };
            cmdbuf.PipelineBarrier(VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
                                   VK_PIPELINE_STAGE_TRANSFER_BIT, 0,
                                   barrier(VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT,
                                           VK_ACCESS_TRANSFER_WRITE_BIT));
            cmdbuf.ClearColorImage(output_image_handle, VK_IMAGE_LAYOUT_GENERAL,
                                   VkClearColorValue{}, range);
            cmdbuf.PipelineBarrier(
                VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0,
                barrier(VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT));
        };
        if (options_changed) {
            const auto set = reinterpret_cast<PFun_slDLSSSetOptions*>(set_options);
            if (const sl::Result result = set(viewport, options); result != sl::Result::eOk) {
                if (!failed.exchange(true)) {
                    LOG_ERROR(Render_Vulkan,
                              "Streamline rejected the DLSS options (result {})",
                              static_cast<int>(result));
                }
                clear_output();
                return;
            }
        }
        sl::FrameToken* frame{};
        if (const sl::Result result = api.get_new_frame_token(frame, &index);
            result != sl::Result::eOk || !frame) {
            if (!failed.exchange(true)) {
                LOG_ERROR(Render_Vulkan, "Streamline gave no frame token (result {})",
                          static_cast<int>(result));
            }
            clear_output();
            return;
        }

        constexpr VkAccessFlags WRITES =
            VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT |
            VK_ACCESS_SHADER_WRITE_BIT | VK_ACCESS_TRANSFER_WRITE_BIT;
        // The inputs come from the game's draws and dispatches and stay in GENERAL otherwise;
        // DLSS reads them as sampled images.
        const std::array to_read{
            Barrier(in.color, WRITES, VK_ACCESS_SHADER_READ_BIT, VK_IMAGE_LAYOUT_GENERAL,
                    VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL),
            Barrier(in.depth, WRITES, VK_ACCESS_SHADER_READ_BIT, VK_IMAGE_LAYOUT_GENERAL,
                    VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL),
            Barrier(in.motion_vectors, WRITES, VK_ACCESS_SHADER_READ_BIT,
                    VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL),
        };
        cmdbuf.PipelineBarrier(VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
                               VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, {}, {}, to_read);

        // DLSS super resolution only reads the jitter, motion and depth conventions; the camera
        // fields are for frame generation, which this doesn't use.
        const f32 rw = static_cast<f32>(in.render_size.width);
        const f32 rh = static_cast<f32>(in.render_size.height);
        sl::Constants constants{};
        constants.cameraViewToClip = Identity();
        constants.clipToCameraView = Identity();
        constants.clipToLensClip = Identity();
        constants.clipToPrevClip = Identity();
        constants.prevClipToClip = Identity();
        constants.jitterOffset = {in.jitter[0], in.jitter[1]};
        // Streamline wants motion in [-1,1] screen units; ours turns into render pixels.
        constants.mvecScale = {in.motion_vector_scale[0] / rw, in.motion_vector_scale[1] / rh};
        constants.cameraPinholeOffset = {0.0f, 0.0f};
        constants.cameraPos = {0.0f, 0.0f, 0.0f};
        constants.cameraUp = {0.0f, 1.0f, 0.0f};
        constants.cameraRight = {1.0f, 0.0f, 0.0f};
        constants.cameraFwd = {0.0f, 0.0f, 1.0f};
        constants.cameraNear = 0.1f;
        constants.cameraFar = 1000.0f;
        constants.cameraFOV = 1.0f;
        constants.cameraAspectRatio = rw / rh;
        constants.depthInverted = sl::eTrue;
        constants.cameraMotionIncluded = sl::eTrue;
        constants.motionVectors3D = sl::eFalse;
        constants.reset = reset ? sl::eTrue : sl::eFalse;
        constants.motionVectorsJittered = sl::eFalse;
        api.set_constants(constants, *frame, viewport);

        sl::Resource color = Resource(in.color.image, in.color.view, in.color.format,
                                      VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, in.render_size,
                                      VK_IMAGE_USAGE_SAMPLED_BIT);
        sl::Resource depth = Resource(in.depth.image, in.depth.view, in.depth.format,
                                      VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, in.render_size,
                                      VK_IMAGE_USAGE_SAMPLED_BIT);
        sl::Resource motion = Resource(in.motion_vectors.image, in.motion_vectors.view,
                                       in.motion_vectors.format,
                                       VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, in.render_size,
                                       VK_IMAGE_USAGE_SAMPLED_BIT);
        sl::Resource output = Resource(output_image_handle, output_view_handle, OUTPUT_FORMAT,
                                       VK_IMAGE_LAYOUT_GENERAL, in.upscale_size, OUTPUT_USAGE);
        const sl::Extent render_area{0, 0, in.render_size.width, in.render_size.height};
        const sl::Extent output_area{0, 0, in.upscale_size.width, in.upscale_size.height};
        // Local tags: everything stays put until the evaluation is recorded.
        sl::ResourceTag color_tag{&color, sl::kBufferTypeScalingInputColor,
                                  sl::ResourceLifecycle::eValidUntilEvaluate, &render_area};
        sl::ResourceTag depth_tag{&depth, sl::kBufferTypeDepth,
                                  sl::ResourceLifecycle::eValidUntilEvaluate, &render_area};
        sl::ResourceTag motion_tag{&motion, sl::kBufferTypeMotionVectors,
                                   sl::ResourceLifecycle::eValidUntilEvaluate, &render_area};
        sl::ResourceTag output_tag{&output, sl::kBufferTypeScalingOutputColor,
                                   sl::ResourceLifecycle::eValidUntilEvaluate, &output_area};
        const sl::BaseStructure* inputs[] = {&viewport, &color_tag, &depth_tag, &motion_tag,
                                             &output_tag};
        const sl::Result result =
            api.evaluate_feature(sl::kFeatureDLSS, *frame, inputs, std::size(inputs),
                                 reinterpret_cast<sl::CommandBuffer*>(*cmdbuf));
        if (result != sl::Result::eOk && !failed.exchange(true)) {
            LOG_ERROR(Render_Vulkan, "NVIDIA DLSS evaluation failed (Streamline result {})",
                      static_cast<int>(result));
        }

        const std::array to_general{
            Barrier(in.color, VK_ACCESS_SHADER_READ_BIT, 0,
                    VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_IMAGE_LAYOUT_GENERAL),
            Barrier(in.depth, VK_ACCESS_SHADER_READ_BIT, 0,
                    VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_IMAGE_LAYOUT_GENERAL),
            Barrier(in.motion_vectors, VK_ACCESS_SHADER_READ_BIT, 0,
                    VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_IMAGE_LAYOUT_GENERAL),
        };
        const VkMemoryBarrier output_written{
            .sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER,
            .pNext = nullptr,
            .srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT,
            .dstAccessMask = VK_ACCESS_SHADER_READ_BIT,
        };
        cmdbuf.PipelineBarrier(VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                               VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, output_written, {},
                               to_general);
        if (result != sl::Result::eOk) {
            clear_output();
        }
    });
#else
    const Streamline::NgxPerfQuality quality = NgxQualityFor(in.render_size, in.upscale_size);
    const Streamline::NgxDlssPreset preset = NgxPreset();
    if (options_changed) {
        LOG_INFO(Render_Vulkan, "NVIDIA DLSS (Linux NGX): {}x{} -> {}x{}, quality {}, preset {}",
                 in.render_size.width, in.render_size.height, in.upscale_size.width,
                 in.upscale_size.height, static_cast<int>(quality), static_cast<int>(preset));
    }
    scheduler.RequestOutsideRenderPassOperationContext();
    scheduler.Record([this, in, reset, quality, preset, options_changed,
                      output_image_handle = *output_image,
                      output_view_handle = *output_view](vk::CommandBuffer cmdbuf) {
        const Streamline::NgxApi& api = Streamline::GetNgxApi();
        Streamline::NgxParameter* const params = Streamline::GetNgxParameters();
        const auto clear_output = [&] {
            const VkImageSubresourceRange range{
                .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
                .baseMipLevel = 0,
                .levelCount = 1,
                .baseArrayLayer = 0,
                .layerCount = 1,
            };
            const auto barrier = [&](VkAccessFlags src_access, VkAccessFlags dst_access) {
                return VkImageMemoryBarrier{
                    .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
                    .pNext = nullptr,
                    .srcAccessMask = src_access,
                    .dstAccessMask = dst_access,
                    .oldLayout = VK_IMAGE_LAYOUT_GENERAL,
                    .newLayout = VK_IMAGE_LAYOUT_GENERAL,
                    .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                    .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                    .image = output_image_handle,
                    .subresourceRange = range,
                };
            };
            cmdbuf.PipelineBarrier(VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
                                   VK_PIPELINE_STAGE_TRANSFER_BIT, 0,
                                   barrier(VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT,
                                           VK_ACCESS_TRANSFER_WRITE_BIT));
            cmdbuf.ClearColorImage(output_image_handle, VK_IMAGE_LAYOUT_GENERAL,
                                   VkClearColorValue{}, range);
            cmdbuf.PipelineBarrier(
                VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0,
                barrier(VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT));
        };
        if (!params || !api.create_feature1 || !api.evaluate_feature) {
            failed = true;
            clear_output();
            return;
        }
        auto* handle = reinterpret_cast<Streamline::NgxHandle*>(set_options);
        if (options_changed || !handle) {
            if (handle) {
                std::scoped_lock lock{scheduler.submit_mutex};
                void(device.GetLogical().WaitIdle());
                api.release_feature(handle);
                handle = nullptr;
                set_options = nullptr;
            }
            int create_flags = Streamline::NgxDlssFlagMVLowRes |
                               Streamline::NgxDlssFlagDepthInverted;
            if (EnvInt("EDEN_NX2_DLSS_HDR", 1) != 0) {
                create_flags |= Streamline::NgxDlssFlagIsHDR;
            }
            if (EnvInt("EDEN_NX2_DLSS_AUTO_EXPOSURE", 1) != 0) {
                create_flags |= Streamline::NgxDlssFlagAutoExposure;
            }
            const unsigned int preset_val = static_cast<unsigned int>(preset);
            params->SetUI("DLSS.Hint.Render.Preset.DLAA", preset_val);
            params->SetUI("DLSS.Hint.Render.Preset.Quality", preset_val);
            params->SetUI("DLSS.Hint.Render.Preset.Balanced", preset_val);
            params->SetUI("DLSS.Hint.Render.Preset.Performance", preset_val);
            params->SetUI("DLSS.Hint.Render.Preset.UltraPerformance", preset_val);
            params->SetUI("DLSS.Hint.Render.Preset.UltraQuality", preset_val);
            params->SetUI("CreationNodeMask", 1u);
            params->SetUI("VisibilityNodeMask", 1u);
            params->SetUI("Width", in.render_size.width);
            params->SetUI("Height", in.render_size.height);
            params->SetUI("OutWidth", in.upscale_size.width);
            params->SetUI("OutHeight", in.upscale_size.height);
            params->SetI("PerfQualityValue", static_cast<int>(quality));
            params->SetI("DLSS.Feature.Create.Flags", create_flags);
            params->SetI("DLSS.Enable.Output.Subrects", 0);

            const Streamline::NgxResult create_res = api.create_feature1(
                *device.GetLogical(), *cmdbuf, Streamline::NgxFeature::SuperSampling, params,
                &handle);
            if (Streamline::NgxFailed(create_res) || !handle) {
                if (!failed.exchange(true)) {
                    LOG_ERROR(Render_Vulkan, "NGX failed to create DLSS feature (0x{:x})",
                              create_res);
                }
                clear_output();
                return;
            }
            set_options = handle;
        }

        constexpr VkAccessFlags WRITES =
            VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT |
            VK_ACCESS_SHADER_WRITE_BIT | VK_ACCESS_TRANSFER_WRITE_BIT;
        const std::array to_read{
            Barrier(in.color, WRITES, VK_ACCESS_SHADER_READ_BIT, VK_IMAGE_LAYOUT_GENERAL,
                    VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL),
            Barrier(in.depth, WRITES, VK_ACCESS_SHADER_READ_BIT, VK_IMAGE_LAYOUT_GENERAL,
                    VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL),
            Barrier(in.motion_vectors, WRITES, VK_ACCESS_SHADER_READ_BIT,
                    VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL),
        };
        cmdbuf.PipelineBarrier(VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
                               VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, {}, {}, to_read);

        Streamline::NgxResourceVk color_res = Streamline::MakeNgxImageViewResource(
            in.color.view, in.color.image, in.color.range, in.color.format,
            in.render_size.width, in.render_size.height, false);
        Streamline::NgxResourceVk depth_res = Streamline::MakeNgxImageViewResource(
            in.depth.view, in.depth.image, in.depth.range, in.depth.format,
            in.render_size.width, in.render_size.height, false);
        Streamline::NgxResourceVk motion_res = Streamline::MakeNgxImageViewResource(
            in.motion_vectors.view, in.motion_vectors.image, in.motion_vectors.range,
            in.motion_vectors.format, in.render_size.width, in.render_size.height, false);
        const VkImageSubresourceRange out_range{
            .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
            .baseMipLevel = 0,
            .levelCount = 1,
            .baseArrayLayer = 0,
            .layerCount = 1,
        };
        Streamline::NgxResourceVk output_res = Streamline::MakeNgxImageViewResource(
            output_view_handle, output_image_handle, out_range, OUTPUT_FORMAT,
            in.upscale_size.width, in.upscale_size.height, true);

        params->SetVoidPointer("Color", &color_res);
        params->SetVoidPointer("Output", &output_res);
        params->SetVoidPointer("Depth", &depth_res);
        params->SetVoidPointer("MotionVectors", &motion_res);
        params->SetF("Jitter.Offset.X", in.jitter[0]);
        params->SetF("Jitter.Offset.Y", in.jitter[1]);
        params->SetF("Sharpness", 0.0f);
        params->SetI("Reset", reset ? 1 : 0);
        params->SetF("MV.Scale.X", in.motion_vector_scale[0] == 0.0f ? 1.0f : in.motion_vector_scale[0]);
        params->SetF("MV.Scale.Y", in.motion_vector_scale[1] == 0.0f ? 1.0f : in.motion_vector_scale[1]);
        params->SetVoidPointer("TransparencyMask", nullptr);
        params->SetVoidPointer("ExposureTexture", nullptr);
        params->SetVoidPointer("DLSS.Input.Bias.Current.Color.Mask", nullptr);
        params->SetUI("DLSS.Input.Color.Subrect.Base.X", 0u);
        params->SetUI("DLSS.Input.Color.Subrect.Base.Y", 0u);
        params->SetUI("DLSS.Input.Depth.Subrect.Base.X", 0u);
        params->SetUI("DLSS.Input.Depth.Subrect.Base.Y", 0u);
        params->SetUI("DLSS.Input.MV.Subrect.Base.X", 0u);
        params->SetUI("DLSS.Input.MV.Subrect.Base.Y", 0u);
        params->SetUI("DLSS.Input.Translucency.Subrect.Base.X", 0u);
        params->SetUI("DLSS.Input.Translucency.Subrect.Base.Y", 0u);
        params->SetUI("DLSS.Input.Bias.Current.Color.Subrect.Base.X", 0u);
        params->SetUI("DLSS.Input.Bias.Current.Color.Subrect.Base.Y", 0u);
        params->SetUI("DLSS.Output.Subrect.Base.X", 0u);
        params->SetUI("DLSS.Output.Subrect.Base.Y", 0u);
        params->SetUI("DLSS.Render.Subrect.Dimensions.Width", in.render_size.width);
        params->SetUI("DLSS.Render.Subrect.Dimensions.Height", in.render_size.height);
        params->SetF("DLSS.Pre.Exposure", 1.0f);
        params->SetF("DLSS.Exposure.Scale", 1.0f);
        params->SetI("DLSS.Indicator.Invert.X.Axis", 0);
        params->SetI("DLSS.Indicator.Invert.Y.Axis", 0);

        const Streamline::NgxResult eval_res = api.evaluate_feature(*cmdbuf, handle, params, nullptr);
        if (Streamline::NgxFailed(eval_res) && !failed.exchange(true)) {
            LOG_ERROR(Render_Vulkan, "NVIDIA DLSS evaluation failed (NGX 0x{:x})", eval_res);
        }

        const std::array to_general{
            Barrier(in.color, VK_ACCESS_SHADER_READ_BIT, 0,
                    VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_IMAGE_LAYOUT_GENERAL),
            Barrier(in.depth, VK_ACCESS_SHADER_READ_BIT, 0,
                    VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_IMAGE_LAYOUT_GENERAL),
            Barrier(in.motion_vectors, VK_ACCESS_SHADER_READ_BIT, 0,
                    VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_IMAGE_LAYOUT_GENERAL),
        };
        const VkMemoryBarrier output_written{
            .sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER,
            .pNext = nullptr,
            .srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT,
            .dstAccessMask = VK_ACCESS_SHADER_READ_BIT,
        };
        cmdbuf.PipelineBarrier(VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                               VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, output_written, {},
                               to_general);
        if (Streamline::NgxFailed(eval_res)) {
            clear_output();
        }
    });
#endif
    // DLSS binds its own pipelines and descriptors on the command buffer.
    scheduler.InvalidateState();
    return true;
}

} // namespace Vulkan
