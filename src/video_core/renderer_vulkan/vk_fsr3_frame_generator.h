// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <array>
#include <memory>
#include <vector>

#include "common/common_types.h"
#include "video_core/renderer_vulkan/vk_update_descriptor.h"
#include "video_core/vulkan_common/vulkan_wrapper.h"

namespace Vulkan {

class DescriptorPool;
class Device;
class MemoryAllocator;
class Scheduler;
class StagingBufferPool;

/// AMD FidelityFX frame generation (FSR 3.1's frame interpolation and optical flow, FidelityFX SDK
/// v1.1.4, shaders in externals/FidelityFX-SDK): makes a frame halfway between two presented
/// frames. The host side follows the SDK's ffx_frameinterpolation.cpp and ffx_opticalflow.cpp
/// (the same passes, internal images and constants) and is recorded through the scheduler, like
/// the FSR 3.1 upscaler. The game's motion vectors and depth come from its DLSS evaluation
/// (Prepare, render size); the colour is the presented frame, with the game's UI layer taken out
/// (and put back over the generated frame) when it is known.
class Fsr3FrameGenerator {
public:
    struct PrepareInputs {
        VkImageView depth;          ///< Device depth (reversed) at the render size.
        VkImageView motion_vectors; ///< Motion to the previous frame at the render size.
        VkExtent2D render_size;
        std::array<f32, 2> jitter;              ///< In render pixels.
        std::array<f32, 2> motion_vector_scale; ///< Turns a motion vector into UV units.
    };

    struct GenerateInputs {
        VkImage current_image;    ///< The frame being presented (GENERAL).
        VkImageView current_view; ///< A view of it in its storage (UNORM) format.
        VkImage output_image;     ///< Where the generated frame goes (any layout; GENERAL after).
        VkImageView output_view;  ///< A storage view of it.
        VkFormat format;          ///< Both images' format.
        VkExtent2D display_size;  ///< Both images' size.
        VkRect2D interpolation_rect; ///< Where the game's picture is in them.
        f32 frame_time_ms;
        bool reset;
        u32 transfer_function; ///< 0 sRGB, 1 PQ, 2 scRGB.
        std::array<f32, 2> min_max_luminance;
        /// The game's UI layers for this frame (sampled; cover the interpolation rect). The first is
        /// taken out before interpolation and put back after; the rest are masked.
        std::array<VkImageView, 4> ui_layers; ///< Null-padded; [0] null: no mask.
        bool show_ui_mask; ///< Testing: writes the UI mask instead.
    };

    explicit Fsr3FrameGenerator(const Device& device, Scheduler& scheduler,
                                MemoryAllocator& memory_allocator,
                                StagingBufferPool& staging_buffer_pool,
                                DescriptorPool& descriptor_pool,
                                ComputePassDescriptorQueue& descriptor_queue);
    ~Fsr3FrameGenerator();

    /// Whether the device has what the passes need.
    [[nodiscard]] static bool IsSupported(const Device& device);

    /// Records the reconstruct-and-dilate pass on this frame's depth and motion vectors.
    void Prepare(const PrepareInputs& inputs);

    /// True when Prepare ran since the last Generate (this frame has game motion).
    [[nodiscard]] bool HasPrepared() const noexcept {
        return prepared;
    }

    /// No frame is generated for this one: the next generation starts over.
    void Skip() noexcept {
        prepared = false;
        has_previous = false;
    }

    /// Records optical flow and frame interpolation between the last frame given and this one,
    /// into the output image.
    void Generate(const GenerateInputs& inputs);

private:
    class Pass;

    struct Texture {
        vk::Image image;
        vk::ImageView view;                ///< Every level.
        std::vector<vk::ImageView> levels; ///< One view per level, for images with mips.
    };

    Texture CreateTexture(VkFormat format, u32 width, u32 height, u32 levels = 1);
    void CreateRenderResources(VkExtent2D render_size);
    void CreateDisplayResources(VkExtent2D display_size, VkFormat format);

    const Device& device;
    Scheduler& scheduler;
    MemoryAllocator& memory_allocator;
    StagingBufferPool& staging_buffer_pool;
    ComputePassDescriptorQueue& descriptor_queue;

    vk::Sampler linear_sampler;
    std::vector<std::unique_ptr<Pass>> passes;

    // Render size (frame interpolation).
    Texture dilated_depth;
    Texture dilated_motion_vectors;
    Texture reconstructed_previous_depth;
    Texture reconstructed_interpolated_depth;
    Texture game_motion_x;
    Texture game_motion_y;
    Texture flow_motion_x;
    Texture flow_motion_y;
    Texture disocclusion_mask;
    // Display size.
    Texture inpainting_pyramid;
    Texture previous_source;
    Texture ui_less; ///< This frame without the game's UI (what frame generation works on).
    std::array<std::array<Texture, 7>, 2> flow_input; ///< Luma pyramids, ping-pong.
    std::array<std::array<Texture, 7>, 2> flow;       ///< Optical flow per level, ping-pong.
    Texture flow_vector;    ///< The optical flow result (display / 8).
    Texture scd_histogram;
    Texture scd_previous_histogram;
    Texture scd_temp;
    Texture scd_output;
    // Fixed.
    Texture distortion_field; ///< 1x1 zero.
    Texture zero_uint;        ///< 1x1 zero, for the unused optical flow confidence.
    vk::Buffer counters;

    VkExtent2D render_extent{};
    VkExtent2D display_extent{};
    VkFormat display_format{VK_FORMAT_UNDEFINED};
    std::array<f32, 2> jitter{};
    std::array<f32, 2> motion_vector_scale{};
    u32 flow_frame_index{};
    u32 flow_parity{};
    bool prepared{};
    bool render_resources_new{true};
    bool display_resources_new{true};
    bool fixed_resources_new{true};
    bool has_previous{};
};

} // namespace Vulkan
