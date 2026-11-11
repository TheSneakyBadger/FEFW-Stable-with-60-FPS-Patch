// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <algorithm>
#include <cfloat>
#include <cmath>
#include <cstring>
#include <numbers>
#include <span>

#include "common/alignment.h"
#include "common/logging.h"
#include "video_core/host_shaders/fsr3_fg_fi_disocclusion_mask_comp_spv.h"
#include "video_core/host_shaders/fsr3_fg_fi_game_motion_vector_field_comp_spv.h"
#include "video_core/host_shaders/fsr3_fg_fi_game_vector_field_inpainting_pyramid_comp_spv.h"
#include "video_core/host_shaders/fsr3_fg_fi_inpainting_comp_spv.h"
#include "video_core/host_shaders/fsr3_fg_fi_inpainting_pyramid_comp_spv.h"
#include "video_core/host_shaders/fsr3_fg_fi_interpolation_comp_spv.h"
#include "video_core/host_shaders/fsr3_fg_fi_optical_flow_vector_field_comp_spv.h"
#include "video_core/host_shaders/fsr3_fg_fi_reconstruct_and_dilate_comp_spv.h"
#include "video_core/host_shaders/fsr3_fg_fi_reconstruct_previous_depth_comp_spv.h"
#include "video_core/host_shaders/fsr3_fg_fi_setup_comp_spv.h"
#include "video_core/host_shaders/fsr3_fg_of_filter_comp_spv.h"
#include "video_core/host_shaders/fsr3_fg_of_luminance_pyramid_comp_spv.h"
#include "video_core/host_shaders/fsr3_fg_of_prepare_luma_comp_spv.h"
#include "video_core/host_shaders/fsr3_fg_of_scale_comp_spv.h"
#include "video_core/host_shaders/fsr3_fg_of_scd_divergence_comp_spv.h"
#include "video_core/host_shaders/fsr3_fg_of_scd_histogram_comp_spv.h"
#include "video_core/host_shaders/fsr3_fg_of_search_comp_spv.h"
#include "video_core/host_shaders/vulkan_fsr3_fg_ui_mask_comp_spv.h"
#include "video_core/renderer_vulkan/vk_compute_pass.h"
#include "video_core/renderer_vulkan/vk_descriptor_pool.h"
#include "video_core/renderer_vulkan/vk_fsr3_frame_generator.h"
#include "video_core/renderer_vulkan/vk_scheduler.h"
#include "video_core/renderer_vulkan/vk_staging_buffer_pool.h"
#include "video_core/vulkan_common/vulkan_device.h"
#include "video_core/vulkan_common/vulkan_memory_allocator.h"

namespace Vulkan {
namespace {

// cbFI in ffx_frameinterpolation_callbacks_glsl.h (std140; the SDK's FrameInterpolationConstants).
struct FiConstants {
    std::array<s32, 2> render_size;
    std::array<s32, 2> display_size;
    std::array<f32, 2> display_size_rcp;
    f32 camera_near;
    f32 camera_far;
    std::array<s32, 2> upscaler_target_size;
    s32 mode;
    s32 reset;
    std::array<f32, 4> device_to_view_depth;
    f32 delta_time;
    s32 hudless_attached;
    std::array<s32, 2> distortion_field_size;
    std::array<f32, 2> optical_flow_scale;
    s32 optical_flow_block_size;
    u32 dispatch_flags;
    std::array<s32, 2> max_render_size;
    s32 optical_flow_half_res_mode;
    s32 num_instances;
    std::array<s32, 2> interpolation_rect_base;
    std::array<s32, 2> interpolation_rect_size;
    std::array<f32, 3> debug_bar_color;
    u32 back_buffer_transfer_function;
    std::array<f32, 2> min_max_luminance;
    f32 tan_half_fov;
    s32 pad;
    std::array<f32, 2> jitter;
    std::array<f32, 2> motion_vector_scale;
};
static_assert(sizeof(FiConstants) == 176);

// cbInpaintingPyramid.
struct PyramidConstants {
    u32 mips;
    u32 num_work_groups;
    std::array<u32, 2> work_group_offset;
};

// cbOF and cbOF_SPD (ffx_opticalflow_callbacks_glsl.h).
struct OfConstants {
    std::array<s32, 2> input_luma_resolution;
    u32 pyramid_level;
    u32 pyramid_level_count;
    u32 frame_index;
    u32 transfer_function;
    std::array<f32, 2> min_max_luminance;
};

struct OfSpdConstants {
    u32 mips;
    u32 num_work_groups;
    std::array<u32, 2> work_group_offset;
    u32 num_work_groups_input_pyramid;
    std::array<u32, 3> pad;
};

// Descriptors, as the pass files declare them (read with a SPIR-V reflection of the compiled
// passes). The binding numbers have gaps, so each entry names its own.
enum class Kind : u8 { Texture, Image, Uniform, Storage, Sampler };
struct Binding {
    u32 binding;
    Kind kind;
};
constexpr Kind T = Kind::Texture;
constexpr Kind I = Kind::Image;
constexpr Kind U = Kind::Uniform;
constexpr Kind B = Kind::Storage;

template <size_t N>
constexpr std::array<Binding, N> Sequential(const std::array<Kind, N>& kinds) {
    std::array<Binding, N> result{};
    for (u32 i = 0; i < N; ++i) {
        result[i] = {i, kinds[i]};
    }
    return result;
}

// Frame interpolation (all with the linear clamp sampler at 1000).
constexpr auto RECONSTRUCT_AND_DILATE = Sequential(std::array{T, T, I, I, I, U});
constexpr auto SETUP = Sequential(std::array{T, I, I, I, I, I, B, U});
constexpr auto RECONSTRUCT_PREVIOUS_DEPTH = Sequential(std::array{T, T, T, T, I, U});
constexpr auto GAME_MOTION_VECTOR_FIELD = Sequential(std::array{T, T, T, T, T, I, I, U});
constexpr auto GAME_VECTOR_FIELD_PYRAMID =
    Sequential(std::array{T, T, B, I, I, I, I, I, I, I, I, I, I, I, I, I, U, U});
constexpr std::array<Binding, 8> OPTICAL_FLOW_VECTOR_FIELD{
    {{0, T}, {1, T}, {2, T}, {3, T}, {4, T}, {6, I}, {7, I}, {8, U}}};
constexpr auto DISOCCLUSION_MASK = Sequential(std::array{T, T, T, T, T, T, T, I, U});
constexpr auto INTERPOLATION = Sequential(std::array{T, T, T, T, T, T, T, T, B, I, U});
constexpr auto INPAINTING_PYRAMID =
    Sequential(std::array{T, B, I, I, I, I, I, I, I, I, I, I, I, I, I, U, U});
constexpr std::array<Binding, 6> INPAINTING{{{0, T}, {1, T}, {2, T}, {3, T}, {7, I}, {8, U}}};
// Optical flow (no samplers).
constexpr auto PREPARE_LUMA = Sequential(std::array{T, I, U});
constexpr auto LUMINANCE_PYRAMID = Sequential(std::array{I, I, I, I, I, I, I, U, U});
constexpr auto SCD_HISTOGRAM = Sequential(std::array{T, I, U});
constexpr auto SCD_DIVERGENCE = Sequential(std::array{I, I, I, I, U});
constexpr auto SEARCH = Sequential(std::array{T, T, I, I, U});
constexpr auto FILTER = Sequential(std::array{T, I, U});
constexpr auto SCALE = Sequential(std::array{T, T, T, I, I, U});
// The UI mask (vulkan_fsr3_fg_ui_mask.comp; linear clamp sampler at 1000).
constexpr auto UI_MASK = Sequential(std::array{T, T, T, T, T, I, U, I});

struct UiMaskConstants {
    std::array<s32, 2> rect_base;
    std::array<s32, 2> rect_size;
    f32 threshold;
    u32 show_mask;
    u32 layer_count;
    /// 0: take the UI out of the real frame; 1: put it back over the generated one; 2: mask it.
    u32 mode;
};

enum PassId : size_t {
    PassReconstructAndDilate,
    PassSetup,
    PassReconstructPreviousDepth,
    PassGameMotionVectorField,
    PassGameVectorFieldPyramid,
    PassOpticalFlowVectorField,
    PassDisocclusionMask,
    PassInterpolation,
    PassInpaintingPyramid,
    PassInpainting,
    PassPrepareLuma,
    PassLuminancePyramid,
    PassScdHistogram,
    PassScdDivergence,
    PassSearch,
    PassFilter,
    PassScale,
    PassUiMask,
    PassCount,
};

constexpr u32 FLOW_LEVELS = 7;
constexpr u32 FLOW_BLOCK_SIZE = 8;
constexpr u32 SCD_HISTOGRAM_WIDTH = 256 * 3 * 3;
constexpr u32 PYRAMID_BINDINGS = 13;

/// One descriptor of a dispatch: an image view, or a buffer range.
struct Resource {
    VkImageView view{};
    VkBuffer buffer{};
    VkDeviceSize offset{};
    VkDeviceSize size{};
};

constexpr VkMemoryBarrier MemoryBarrier(VkAccessFlags src, VkAccessFlags dst) {
    return VkMemoryBarrier{
        .sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER,
        .pNext = nullptr,
        .srcAccessMask = src,
        .dstAccessMask = dst,
    };
}

constexpr VkImageSubresourceRange ALL_LEVELS{
    .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
    .baseMipLevel = 0,
    .levelCount = VK_REMAINING_MIP_LEVELS,
    .baseArrayLayer = 0,
    .layerCount = 1,
};

// ffxSpdSetup over a whole width x height area: thread groups and the work group count.
std::array<u32, 3> SpdSetup(u32 width, u32 height) {
    const u32 groups_x = (width - 1) / 64 + 1;
    const u32 groups_y = (height - 1) / 64 + 1;
    return {groups_x, groups_y, groups_x * groups_y};
}

u32 SpdMips(u32 width, u32 height) {
    return static_cast<u32>(
        (std::min)(std::floor(std::log2(static_cast<f32>((std::max)(width, height)))), 12.0f));
}

// No camera comes with DLSS: as in the upscaler port, a typical near plane and vertical field
// of view, reversed infinite depth.
constexpr f32 CAMERA_NEAR = 0.1f;
constexpr f32 CAMERA_FOV = 60.0f * std::numbers::pi_v<f32> / 180.0f;

} // Anonymous namespace

class Fsr3FrameGenerator::Pass final : public ComputePass {
public:
    explicit Pass(const Device& device_, Scheduler& scheduler, DescriptorPool& descriptor_pool,
                  std::span<const Binding> bindings, std::span<const u32> code,
                  VkSampler sampler)
        : Pass(device_, scheduler, descriptor_pool, MakeLayout(bindings, sampler), code) {}

    void Dispatch(Scheduler& scheduler_, ComputePassDescriptorQueue& queue,
                  std::span<const Resource> resources, u32 groups_x, u32 groups_y,
                  u32 groups_z = 1) {
        queue.Acquire(scheduler_, resources.size());
        for (const Resource& resource : resources) {
            if (resource.buffer) {
                queue.AddBuffer(resource.buffer, resource.offset, resource.size);
            } else {
                queue.AddImage(resource.view);
            }
        }
        const void* const descriptor_data{queue.UpdateData()};
        scheduler_.Record(
            [this, descriptor_data, groups_x, groups_y, groups_z](vk::CommandBuffer cmdbuf) {
                const VkDescriptorSet set = descriptor_allocator.Commit();
                device.GetLogical().UpdateDescriptorSet(set, *descriptor_template,
                                                        descriptor_data);
                cmdbuf.BindPipeline(VK_PIPELINE_BIND_POINT_COMPUTE, *pipeline);
                cmdbuf.BindDescriptorSets(VK_PIPELINE_BIND_POINT_COMPUTE, *layout, 0, set, {});
                cmdbuf.Dispatch(groups_x, groups_y, groups_z);
                // The next pass reads what this one wrote (everything stays in GENERAL).
                cmdbuf.PipelineBarrier(
                    VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                    VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_TRANSFER_BIT, 0,
                    MemoryBarrier(VK_ACCESS_SHADER_WRITE_BIT,
                                  VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT |
                                      VK_ACCESS_TRANSFER_READ_BIT | VK_ACCESS_TRANSFER_WRITE_BIT));
            });
    }

private:
    struct Layout {
        std::vector<VkDescriptorSetLayoutBinding> bindings;
        std::vector<VkDescriptorUpdateTemplateEntry> templates;
        DescriptorBankInfo bank;
    };

    explicit Pass(const Device& device_, Scheduler& scheduler, DescriptorPool& descriptor_pool,
                  const Layout& pass_layout, std::span<const u32> code)
        : ComputePass(device_, scheduler, descriptor_pool, pass_layout.bindings,
                      pass_layout.templates, pass_layout.bank, {}, code) {}

    static Layout MakeLayout(std::span<const Binding> bindings, VkSampler sampler) {
        Layout result;
        for (u32 index = 0; index < bindings.size(); ++index) {
            const Binding& binding = bindings[index];
            VkDescriptorType type{};
            switch (binding.kind) {
            case Kind::Texture:
                type = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
                ++result.bank.sampled_images;
                break;
            case Kind::Image:
                type = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
                ++result.bank.images;
                break;
            case Kind::Uniform:
                type = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
                ++result.bank.uniform_buffers;
                break;
            case Kind::Storage:
                type = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
                ++result.bank.storage_buffers;
                break;
            case Kind::Sampler:
                break;
            }
            result.bindings.push_back({
                .binding = binding.binding,
                .descriptorType = type,
                .descriptorCount = 1,
                .stageFlags = VK_SHADER_STAGE_COMPUTE_BIT,
                .pImmutableSamplers = nullptr,
            });
            result.templates.push_back({
                .dstBinding = binding.binding,
                .dstArrayElement = 0,
                .descriptorCount = 1,
                .descriptorType = type,
                .offset = index * sizeof(DescriptorUpdateEntry),
                .stride = sizeof(DescriptorUpdateEntry),
            });
        }
        if (sampler) {
            result.bindings.push_back({
                .binding = 1000,
                .descriptorType = VK_DESCRIPTOR_TYPE_SAMPLER,
                .descriptorCount = 1,
                .stageFlags = VK_SHADER_STAGE_COMPUTE_BIT,
                .pImmutableSamplers = &StoredSampler(sampler),
            });
            ++result.bank.samplers;
        }
        result.bank.score = static_cast<s32>(result.bindings.size());
        return result;
    }

    // The immutable sampler, where the layout's pointer to it stays valid while the pass is
    // created (every pass uses the same one).
    static const VkSampler& StoredSampler(VkSampler sampler) {
        static VkSampler stored{};
        stored = sampler;
        return stored;
    }
};

Fsr3FrameGenerator::Fsr3FrameGenerator(const Device& device_, Scheduler& scheduler_,
                                       MemoryAllocator& memory_allocator_,
                                       StagingBufferPool& staging_buffer_pool_,
                                       DescriptorPool& descriptor_pool,
                                       ComputePassDescriptorQueue& descriptor_queue_)
    : device{device_}, scheduler{scheduler_}, memory_allocator{memory_allocator_},
      staging_buffer_pool{staging_buffer_pool_}, descriptor_queue{descriptor_queue_} {
    linear_sampler = device.GetLogical().CreateSampler(VkSamplerCreateInfo{
        .sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO,
        .pNext = nullptr,
        .flags = 0,
        .magFilter = VK_FILTER_LINEAR,
        .minFilter = VK_FILTER_LINEAR,
        .mipmapMode = VK_SAMPLER_MIPMAP_MODE_LINEAR,
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
    const VkSampler sampler = *linear_sampler;
    const auto pass = [&](std::span<const Binding> bindings, std::span<const u32> code,
                          bool with_sampler) {
        return std::make_unique<Pass>(device, scheduler, descriptor_pool, bindings, code,
                                      with_sampler ? sampler : VkSampler{});
    };
    passes.resize(PassCount);
    passes[PassReconstructAndDilate] =
        pass(RECONSTRUCT_AND_DILATE, FSR3_FG_FI_RECONSTRUCT_AND_DILATE_COMP_SPV, true);
    passes[PassSetup] = pass(SETUP, FSR3_FG_FI_SETUP_COMP_SPV, true);
    passes[PassReconstructPreviousDepth] =
        pass(RECONSTRUCT_PREVIOUS_DEPTH, FSR3_FG_FI_RECONSTRUCT_PREVIOUS_DEPTH_COMP_SPV, true);
    passes[PassGameMotionVectorField] =
        pass(GAME_MOTION_VECTOR_FIELD, FSR3_FG_FI_GAME_MOTION_VECTOR_FIELD_COMP_SPV, true);
    passes[PassGameVectorFieldPyramid] = pass(
        GAME_VECTOR_FIELD_PYRAMID, FSR3_FG_FI_GAME_VECTOR_FIELD_INPAINTING_PYRAMID_COMP_SPV, true);
    passes[PassOpticalFlowVectorField] =
        pass(OPTICAL_FLOW_VECTOR_FIELD, FSR3_FG_FI_OPTICAL_FLOW_VECTOR_FIELD_COMP_SPV, true);
    passes[PassDisocclusionMask] =
        pass(DISOCCLUSION_MASK, FSR3_FG_FI_DISOCCLUSION_MASK_COMP_SPV, true);
    passes[PassInterpolation] = pass(INTERPOLATION, FSR3_FG_FI_INTERPOLATION_COMP_SPV, true);
    passes[PassInpaintingPyramid] =
        pass(INPAINTING_PYRAMID, FSR3_FG_FI_INPAINTING_PYRAMID_COMP_SPV, true);
    passes[PassInpainting] = pass(INPAINTING, FSR3_FG_FI_INPAINTING_COMP_SPV, true);
    passes[PassPrepareLuma] = pass(PREPARE_LUMA, FSR3_FG_OF_PREPARE_LUMA_COMP_SPV, false);
    passes[PassLuminancePyramid] =
        pass(LUMINANCE_PYRAMID, FSR3_FG_OF_LUMINANCE_PYRAMID_COMP_SPV, false);
    passes[PassScdHistogram] = pass(SCD_HISTOGRAM, FSR3_FG_OF_SCD_HISTOGRAM_COMP_SPV, false);
    passes[PassScdDivergence] = pass(SCD_DIVERGENCE, FSR3_FG_OF_SCD_DIVERGENCE_COMP_SPV, false);
    passes[PassSearch] = pass(SEARCH, FSR3_FG_OF_SEARCH_COMP_SPV, false);
    passes[PassFilter] = pass(FILTER, FSR3_FG_OF_FILTER_COMP_SPV, false);
    passes[PassScale] = pass(SCALE, FSR3_FG_OF_SCALE_COMP_SPV, false);
    passes[PassUiMask] = pass(UI_MASK, VULKAN_FSR3_FG_UI_MASK_COMP_SPV, true);

    distortion_field = CreateTexture(VK_FORMAT_R8G8_UNORM, 1, 1);
    no_ui = CreateTexture(VK_FORMAT_R8G8B8A8_UNORM, 1, 1);
    zero_uint = CreateTexture(VK_FORMAT_R32_UINT, 1, 1);
    counters = memory_allocator.CreateBuffer(
        VkBufferCreateInfo{
            .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
            .pNext = nullptr,
            .flags = 0,
            .size = 16,
            .usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT,
            .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
            .queueFamilyIndexCount = 0,
            .pQueueFamilyIndices = nullptr,
        },
        MemoryUsage::DeviceLocal);
    LOG_INFO(Render_Vulkan, "AMD FSR 3.1 frame generation ready");
}

Fsr3FrameGenerator::~Fsr3FrameGenerator() = default;

bool Fsr3FrameGenerator::IsSupported(const Device& device) {
    if (!device.IsStorageImageExtendedFormatsSupported() || !device.IsFormatlessImageLoadSupported()) {
        return false;
    }
    // The pyramids reduce with quad operations, the optical flow search with subgroup arithmetic.
    if ((device.GetSubgroupSupportedStages() & VK_SHADER_STAGE_COMPUTE_BIT) == 0 ||
        !device.IsSubgroupFeatureSupported(VK_SUBGROUP_FEATURE_QUAD_BIT) ||
        !device.IsSubgroupFeatureSupported(VK_SUBGROUP_FEATURE_ARITHMETIC_BIT) ||
        !device.IsSubgroupFeatureSupported(VK_SUBGROUP_FEATURE_BASIC_BIT)) {
        return false;
    }
    constexpr VkFormatFeatureFlags storage =
        VK_FORMAT_FEATURE_STORAGE_IMAGE_BIT | VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT;
    for (const VkFormat format :
         {VK_FORMAT_R8_UINT, VK_FORMAT_R8G8_UNORM, VK_FORMAT_R16G16_SINT, VK_FORMAT_R16G16_SFLOAT,
          VK_FORMAT_R32_UINT, VK_FORMAT_R32_SFLOAT, VK_FORMAT_R16G16B16A16_SFLOAT}) {
        if (!device.IsFormatSupported(format, storage, FormatType::Optimal)) {
            return false;
        }
    }
    return true;
}

Fsr3FrameGenerator::Texture Fsr3FrameGenerator::CreateTexture(VkFormat format, u32 width,
                                                              u32 height, u32 levels) {
    Texture texture;
    texture.image = memory_allocator.CreateImage(VkImageCreateInfo{
        .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
        .pNext = nullptr,
        .flags = 0,
        .imageType = VK_IMAGE_TYPE_2D,
        .format = format,
        .extent = {(std::max)(width, 1U), (std::max)(height, 1U), 1},
        .mipLevels = levels,
        .arrayLayers = 1,
        .samples = VK_SAMPLE_COUNT_1_BIT,
        .tiling = VK_IMAGE_TILING_OPTIMAL,
        .usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_STORAGE_BIT |
                 VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT,
        .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
        .queueFamilyIndexCount = 0,
        .pQueueFamilyIndices = nullptr,
        .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
    });
    const auto view = [&](u32 base_level, u32 level_count) {
        return device.GetLogical().CreateImageView(VkImageViewCreateInfo{
            .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
            .pNext = nullptr,
            .flags = 0,
            .image = *texture.image,
            .viewType = VK_IMAGE_VIEW_TYPE_2D,
            .format = format,
            .components{},
            .subresourceRange{
                .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
                .baseMipLevel = base_level,
                .levelCount = level_count,
                .baseArrayLayer = 0,
                .layerCount = 1,
            },
        });
    };
    texture.view = view(0, levels);
    if (levels > 1) {
        for (u32 level = 0; level < levels; ++level) {
            texture.levels.push_back(view(level, 1));
        }
    }
    return texture;
}

void Fsr3FrameGenerator::CreateRenderResources(VkExtent2D render_size) {
    const u32 w = render_size.width;
    const u32 h = render_size.height;
    for (PreparedFrame& frame : prepared_frames) {
        frame.dilated_depth = CreateTexture(VK_FORMAT_R32_SFLOAT, w, h);
        frame.dilated_motion_vectors = CreateTexture(VK_FORMAT_R16G16_SFLOAT, w, h);
        frame.reconstructed_previous_depth = CreateTexture(VK_FORMAT_R32_UINT, w, h);
        frame.ready = false;
    }
    reconstructed_interpolated_depth = CreateTexture(VK_FORMAT_R32_UINT, w, h);
    game_motion_x = CreateTexture(VK_FORMAT_R32_UINT, w, h);
    game_motion_y = CreateTexture(VK_FORMAT_R32_UINT, w, h);
    flow_motion_x = CreateTexture(VK_FORMAT_R32_UINT, w, h);
    flow_motion_y = CreateTexture(VK_FORMAT_R32_UINT, w, h);
    disocclusion_mask = CreateTexture(VK_FORMAT_R8G8_UNORM, w, h);
    render_extent = render_size;
    render_resources_new = true;
}

void Fsr3FrameGenerator::CreateDisplayResources(VkExtent2D display_size, VkFormat format) {
    const u32 w = display_size.width;
    const u32 h = display_size.height;
    const u32 pyramid_levels =
        static_cast<u32>(std::floor(std::log2(static_cast<f32>((std::max)(w / 2, h / 2))))) + 1;
    inpainting_pyramid = CreateTexture(VK_FORMAT_R16G16B16A16_SFLOAT, w / 2, h / 2,
                                       (std::min)(pyramid_levels, PYRAMID_BINDINGS));
    previous_source = CreateTexture(format, w, h);
    for (Texture& texture : ui_less) {
        texture = CreateTexture(format, w, h);
    }
    for (Texture& texture : ui_alpha) {
        // 16-bit float: Streamline doesn't know R8_UNORM ("Cannot have undefined format").
        texture = CreateTexture(VK_FORMAT_R16_SFLOAT, w, h);
    }
    std::array<VkExtent2D, FLOW_LEVELS> flow_sizes{};
    flow_sizes[0] = {(w + FLOW_BLOCK_SIZE - 1) / FLOW_BLOCK_SIZE,
                     (h + FLOW_BLOCK_SIZE - 1) / FLOW_BLOCK_SIZE};
    for (u32 level = 1; level < FLOW_LEVELS; ++level) {
        flow_sizes[level] = {(flow_sizes[level - 1].width + 1) / 2,
                             (flow_sizes[level - 1].height + 1) / 2};
    }
    for (u32 set = 0; set < 2; ++set) {
        for (u32 level = 0; level < FLOW_LEVELS; ++level) {
            flow_input[set][level] = CreateTexture(VK_FORMAT_R8_UINT, w >> level, h >> level);
            flow[set][level] = CreateTexture(VK_FORMAT_R16G16_SINT, flow_sizes[level].width,
                                             flow_sizes[level].height);
        }
    }
    flow_vector = CreateTexture(VK_FORMAT_R16G16_SINT, flow_sizes[0].width, flow_sizes[0].height);
    scd_histogram = CreateTexture(VK_FORMAT_R32_UINT, SCD_HISTOGRAM_WIDTH, 1);
    scd_previous_histogram = CreateTexture(VK_FORMAT_R32_SFLOAT, SCD_HISTOGRAM_WIDTH, 1);
    scd_temp = CreateTexture(VK_FORMAT_R32_UINT, 3, 1);
    scd_output = CreateTexture(VK_FORMAT_R32_UINT, 3, 1);
    display_extent = display_size;
    display_format = format;
    display_resources_new = true;
    has_previous = false;
    LOG_INFO(Render_Vulkan, "AMD FSR 3.1 frame generation: {}x{}", w, h);
}

namespace {

// Records a clear of every image given (UNDEFINED to GENERAL first where `initialize`).
void RecordClears(Scheduler& scheduler, std::vector<VkImage> initialize,
                  std::vector<VkImage> clear_zero, VkBuffer counters) {
    scheduler.Record([initialize = std::move(initialize), clear_zero = std::move(clear_zero),
                      counters](vk::CommandBuffer cmdbuf) {
        cmdbuf.PipelineBarrier(VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
                               VK_PIPELINE_STAGE_TRANSFER_BIT | VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                               0,
                               MemoryBarrier(VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT |
                                                 VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT |
                                                 VK_ACCESS_SHADER_WRITE_BIT |
                                                 VK_ACCESS_TRANSFER_WRITE_BIT,
                                             VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT |
                                                 VK_ACCESS_TRANSFER_WRITE_BIT));
        if (!initialize.empty()) {
            std::vector<VkImageMemoryBarrier> barriers;
            for (const VkImage image : initialize) {
                barriers.push_back(VkImageMemoryBarrier{
                    .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
                    .pNext = nullptr,
                    .srcAccessMask = 0,
                    .dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,
                    .oldLayout = VK_IMAGE_LAYOUT_UNDEFINED,
                    .newLayout = VK_IMAGE_LAYOUT_GENERAL,
                    .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                    .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                    .image = image,
                    .subresourceRange = ALL_LEVELS,
                });
            }
            cmdbuf.PipelineBarrier(VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                                   VK_PIPELINE_STAGE_TRANSFER_BIT, 0, {}, {}, barriers);
        }
        for (const VkImage image : clear_zero) {
            cmdbuf.ClearColorImage(image, VK_IMAGE_LAYOUT_GENERAL, VkClearColorValue{},
                                   ALL_LEVELS);
        }
        if (counters) {
            cmdbuf.FillBuffer(counters, 0, VK_WHOLE_SIZE, 0);
        }
        cmdbuf.PipelineBarrier(VK_PIPELINE_STAGE_TRANSFER_BIT,
                               VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0,
                               MemoryBarrier(VK_ACCESS_TRANSFER_WRITE_BIT,
                                             VK_ACCESS_SHADER_READ_BIT |
                                                 VK_ACCESS_SHADER_WRITE_BIT));
    });
}

FiConstants MakeFiConstants(VkExtent2D render, VkExtent2D display, VkRect2D rect,
                            std::array<f32, 2> jitter, std::array<f32, 2> mv_scale) {
    FiConstants c{};
    const s32 rw = static_cast<s32>(render.width);
    const s32 rh = static_cast<s32>(render.height);
    const s32 dw = static_cast<s32>(display.width);
    const s32 dh = static_cast<s32>(display.height);
    c.render_size = {rw, rh};
    c.display_size = {dw, dh};
    c.display_size_rcp = {1.0f / static_cast<f32>((std::max)(dw, 1)),
                          1.0f / static_cast<f32>((std::max)(dh, 1))};
    c.camera_near = CAMERA_NEAR;
    c.camera_far = 1.0e6f;
    c.upscaler_target_size = {static_cast<s32>(rect.extent.width),
                              static_cast<s32>(rect.extent.height)};
    // setupDeviceDepthToViewSpaceDepthParams, reversed and infinite.
    const f32 aspect = static_cast<f32>(rw) / static_cast<f32>((std::max)(rh, 1));
    const f32 cot_half_fov = std::cos(0.5f * CAMERA_FOV) / std::sin(0.5f * CAMERA_FOV);
    c.device_to_view_depth = {-1.0f * (0.0f + FLT_EPSILON), CAMERA_NEAR,
                              1.0f / (cot_half_fov / aspect), 1.0f / cot_half_fov};
    c.distortion_field_size = {1, 1};
    c.optical_flow_scale = {1.0f / static_cast<f32>((std::max)(dw, 1)),
                            1.0f / static_cast<f32>((std::max)(dh, 1))};
    c.optical_flow_block_size = static_cast<s32>(FLOW_BLOCK_SIZE);
    c.max_render_size = {rw, rh};
    c.interpolation_rect_base = {rect.offset.x, rect.offset.y};
    c.interpolation_rect_size = {static_cast<s32>(rect.extent.width),
                                 static_cast<s32>(rect.extent.height)};
    const f32 horizontal = std::atan(std::tan(CAMERA_FOV / 2.0f) * aspect) * 2.0f;
    c.tan_half_fov = std::tan(horizontal * 0.5f);
    c.jitter = jitter;
    c.motion_vector_scale = mv_scale;
    return c;
}

} // Anonymous namespace

void Fsr3FrameGenerator::Prepare(const PrepareInputs& in, u64 seq) {
    if (!prepared_frames[0].dilated_depth.image || in.render_size.width != render_extent.width ||
        in.render_size.height != render_extent.height) {
        if (prepared_frames[0].dilated_depth.image) {
            scheduler.Finish(); // the old images may still be in use
        }
        CreateRenderResources(in.render_size);
    }
    PreparedFrame& frame = prepared_frames[seq % KEPT_FRAMES];
    const Texture& dilated_depth = frame.dilated_depth;
    const Texture& dilated_motion_vectors = frame.dilated_motion_vectors;
    const Texture& reconstructed_previous_depth = frame.reconstructed_previous_depth;
    frame.jitter = in.jitter;
    frame.motion_vector_scale = in.motion_vector_scale;
    const VkExtent2D display = display_extent.width ? display_extent : in.render_size;
    const VkRect2D rect{.offset = {0, 0}, .extent = display};
    const FiConstants constants =
        MakeFiConstants(render_extent, display, rect, frame.jitter, frame.motion_vector_scale);

    const VkDeviceSize alignment =
        (std::max<VkDeviceSize>)(device.GetUniformBufferAlignment(), 16);
    const VkDeviceSize slot = Common::AlignUp(VkDeviceSize{sizeof(FiConstants)}, alignment);
    const StagingBufferRef uniforms = staging_buffer_pool.Request(slot, MemoryUsage::Upload);
    std::memcpy(uniforms.mapped_span.data(), &constants, sizeof(constants));
    const Resource cb{.buffer = uniforms.buffer, .offset = uniforms.offset, .size = slot};

    std::vector<VkImage> initialize;
    std::vector<VkImage> clear_zero{*reconstructed_previous_depth.image};
    if (render_resources_new) {
        for (const PreparedFrame& kept : prepared_frames) {
            initialize.push_back(*kept.dilated_depth.image);
            initialize.push_back(*kept.dilated_motion_vectors.image);
            initialize.push_back(*kept.reconstructed_previous_depth.image);
        }
        for (const Texture* texture :
             {&reconstructed_interpolated_depth, &game_motion_x, &game_motion_y, &flow_motion_x,
              &flow_motion_y, &disocclusion_mask}) {
            initialize.push_back(*texture->image);
        }
        clear_zero = initialize;
        render_resources_new = false;
    }
    scheduler.RequestOutsideRenderPassOperationContext();
    RecordClears(scheduler, std::move(initialize), std::move(clear_zero), VkBuffer{});

    const auto tex = [](const Texture& texture) { return Resource{.view = *texture.view}; };
    passes[PassReconstructAndDilate]->Dispatch(
        scheduler, descriptor_queue,
        std::array{Resource{.view = in.motion_vectors}, Resource{.view = in.depth},
                   tex(reconstructed_previous_depth), tex(dilated_motion_vectors),
                   tex(dilated_depth), cb},
        (render_extent.width + 7) / 8, (render_extent.height + 7) / 8);
    frame.seq = seq;
    frame.ready = true;
}

void Fsr3FrameGenerator::StashUi(VkImage image, VkFormat format, VkExtent2D size, u64 seq) {
    UiCopy& copy_slot = ui_copies[seq % KEPT_FRAMES];
    copy_slot.seq = 0;
    if (!image || size.width == 0 || size.height == 0) {
        return;
    }
    // Retired copies go once the GPU is done with them (no wait).
    std::erase_if(retired_ui_copies, [&](const RetiredUiCopy& retired) {
        return scheduler.IsFree(retired.tick);
    });
    if (!copy_slot.image || format != copy_slot.format || size.width != copy_slot.extent.width ||
        size.height != copy_slot.extent.height) {
        if (copy_slot.image) {
            // The game's UI layer changed format or size (it may alternate between layers):
            // waiting for the GPU here stalled every such frame.
            retired_ui_copies.push_back({scheduler.CurrentTick(), std::move(copy_slot.image),
                                         std::move(copy_slot.view)});
            ++ui_copies_recreated;
        }
        copy_slot.image = memory_allocator.CreateImage(VkImageCreateInfo{
            .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
            .pNext = nullptr,
            .flags = 0,
            .imageType = VK_IMAGE_TYPE_2D,
            .format = format,
            .extent = {size.width, size.height, 1},
            .mipLevels = 1,
            .arrayLayers = 1,
            .samples = VK_SAMPLE_COUNT_1_BIT,
            .tiling = VK_IMAGE_TILING_OPTIMAL,
            .usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT,
            .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
            .queueFamilyIndexCount = 0,
            .pQueueFamilyIndices = nullptr,
            .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
        });
        copy_slot.view = device.GetLogical().CreateImageView(VkImageViewCreateInfo{
            .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
            .pNext = nullptr,
            .flags = 0,
            .image = *copy_slot.image,
            .viewType = VK_IMAGE_VIEW_TYPE_2D,
            .format = format,
            .components{},
            .subresourceRange = ALL_LEVELS,
        });
        copy_slot.format = format;
        copy_slot.extent = size;
        copy_slot.fresh = true;
    }
    scheduler.RequestOutsideRenderPassOperationContext();
    scheduler.Record([source = image, destination = *copy_slot.image, size,
                      fresh = std::exchange(copy_slot.fresh, false)](vk::CommandBuffer cmdbuf) {
        const VkImageMemoryBarrier to_general{
            .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
            .pNext = nullptr,
            .srcAccessMask = 0,
            .dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,
            .oldLayout = VK_IMAGE_LAYOUT_UNDEFINED,
            .newLayout = VK_IMAGE_LAYOUT_GENERAL,
            .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .image = destination,
            .subresourceRange = ALL_LEVELS,
        };
        // The UI draws (and any earlier read of the stash) before the copy.
        cmdbuf.PipelineBarrier(VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
                               0,
                               MemoryBarrier(VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT |
                                                 VK_ACCESS_SHADER_WRITE_BIT |
                                                 VK_ACCESS_TRANSFER_WRITE_BIT,
                                             VK_ACCESS_TRANSFER_READ_BIT |
                                                 VK_ACCESS_TRANSFER_WRITE_BIT));
        if (fresh) {
            cmdbuf.PipelineBarrier(VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                                   VK_PIPELINE_STAGE_TRANSFER_BIT, 0, {}, {}, to_general);
        }
        const VkImageCopy copy{
            .srcSubresource{VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1},
            .srcOffset{0, 0, 0},
            .dstSubresource{VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1},
            .dstOffset{0, 0, 0},
            .extent{size.width, size.height, 1},
        };
        cmdbuf.CopyImage(source, VK_IMAGE_LAYOUT_GENERAL, destination, VK_IMAGE_LAYOUT_GENERAL,
                         copy);
        cmdbuf.PipelineBarrier(VK_PIPELINE_STAGE_TRANSFER_BIT,
                               VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0,
                               MemoryBarrier(VK_ACCESS_TRANSFER_WRITE_BIT,
                                             VK_ACCESS_SHADER_READ_BIT |
                                                 VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT |
                                                 VK_ACCESS_TRANSFER_READ_BIT));
    });
    copy_slot.seq = seq;
}

void Fsr3FrameGenerator::Generate(const GenerateInputs& in) {
    if (!previous_source.image || in.display_size.width != display_extent.width ||
        in.display_size.height != display_extent.height || in.format != display_format) {
        if (previous_source.image) {
            scheduler.Finish();
        }
        CreateDisplayResources(in.display_size, in.format);
    }
    const bool frame_ready = HasPrepared(in.seq);
    const bool reset = in.reset || !has_previous || !frame_ready ||
                       !prepared_frames[0].dilated_depth.image;
    if (!prepared_frames[0].dilated_depth.image) {
        // No game motion yet: interpolation needs the render-size images all the same.
        CreateRenderResources({16, 16});
    }
    PreparedFrame& frame = prepared_frames[in.seq % KEPT_FRAMES];
    const Texture& dilated_depth = frame.dilated_depth;
    const Texture& dilated_motion_vectors = frame.dilated_motion_vectors;
    const Texture& reconstructed_previous_depth = frame.reconstructed_previous_depth;
    // This frame is used up, and so is any older one still kept.
    for (PreparedFrame& kept : prepared_frames) {
        if (kept.seq <= in.seq) {
            kept.ready = false;
        }
    }
    // The UI: this frame's layers, or the copy kept when it was presented late.
    std::array<VkImageView, 4> ui_layers = in.late ? std::array<VkImageView, 4>{} : in.ui_layers;
    if (in.ui_layers[0] || in.late) {
        for (const UiCopy& kept : ui_copies) {
            if (kept.seq == in.seq && in.seq != 0) {
                ui_layers = {*kept.view};
            }
        }
    }
    const u32 dw = display_extent.width;
    const u32 dh = display_extent.height;
    const u32 rw = render_extent.width;
    const u32 rh = render_extent.height;

    // Constants: frame interpolation, the two pyramids, optical flow (common, one per level,
    // and its SPD).
    FiConstants fi = MakeFiConstants(render_extent, display_extent, in.interpolation_rect,
                                     frame.jitter, frame.motion_vector_scale);
    fi.reset = reset ? 1 : 0;
    fi.delta_time = std::clamp(in.frame_time_ms, 0.0f, 1000.0f);
    fi.back_buffer_transfer_function = in.transfer_function;
    fi.min_max_luminance = in.min_max_luminance;

    const auto game_spd = SpdSetup(rw, rh);
    const PyramidConstants game_pyramid{
        .mips = SpdMips(rw, rh),
        .num_work_groups = game_spd[2],
        .work_group_offset = {0, 0},
    };
    const auto display_spd = SpdSetup(dw, dh);
    const PyramidConstants display_pyramid{
        .mips = SpdMips(dw, dh),
        .num_work_groups = display_spd[2],
        .work_group_offset = {0, 0},
    };
    const bool flow_reset = reset;
    flow_frame_index = flow_reset ? 0 : flow_frame_index + 1;
    std::array<OfConstants, FLOW_LEVELS + 1> of{};
    for (u32 i = 0; i <= FLOW_LEVELS; ++i) {
        of[i] = OfConstants{
            .input_luma_resolution = {static_cast<s32>(dw), static_cast<s32>(dh)},
            .pyramid_level = i < FLOW_LEVELS ? i : 0,
            .pyramid_level_count = FLOW_LEVELS,
            .frame_index = flow_frame_index,
            .transfer_function = in.transfer_function,
            .min_max_luminance = in.min_max_luminance,
        };
    }
    const OfSpdConstants of_spd{
        .mips = 4,
        .num_work_groups = display_spd[2],
        .work_group_offset = {0, 0},
        .num_work_groups_input_pyramid = display_spd[2],
        .pad = {},
    };

    const VkDeviceSize alignment =
        (std::max<VkDeviceSize>)(device.GetUniformBufferAlignment(), 16);
    const auto align = [&](size_t size) {
        return Common::AlignUp(static_cast<VkDeviceSize>(size), alignment);
    };
    const VkDeviceSize fi_slot = align(sizeof(FiConstants));
    const VkDeviceSize pyramid_slot = align(sizeof(PyramidConstants));
    const VkDeviceSize of_slot = align(sizeof(OfConstants));
    const VkDeviceSize spd_slot = align(sizeof(OfSpdConstants));
    const VkDeviceSize ui_slot = align(sizeof(UiMaskConstants));
    const VkDeviceSize total =
        fi_slot + 2 * pyramid_slot + (FLOW_LEVELS + 1) * of_slot + spd_slot + 2 * ui_slot;
    const StagingBufferRef uniforms = staging_buffer_pool.Request(total, MemoryUsage::Upload);
    VkDeviceSize offset = 0;
    const auto stage = [&](const void* data, size_t size, VkDeviceSize slot_size) {
        std::memcpy(uniforms.mapped_span.data() + offset, data, size);
        const Resource result{.buffer = uniforms.buffer,
                              .offset = uniforms.offset + offset,
                              .size = slot_size};
        offset += slot_size;
        return result;
    };
    const Resource cb_fi = stage(&fi, sizeof(fi), fi_slot);
    const Resource cb_game_pyramid = stage(&game_pyramid, sizeof(game_pyramid), pyramid_slot);
    const Resource cb_display_pyramid =
        stage(&display_pyramid, sizeof(display_pyramid), pyramid_slot);
    std::array<Resource, FLOW_LEVELS + 1> cb_of{};
    for (u32 i = 0; i <= FLOW_LEVELS; ++i) {
        cb_of[i] = stage(&of[i], sizeof(OfConstants), of_slot);
    }
    const Resource cb_of_common = cb_of[FLOW_LEVELS];
    const Resource cb_of_spd = stage(&of_spd, sizeof(of_spd), spd_slot);
    UiMaskConstants ui_constants{
        .rect_base = {in.interpolation_rect.offset.x, in.interpolation_rect.offset.y},
        .rect_size = {static_cast<s32>(in.interpolation_rect.extent.width),
                      static_cast<s32>(in.interpolation_rect.extent.height)},
        .threshold = 0.004f,
        .show_mask = in.show_ui_mask ? 1U : 0U,
        .layer_count = static_cast<u32>(std::count_if(ui_layers.begin(), ui_layers.end(),
                                                      [](VkImageView v) { return v != VkImageView{}; })),
        // SDR frames: the UI is taken out; HDR ones: what it covers is filled from around it.
        .mode = in.transfer_function == 0 ? 0U : 3U,
    };
    const Resource cb_ui_remove = stage(&ui_constants, sizeof(ui_constants), ui_slot);
    // With the game's UI layer, frame generation works on the frame without its UI. An SDR
    // frame's UI goes back over the generated scene afterwards; an HDR frame's pixels under it are
    // the real frame's.
    const bool has_ui = ui_layers[0] != VkImageView{};
    const bool remove_ui = has_ui;
    ui_constants.mode = in.transfer_function == 0 ? 1U : 2U;
    const Resource cb_ui_finish = stage(&ui_constants, sizeof(ui_constants), ui_slot);

    // Clears: every new image, the optical flow's history on a reset.
    std::vector<VkImage> initialize;
    std::vector<VkImage> clear_zero;
    if (fixed_resources_new) {
        initialize.push_back(*distortion_field.image);
        initialize.push_back(*zero_uint.image);
        clear_zero = initialize;
        initialize.push_back(*no_ui.image); // cleared to "no UI" below
    }
    if (display_resources_new) {
        std::vector<VkImage> images{*inpainting_pyramid.image, *previous_source.image,
                                    *flow_vector.image,        *scd_histogram.image,
                                    *scd_previous_histogram.image, *scd_temp.image,
                                    *scd_output.image};
        for (size_t i = 0; i < KEPT_FRAMES; ++i) {
            images.push_back(*ui_less[i].image);
            images.push_back(*ui_alpha[i].image);
        }
        for (const auto& set : flow_input) {
            for (const Texture& texture : set) {
                images.push_back(*texture.image);
            }
        }
        for (const auto& set : flow) {
            for (const Texture& texture : set) {
                images.push_back(*texture.image);
            }
        }
        initialize.insert(initialize.end(), images.begin(), images.end());
        clear_zero.insert(clear_zero.end(), images.begin(), images.end());
    } else if (flow_reset) {
        clear_zero.insert(clear_zero.end(),
                          {*scd_temp.image, *scd_output.image, *scd_histogram.image,
                           *scd_previous_histogram.image});
        for (const auto& set : flow_input) {
            for (const Texture& texture : set) {
                clear_zero.push_back(*texture.image);
            }
        }
    }
    if (render_resources_new) {
        for (const Texture* texture :
             {&reconstructed_interpolated_depth, &game_motion_x, &game_motion_y, &flow_motion_x,
              &flow_motion_y, &disocclusion_mask}) {
            initialize.push_back(*texture->image);
            clear_zero.push_back(*texture->image);
        }
        render_resources_new = false;
    }
    const VkBuffer counters_clear =
        (fixed_resources_new || display_resources_new) ? *counters : VkBuffer{};
    const bool clear_no_ui = fixed_resources_new;
    fixed_resources_new = false;
    display_resources_new = false;

    scheduler.RequestOutsideRenderPassOperationContext();
    RecordClears(scheduler, std::move(initialize), std::move(clear_zero), counters_clear);
    if (clear_no_ui) {
        // Alpha 1: the scene's share everywhere, as the game's layer has where there is no UI.
        scheduler.Record([image = *no_ui.image](vk::CommandBuffer cmdbuf) {
            cmdbuf.ClearColorImage(image, VK_IMAGE_LAYOUT_GENERAL,
                                   VkClearColorValue{.float32 = {0.0f, 0.0f, 0.0f, 1.0f}},
                                   ALL_LEVELS);
            cmdbuf.PipelineBarrier(VK_PIPELINE_STAGE_TRANSFER_BIT,
                                   VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0,
                                   MemoryBarrier(VK_ACCESS_TRANSFER_WRITE_BIT,
                                                 VK_ACCESS_SHADER_READ_BIT));
        });
    }
    const size_t ring = static_cast<size_t>(ui_ring_next++ % KEPT_FRAMES);
    const Texture& ui_less_now = ui_less[ring];
    const Texture& ui_alpha_now = ui_alpha[ring];
    const auto tex = [](const Texture& texture) { return Resource{.view = *texture.view}; };
    const auto groups = [](u32 size, u32 tile) { return (size + tile - 1) / tile; };
    const Resource real_current{.view = in.current_view};
    const auto layer = [&](size_t i) {
        if (!ui_layers[0]) {
            return tex(no_ui);
        }
        return Resource{.view = ui_layers[i] ? ui_layers[i] : ui_layers[0]};
    };
    if (in.dlss) {
        // NVIDIA's frame generation makes the frame: it gets this frame without its UI and the
        // UI's coverage (both at the frame's size), with the prepared depth and motion.
        passes[PassUiMask]->Dispatch(scheduler, descriptor_queue,
                                     std::array{layer(0), layer(1), layer(2), layer(3),
                                                real_current, tex(ui_less_now), cb_ui_remove,
                                                tex(ui_alpha_now)},
                                     groups(dw, 8), groups(dh, 8));
        dlss_images = DlssImages{
            .depth = {*frame.dilated_depth.image, *frame.dilated_depth.view,
                      VK_FORMAT_R32_SFLOAT, render_extent},
            .motion_vectors = {*frame.dilated_motion_vectors.image,
                               *frame.dilated_motion_vectors.view, VK_FORMAT_R16G16_SFLOAT,
                               render_extent},
            .hudless = {*ui_less_now.image, *ui_less_now.view, display_format, display_extent},
            .ui_alpha = {*ui_alpha_now.image, *ui_alpha_now.view, VK_FORMAT_R16_SFLOAT,
                         display_extent},
            .jitter = frame.jitter,
            .reset = reset,
        };
        has_previous = true;
        return;
    }
    // The output frame: whatever it held is replaced (every pixel is written).
    scheduler.Record([output = in.output_image](vk::CommandBuffer cmdbuf) {
        const VkImageMemoryBarrier barrier{
            .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
            .pNext = nullptr,
            .srcAccessMask = 0,
            .dstAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT,
            .oldLayout = VK_IMAGE_LAYOUT_UNDEFINED,
            .newLayout = VK_IMAGE_LAYOUT_GENERAL,
            .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .image = output,
            .subresourceRange = ALL_LEVELS,
        };
        cmdbuf.PipelineBarrier(VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
                               VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, {}, {}, barrier);
    });

    const Resource output{.view = in.output_view};
    if (remove_ui) {
        passes[PassUiMask]->Dispatch(scheduler, descriptor_queue,
                                     std::array{layer(0), layer(1), layer(2), layer(3),
                                                real_current, tex(ui_less_now), cb_ui_remove,
                                                tex(ui_alpha_now)},
                                     groups(dw, 8), groups(dh, 8));
    }
    // What frame generation works on: this frame, without its UI where it could be taken out.
    const Resource current = remove_ui ? tex(ui_less_now) : real_current;
    const Resource counters_buffer{.buffer = *counters, .offset = 0, .size = 16};

    // Optical flow on the current frame (ffxOpticalflowContextDispatch).
    {
        const u32 p = flow_parity;
        const u32 q = p ^ 1;
        auto& input = flow_input[p];
        auto& previous_input = flow_input[q];
        passes[PassPrepareLuma]->Dispatch(scheduler, descriptor_queue,
                                          std::array{current, tex(input[0]), cb_of_common},
                                          groups(groups(dw, 2), 16), groups(groups(dh, 2), 16));
        passes[PassLuminancePyramid]->Dispatch(
            scheduler, descriptor_queue,
            std::array{tex(input[0]), tex(input[1]), tex(input[2]), tex(input[3]),
                       tex(input[4]), tex(input[5]), tex(input[6]), cb_of_common, cb_of_spd},
            display_spd[0], display_spd[1]);
        passes[PassScdHistogram]->Dispatch(
            scheduler, descriptor_queue,
            std::array{tex(input[0]), tex(scd_histogram), cb_of_common},
            groups((dw / 4) / 3, 32), 16, 9);
        passes[PassScdDivergence]->Dispatch(
            scheduler, descriptor_queue,
            std::array{tex(scd_histogram), tex(scd_previous_histogram), tex(scd_temp),
                       tex(scd_output), cb_of_common},
            9, 3);
        std::array<VkExtent2D, FLOW_LEVELS> sizes{};
        sizes[0] = {groups(dw, FLOW_BLOCK_SIZE), groups(dh, FLOW_BLOCK_SIZE)};
        for (u32 level = 1; level < FLOW_LEVELS; ++level) {
            sizes[level] = {(sizes[level - 1].width + 1) / 2, (sizes[level - 1].height + 1) / 2};
        }
        for (s32 level = FLOW_LEVELS - 1; level >= 0; --level) {
            const u32 l = static_cast<u32>(level);
            const bool odd_level = (l & 1) != 0;
            const bool odd_frame = p != 0;
            auto& flow_a = flow[(odd_frame != odd_level) ? 1 : 0];
            auto& flow_b = flow[(odd_frame != odd_level) ? 0 : 1];
            const u32 luma_w = (std::max)(dw >> l, 1U);
            const u32 luma_h = (std::max)(dh >> l, 1U);
            passes[PassSearch]->Dispatch(
                scheduler, descriptor_queue,
                std::array{tex(input[l]), tex(previous_input[l]), tex(flow_a[l]),
                           tex(scd_output), cb_of[l]},
                groups(groups(luma_w, 4) * 16, 64), groups(luma_h, 16));
            const Resource filtered = l == 0 ? tex(flow_vector) : tex(flow_b[l]);
            passes[PassFilter]->Dispatch(scheduler, descriptor_queue,
                                         std::array{tex(flow_a[l]), filtered, cb_of[l]},
                                         groups(sizes[l].width, 16), groups(sizes[l].height, 4));
            if (l > 0) {
                passes[PassScale]->Dispatch(
                    scheduler, descriptor_queue,
                    std::array{tex(input[l]), tex(previous_input[l]), tex(flow_b[l]),
                               tex(flow_b[l - 1]), tex(scd_output), cb_of[l]},
                    groups(sizes[l - 1].width, 4), groups(sizes[l - 1].height, 4));
            }
        }
        flow_parity = q;
    }

    // Frame interpolation (ffxFrameInterpolationDispatch).
    const Resource previous = tex(previous_source);
    const auto pyramid_level = [&](u32 level) {
        if (inpainting_pyramid.levels.empty()) {
            return Resource{.view = *inpainting_pyramid.view};
        }
        const size_t index =
            (std::min)(static_cast<size_t>(level), inpainting_pyramid.levels.size() - 1);
        return Resource{.view = *inpainting_pyramid.levels[index]};
    };
    const auto pyramid_mips = [&](std::vector<Resource>& list) {
        for (u32 level = 0; level < PYRAMID_BINDINGS; ++level) {
            list.push_back(pyramid_level(level));
        }
    };
    passes[PassSetup]->Dispatch(
        scheduler, descriptor_queue,
        std::array{tex(scd_output), tex(game_motion_x), tex(game_motion_y), tex(flow_motion_x),
                   tex(flow_motion_y), tex(disocclusion_mask), counters_buffer, cb_fi},
        groups(rw, 8), groups(rh, 8));
    if (!reset) {
        scheduler.Record([image = *reconstructed_interpolated_depth.image](vk::CommandBuffer cmdbuf) {
            cmdbuf.ClearColorImage(image, VK_IMAGE_LAYOUT_GENERAL, VkClearColorValue{},
                                   ALL_LEVELS);
            cmdbuf.PipelineBarrier(VK_PIPELINE_STAGE_TRANSFER_BIT,
                                   VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0,
                                   MemoryBarrier(VK_ACCESS_TRANSFER_WRITE_BIT,
                                                 VK_ACCESS_SHADER_READ_BIT |
                                                     VK_ACCESS_SHADER_WRITE_BIT));
        });
        passes[PassReconstructPreviousDepth]->Dispatch(
            scheduler, descriptor_queue,
            std::array{tex(dilated_motion_vectors), tex(dilated_depth), current,
                       tex(distortion_field), tex(reconstructed_interpolated_depth), cb_fi},
            groups(rw, 8), groups(rh, 8));
        passes[PassGameMotionVectorField]->Dispatch(
            scheduler, descriptor_queue,
            std::array{tex(dilated_motion_vectors), tex(dilated_depth), previous, current,
                       tex(distortion_field), tex(game_motion_x), tex(game_motion_y), cb_fi},
            groups(rw, 8), groups(rh, 8));
        {
            std::vector<Resource> list{tex(game_motion_x), tex(game_motion_y), counters_buffer};
            pyramid_mips(list);
            list.push_back(cb_fi);
            list.push_back(cb_game_pyramid);
            passes[PassGameVectorFieldPyramid]->Dispatch(scheduler, descriptor_queue, list,
                                                         game_spd[0], game_spd[1]);
        }
        const u32 flow_groups_x =
            (static_cast<u32>(static_cast<f32>(dw) / static_cast<f32>(FLOW_BLOCK_SIZE)) + 7) / 8;
        const u32 flow_groups_y =
            (static_cast<u32>(static_cast<f32>(dh) / static_cast<f32>(FLOW_BLOCK_SIZE)) + 7) / 8;
        passes[PassOpticalFlowVectorField]->Dispatch(
            scheduler, descriptor_queue,
            std::array{tex(flow_vector), tex(zero_uint), tex(dilated_depth), previous, current,
                       tex(flow_motion_x), tex(flow_motion_y), cb_fi},
            flow_groups_x, flow_groups_y);
        passes[PassDisocclusionMask]->Dispatch(
            scheduler, descriptor_queue,
            std::array{tex(game_motion_x), tex(game_motion_y), tex(reconstructed_previous_depth),
                       tex(dilated_depth), tex(reconstructed_interpolated_depth),
                       tex(inpainting_pyramid), tex(distortion_field), tex(disocclusion_mask),
                       cb_fi},
            groups(rw, 8), groups(rh, 8));
    }
    passes[PassInterpolation]->Dispatch(
        scheduler, descriptor_queue,
        std::array{tex(game_motion_x), tex(game_motion_y), tex(flow_motion_x), tex(flow_motion_y),
                   previous, current, tex(disocclusion_mask), tex(inpainting_pyramid),
                   counters_buffer, output, cb_fi},
        groups(dw, 8), groups(dh, 8));
    {
        std::vector<Resource> list{output, counters_buffer};
        pyramid_mips(list);
        list.push_back(cb_fi);
        list.push_back(cb_display_pyramid);
        passes[PassInpaintingPyramid]->Dispatch(scheduler, descriptor_queue, list,
                                                display_spd[0], display_spd[1]);
    }
    passes[PassInpainting]->Dispatch(
        scheduler, descriptor_queue,
        std::array{tex(scd_output), tex(inpainting_pyramid), current, current, output, cb_fi},
        groups(dw, 8), groups(dh, 8));
    if (has_ui) {
        passes[PassUiMask]->Dispatch(
            scheduler, descriptor_queue,
            std::array{layer(0), layer(1), layer(2), layer(3), real_current, output,
                       cb_ui_finish, tex(ui_alpha_now)},
            groups(dw, 8), groups(dh, 8));
    }

    if (in.show_ui_less && remove_ui) {
        scheduler.Record([source = *ui_less_now.image, destination = in.output_image, dw,
                          dh](vk::CommandBuffer cmdbuf) {
            cmdbuf.CopyImage(source, VK_IMAGE_LAYOUT_GENERAL, destination, VK_IMAGE_LAYOUT_GENERAL,
                             VkImageCopy{
                                 .srcSubresource{VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1},
                                 .srcOffset{0, 0, 0},
                                 .dstSubresource{VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1},
                                 .dstOffset{0, 0, 0},
                                 .extent{dw, dh, 1},
                             });
        });
    }

    // Keep this frame (as frame generation saw it) for the next interpolation; hand the output to
    // the presentation.
    scheduler.Record([source = remove_ui ? *ui_less_now.image : in.current_image,
                      destination = *previous_source.image, dw,
                      dh](vk::CommandBuffer cmdbuf) {
        const VkImageCopy copy{
            .srcSubresource{VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1},
            .srcOffset{0, 0, 0},
            .dstSubresource{VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1},
            .dstOffset{0, 0, 0},
            .extent{dw, dh, 1},
        };
        cmdbuf.CopyImage(source, VK_IMAGE_LAYOUT_GENERAL, destination, VK_IMAGE_LAYOUT_GENERAL,
                         copy);
        cmdbuf.PipelineBarrier(VK_PIPELINE_STAGE_TRANSFER_BIT | VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                               VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0,
                               MemoryBarrier(VK_ACCESS_TRANSFER_WRITE_BIT |
                                                 VK_ACCESS_SHADER_WRITE_BIT,
                                             VK_ACCESS_SHADER_READ_BIT |
                                                 VK_ACCESS_TRANSFER_READ_BIT |
                                                 VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT |
                                                 VK_ACCESS_MEMORY_READ_BIT));
    });
    has_previous = true;
}

} // namespace Vulkan
