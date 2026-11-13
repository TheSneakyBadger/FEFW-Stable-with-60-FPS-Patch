// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

// AMD FSR 4 through the fsr4-vk FidelityFX API provider, in place of a game's DLSS evaluation.

#pragma once

#include <array>
#include <atomic>

#include "common/common_types.h"
#include "video_core/vulkan_common/vulkan_wrapper.h"

namespace Vulkan {

class Device;
class MemoryAllocator;
class Scheduler;

class Fsr4Upscaler {
public:
    struct Image {
        VkImageView view;
        VkImage image;
        VkFormat format;
        VkImageSubresourceRange range;     ///< The view's level and layer, with the view's aspect.
        VkImageAspectFlags barrier_aspect; ///< Every aspect of the image's format.
    };

    struct Inputs {
        Image color;          ///< Jittered linear colour at the render size.
        Image depth;          ///< Device depth (reversed) at the render size.
        Image motion_vectors; ///< Motion to the previous frame at the render size.
        VkExtent2D render_size;
        VkExtent2D upscale_size;
        /// This frame's sample offset in render pixels, as the game hands its own DLSS.
        std::array<f32, 2> jitter;
        /// Turns a motion vector into render pixels (the render size for UV offsets).
        std::array<f32, 2> motion_vector_scale;
        bool reset;
    };

    /// Throws vk::Exception when the fsr4-vk provider or a Vulkan context is unavailable.
    /// The context accepts render and upscale sizes up to max_render and max_upscale.
    explicit Fsr4Upscaler(const Device& device, Scheduler& scheduler,
                          MemoryAllocator& memory_allocator, VkExtent2D max_render,
                          VkExtent2D max_upscale);
    ~Fsr4Upscaler();

    /// Records one evaluation into the output image (RGBA16F at the upscale size, GENERAL
    /// layout). Returns false when FSR4 can't run this frame.
    bool Upscale(const Inputs& inputs);

    [[nodiscard]] VkImage OutputImage() const noexcept {
        return *output_image;
    }
    [[nodiscard]] VkImageView OutputView() const noexcept {
        return *output_view;
    }
    /// Nearest-neighbour clamp sampler (normalized coordinates), for reading the output.
    [[nodiscard]] VkSampler PointSampler() const noexcept {
        return *point_sampler;
    }

    /// Whether this context can take these sizes (otherwise make a bigger one).
    [[nodiscard]] bool Fits(VkExtent2D render, VkExtent2D upscale) const noexcept {
        return render.width <= max_render_size.width && render.height <= max_render_size.height &&
               upscale.width <= max_upscale_size.width && upscale.height <= max_upscale_size.height;
    }

    /// Whether the fsr4-vk provider library can be loaded. Does not create a Vulkan context.
    [[nodiscard]] static bool IsAvailable();

private:
    void CreateOutput(VkExtent2D upscale_size);

    const Device& device;
    Scheduler& scheduler;
    MemoryAllocator& memory_allocator;
    void* context{};
    VkExtent2D max_render_size{};
    VkExtent2D max_upscale_size{};
    VkExtent2D render_extent{};
    VkExtent2D upscale_extent{};
    vk::Image output_image;
    vk::ImageView output_view;
    vk::Sampler point_sampler;
    std::atomic<bool> failed{};
};

} // namespace Vulkan
