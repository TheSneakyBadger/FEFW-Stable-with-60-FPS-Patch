// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

// AMD FSR 4 through fsr4-vk's FidelityFX API provider (https://github.com/Astyyyyy/fsr4-vk,
// GPL-3.0; ported from an Eden FSR 4 build). The provider is loaded at run time
// (amd_fidelityfx_upscaler_vk.dll / .so) and owns its pipelines, weights and images.

#include <array>
#include <cstdlib>
#include <filesystem>
#include <mutex>
#include <string>
#include <vector>

#ifdef _WIN32
#include <windows.h>
#else
#include <dlfcn.h>
#endif

#if !defined(_WIN32)
#define __declspec(x)
#endif
#include "ffx_upscale.h"
#if !defined(_WIN32)
#undef __declspec
#endif

#include "common/fs/path_util.h"
#include "common/logging.h"
#include "video_core/renderer_vulkan/vk_fsr4_upscaler.h"
#include "video_core/renderer_vulkan/vk_scheduler.h"
#include "video_core/vulkan_common/vulkan_device.h"
#include "video_core/vulkan_common/vulkan_memory_allocator.h"
#include "video_core/vulkan_common/vulkan_wrapper.h"

namespace Vulkan {
namespace {

constexpr VkFormat OUTPUT_FORMAT = VK_FORMAT_R16G16B16A16_SFLOAT;
constexpr VkImageUsageFlags OUTPUT_USAGE =
    VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT |
    VK_IMAGE_USAGE_TRANSFER_DST_BIT;
#ifdef _WIN32
constexpr const char* LIBRARY_NAME = "amd_fidelityfx_upscaler_vk.dll";
#else
constexpr const char* LIBRARY_NAME = "amd_fidelityfx_upscaler_vk.so";
#endif

void* OpenLibrary(const std::filesystem::path& path) {
#ifdef _WIN32
    return static_cast<void*>(LoadLibraryW(path.wstring().c_str()));
#else
    return dlopen(path.c_str(), RTLD_NOW | RTLD_LOCAL);
#endif
}

void* LibrarySymbol(void* library, const char* name) {
#ifdef _WIN32
    return reinterpret_cast<void*>(GetProcAddress(static_cast<HMODULE>(library), name));
#else
    return dlsym(library, name);
#endif
}

void CloseLibrary(void* library) {
#ifdef _WIN32
    FreeLibrary(static_cast<HMODULE>(library));
#else
    dlclose(library);
#endif
}

std::string LibraryError() {
#ifdef _WIN32
    return std::to_string(GetLastError());
#else
    const char* const error = dlerror();
    return error ? error : "unknown";
#endif
}
// The Vulkan backend descriptor: AMD's FSR SDK 2.4 type (FFX_API_CREATE_CONTEXT_DESC_TYPE_BACKEND_VK,
// FFX_API_BACKEND_ID_VK | 0x03), and the deprecated 0x03 that fsr4-vk takes. Same layout.
constexpr ffxStructType_t BACKEND_VK_DESC_TYPE = 0x01000003u;
constexpr ffxStructType_t BACKEND_VK_DESC_TYPE_DEPRECATED = 3;
// FSR SDK 2.1 and later: ffxCreateContextDescUpscaleVersion must be chained to the upscale
// descriptor (FFX_API_CREATE_CONTEXT_DESC_TYPE_UPSCALE_VERSION), else AMD's library refuses the
// context. FFX_UPSCALER_VERSION 4.1.2 (FSR SDK 2.4).
constexpr ffxStructType_t UPSCALE_VERSION_DESC_TYPE = 0x0001000bu;
constexpr uint32_t UPSCALER_VERSION = (4u << 22) | (1u << 12) | 2u;

struct UpscaleVersionDesc {
    ffxCreateContextDescHeader header;
    uint32_t version;
};

struct BackendVkDesc {
    ffxCreateContextDescHeader header;
    VkDevice device;
    VkPhysicalDevice physical_device;
    PFN_vkGetDeviceProcAddr get_device_proc_addr;
};

using CreateContextFn = ffxReturnCode_t (*)(ffxContext*, ffxCreateContextDescHeader*,
                                            const ffxAllocationCallbacks*);
using DestroyContextFn = ffxReturnCode_t (*)(ffxContext*, const ffxAllocationCallbacks*);
using QueryFn = ffxReturnCode_t (*)(ffxContext*, ffxQueryDescHeader*);
using DispatchFn = ffxReturnCode_t (*)(ffxContext*, const ffxDispatchDescHeader*);

struct Fsr4Api {
    void* library{};
    CreateContextFn create{};
    DestroyContextFn destroy{};
    QueryFn query{};
    DispatchFn dispatch{};

    bool Load() {
        std::call_once(load_once, [this] { LoadOnce(); });
        return library != nullptr;
    }

private:
    void LoadOnce() {
        std::vector<std::filesystem::path> candidates;
        if (const char* path = std::getenv("EDEN_FSR4_PATH"); path && *path) {
            std::filesystem::path candidate{path};
            std::error_code ec;
            if (std::filesystem::is_directory(candidate, ec)) {
                candidate /= LIBRARY_NAME;
            }
            candidates.push_back(std::move(candidate));
        }
        std::error_code ec;
#ifdef _WIN32
        // Next to Fortune's Eden.exe (where the package puts it), then the user folder.
        candidates.push_back(Common::FS::GetExeDirectory() / LIBRARY_NAME);
        candidates.push_back(Common::FS::GetEdenPath(Common::FS::EdenPath::EdenDir) /
                             LIBRARY_NAME);
#else
        if (const auto executable = std::filesystem::read_symlink("/proc/self/exe", ec); !ec) {
            candidates.push_back(executable.parent_path() / LIBRARY_NAME);
        }
        if (const char* appimage = std::getenv("APPIMAGE"); appimage && *appimage) {
            candidates.push_back(std::filesystem::path{appimage}.parent_path() / LIBRARY_NAME);
        }
        candidates.push_back(Common::FS::GetEdenPath(Common::FS::EdenPath::EdenDir) / LIBRARY_NAME);
        candidates.emplace_back(std::filesystem::current_path() / LIBRARY_NAME);
        candidates.emplace_back("externals/fsr4-vk/build/linux/" + std::string{LIBRARY_NAME});
#endif

        for (const auto& candidate : candidates) {
            if (candidate.empty() || !std::filesystem::exists(candidate, ec)) {
                continue;
            }
            library = OpenLibrary(candidate);
            if (library) {
                LOG_INFO(Render_Vulkan, "FSR4 provider loaded: {}",
                         Common::FS::PathToUTF8String(candidate));
                break;
            }
            LOG_WARNING(Render_Vulkan, "FSR4 provider {} failed to load ({})",
                        Common::FS::PathToUTF8String(candidate), LibraryError());
        }
        if (!library) {
            LOG_WARNING(Render_Vulkan, "FSR4 provider {} not found", LIBRARY_NAME);
            return;
        }

        const auto symbol = [this](const char* name) { return LibrarySymbol(library, name); };
        create = reinterpret_cast<CreateContextFn>(symbol("ffxCreateContext"));
        destroy = reinterpret_cast<DestroyContextFn>(symbol("ffxDestroyContext"));
        query = reinterpret_cast<QueryFn>(symbol("ffxQuery"));
        dispatch = reinterpret_cast<DispatchFn>(symbol("ffxDispatch"));
        if (!create || !destroy || !query || !dispatch) {
            LOG_WARNING(Render_Vulkan, "FSR4 provider is missing the FidelityFX API exports");
            CloseLibrary(library);
            library = nullptr;
        }
    }

    std::once_flag load_once;
};

Fsr4Api& GetApi() {
    static Fsr4Api api;
    return api;
}

uint32_t ToFfxFormat(VkFormat format) {
    switch (format) {
    case VK_FORMAT_R16G16B16A16_SFLOAT:
        return FFX_API_SURFACE_FORMAT_R16G16B16A16_FLOAT;
    case VK_FORMAT_B10G11R11_UFLOAT_PACK32:
        return FFX_API_SURFACE_FORMAT_R11G11B10_FLOAT;
    case VK_FORMAT_E5B9G9R9_UFLOAT_PACK32:
        return FFX_API_SURFACE_FORMAT_R9G9B9E5_SHAREDEXP;
    case VK_FORMAT_R16G16_SFLOAT:
        return FFX_API_SURFACE_FORMAT_R16G16_FLOAT;
    case VK_FORMAT_R32G32_SFLOAT:
        return FFX_API_SURFACE_FORMAT_R32G32_FLOAT;
    case VK_FORMAT_R32_SFLOAT:
    case VK_FORMAT_D32_SFLOAT:
    case VK_FORMAT_D32_SFLOAT_S8_UINT:
    case VK_FORMAT_X8_D24_UNORM_PACK32:
    case VK_FORMAT_D24_UNORM_S8_UINT:
        return FFX_API_SURFACE_FORMAT_R32_FLOAT;
    default:
        return FFX_API_SURFACE_FORMAT_UNKNOWN;
    }
}

FfxApiResource MakeResource(const Fsr4Upscaler::Image& image, u32 width, u32 height, u32 usage,
                            u32 state) {
    FfxApiResource resource{};
    resource.resource = reinterpret_cast<void*>(image.image);
    resource.description.type = FFX_API_RESOURCE_TYPE_TEXTURE2D;
    resource.description.format = ToFfxFormat(image.format);
    resource.description.width = width;
    resource.description.height = height;
    resource.description.depth = 1;
    resource.description.mipCount = 1;
    resource.description.flags = 0;
    resource.description.usage = usage;
    resource.state = state;
    return resource;
}

VkImageMemoryBarrier Barrier(const Fsr4Upscaler::Image& image, VkAccessFlags src_access,
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

// The provider's own error and warning text (why a context can't be created, say).
void LogProviderMessage(uint32_t type, const wchar_t* message) {
    std::string text;
    for (const wchar_t* c = message; c && *c; ++c) {
        text.push_back(*c > 0 && *c < 128 ? static_cast<char>(*c) : '?');
    }
    if (type == FFX_API_MESSAGE_TYPE_ERROR) {
        LOG_ERROR(Render_Vulkan, "FSR4 provider: {}", text);
    } else {
        LOG_WARNING(Render_Vulkan, "FSR4 provider: {}", text);
    }
}

constexpr VkPipelineStageFlags WRITE_STAGES =
    VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT | VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT |
    VK_PIPELINE_STAGE_TRANSFER_BIT;

} // Anonymous namespace

Fsr4Upscaler::Fsr4Upscaler(const Device& device_, Scheduler& scheduler_,
                           MemoryAllocator& memory_allocator_, VkExtent2D max_render,
                           VkExtent2D max_upscale)
    : device{device_}, scheduler{scheduler_}, memory_allocator{memory_allocator_},
      max_render_size{max_render}, max_upscale_size{max_upscale} {
    if (!GetApi().Load()) {
        throw vk::Exception(VK_ERROR_FEATURE_NOT_PRESENT);
    }

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

    BackendVkDesc backend{
        .header{.type = BACKEND_VK_DESC_TYPE, .pNext = nullptr},
        .device = *device.GetLogical(),
        .physical_device = static_cast<VkPhysicalDevice>(device.GetPhysical()),
        .get_device_proc_addr = device.GetDispatchLoader().vkGetDeviceProcAddr,
    };
    UpscaleVersionDesc version{
        .header{.type = UPSCALE_VERSION_DESC_TYPE,
                .pNext = reinterpret_cast<ffxApiHeader*>(&backend)},
        .version = UPSCALER_VERSION,
    };
    ffxCreateContextDescUpscale desc{};
    desc.header.type = FFX_API_CREATE_CONTEXT_DESC_TYPE_UPSCALE;
    desc.header.pNext = reinterpret_cast<ffxApiHeader*>(&version);
    desc.flags = FFX_UPSCALE_ENABLE_HIGH_DYNAMIC_RANGE |
                 FFX_UPSCALE_ENABLE_DEPTH_INVERTED | FFX_UPSCALE_ENABLE_AUTO_EXPOSURE;
    desc.maxRenderSize = {max_render_size.width, max_render_size.height};
    desc.maxUpscaleSize = {max_upscale_size.width, max_upscale_size.height};
    desc.fpMessage = &LogProviderMessage;

    // AMD's SDK 2.4 descriptors first; then the deprecated backend type with no version
    // descriptor, as before (fsr4-vk and older providers).
    ffxContext context_handle{};
    auto result = GetApi().create(&context_handle, &desc.header, nullptr);
    if (result != FFX_API_RETURN_OK) {
        LOG_WARNING(Render_Vulkan,
                    "FSR4 context creation with the FSR SDK 2.4 descriptors failed (FidelityFX "
                    "error {}); trying the older ones",
                    static_cast<unsigned>(result));
        backend.header.type = BACKEND_VK_DESC_TYPE_DEPRECATED;
        desc.header.pNext = reinterpret_cast<ffxApiHeader*>(&backend);
        context_handle = {};
        result = GetApi().create(&context_handle, &desc.header, nullptr);
    }
    if (result != FFX_API_RETURN_OK) {
        LOG_ERROR(Render_Vulkan, "FSR4 context creation failed (FidelityFX error {})",
                  static_cast<unsigned>(result));
        throw vk::Exception(VK_ERROR_INITIALIZATION_FAILED);
    }
    context = context_handle;
    if (GetApi().query) {
        ffxQueryGetProviderVersion query{};
        query.header.type = FFX_API_QUERY_DESC_TYPE_GET_PROVIDER_VERSION;
        if (GetApi().query(&context, &query.header) == FFX_API_RETURN_OK) {
            LOG_INFO(Render_Vulkan, "FSR4 provider: {} (id 0x{:x})",
                     query.versionName ? query.versionName : "unknown", query.versionId);
        }
    }
    LOG_INFO(Render_Vulkan, "FSR4 temporal upscaler created (max {}x{} -> {}x{})",
             max_render_size.width, max_render_size.height, max_upscale_size.width,
             max_upscale_size.height);
}

Fsr4Upscaler::~Fsr4Upscaler() {
    if (context && GetApi().Load()) {
        void(device.GetLogical().WaitIdle());
        GetApi().destroy(&context, nullptr);
        context = nullptr;
    }
}

bool Fsr4Upscaler::IsAvailable() {
    return GetApi().Load();
}

void Fsr4Upscaler::CreateOutput(VkExtent2D upscale_size) {
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

bool Fsr4Upscaler::Upscale(const Inputs& in) {
    if (failed || !context) {
        return false;
    }
    if (in.render_size.width == 0 || in.render_size.height == 0 || in.upscale_size.width == 0 ||
        in.upscale_size.height == 0) {
        return false;
    }
    if (ToFfxFormat(in.color.format) == FFX_API_SURFACE_FORMAT_UNKNOWN ||
        ToFfxFormat(in.motion_vectors.format) == FFX_API_SURFACE_FORMAT_UNKNOWN ||
        ToFfxFormat(in.depth.format) == FFX_API_SURFACE_FORMAT_UNKNOWN) {
        LOG_ERROR(Render_Vulkan, "FSR4 cannot use color {} / motion {} / depth {}",
                  static_cast<int>(in.color.format),
                  static_cast<int>(in.motion_vectors.format), static_cast<int>(in.depth.format));
        failed = true;
        return false;
    }

    bool reset = in.reset;
    if (!output_image || in.upscale_size.width != upscale_extent.width ||
        in.upscale_size.height != upscale_extent.height) {
        CreateOutput(in.upscale_size);
        reset = true;
    }
    render_extent = in.render_size;
    upscale_extent = in.upscale_size;

    const ffxContext ctx = context;
    const VkImage output = *output_image;
    scheduler.RequestOutsideRenderPassOperationContext();
    scheduler.Record([ctx, output, in, reset, output_size = in.upscale_size](
                         vk::CommandBuffer cmdbuf) {
        std::array<VkImageMemoryBarrier, 3> input_barriers{
            Barrier(in.color, WRITE_STAGES, VK_ACCESS_SHADER_READ_BIT, VK_IMAGE_LAYOUT_GENERAL,
                    VK_IMAGE_LAYOUT_GENERAL),
            Barrier(in.depth, WRITE_STAGES, VK_ACCESS_SHADER_READ_BIT, VK_IMAGE_LAYOUT_GENERAL,
                    VK_IMAGE_LAYOUT_GENERAL),
            Barrier(in.motion_vectors, WRITE_STAGES, VK_ACCESS_SHADER_READ_BIT,
                    VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_GENERAL),
        };
        for (VkImageMemoryBarrier& barrier : input_barriers) {
            cmdbuf.PipelineBarrier(WRITE_STAGES, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, barrier);
        }
        cmdbuf.PipelineBarrier(
            VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0,
            VkImageMemoryBarrier{
                .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
                .pNext = nullptr,
                .srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT,
                .dstAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT,
                .oldLayout = VK_IMAGE_LAYOUT_GENERAL,
                .newLayout = VK_IMAGE_LAYOUT_GENERAL,
                .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                .image = output,
                .subresourceRange{
                    .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
                    .baseMipLevel = 0,
                    .levelCount = 1,
                    .baseArrayLayer = 0,
                    .layerCount = 1,
                },
            });

        ffxDispatchDescUpscale desc{};
        desc.header.type = FFX_API_DISPATCH_DESC_TYPE_UPSCALE;
        desc.header.pNext = nullptr;
        desc.commandList = reinterpret_cast<void*>(*cmdbuf);
        desc.color = MakeResource(in.color, in.render_size.width, in.render_size.height, 0,
                                  FFX_API_RESOURCE_STATE_COMPUTE_READ);
        desc.depth = MakeResource(in.depth, in.render_size.width, in.render_size.height,
                                  FFX_API_RESOURCE_USAGE_DEPTHTARGET,
                                  FFX_API_RESOURCE_STATE_COMPUTE_READ);
        desc.motionVectors = MakeResource(in.motion_vectors, in.render_size.width,
                                          in.render_size.height, 0,
                                          FFX_API_RESOURCE_STATE_COMPUTE_READ);
        desc.output = MakeResource(
            Fsr4Upscaler::Image{
                .view = VK_NULL_HANDLE,
                .image = output,
                .format = OUTPUT_FORMAT,
                .range{},
                .barrier_aspect = VK_IMAGE_ASPECT_COLOR_BIT,
            },
            output_size.width, output_size.height, FFX_API_RESOURCE_USAGE_UAV,
            FFX_API_RESOURCE_STATE_UNORDERED_ACCESS);
        desc.jitterOffset = {in.jitter[0], in.jitter[1]};
        desc.motionVectorScale = {in.motion_vector_scale[0], in.motion_vector_scale[1]};
        desc.renderSize = {in.render_size.width, in.render_size.height};
        desc.upscaleSize = {output_size.width, output_size.height};
        desc.enableSharpening = false;
        desc.sharpness = 0.0f;
        desc.frameTimeDelta = 16.6667f;
        desc.preExposure = 1.0f;
        desc.reset = reset;
        desc.cameraNear = 0.1f;
        desc.cameraFar = 1000.0f;
        desc.cameraFovAngleVertical = 0.785398f;
        desc.viewSpaceToMetersFactor = 1.0f;
        desc.flags = 0;

        const auto result = GetApi().dispatch(
            const_cast<ffxContext*>(&ctx), &desc.header);
        if (result != FFX_API_RETURN_OK) {
            LOG_ERROR(Render_Vulkan, "FSR4 dispatch failed (FidelityFX error {})",
                      static_cast<unsigned>(result));
        }
    });
    return true;
}

} // namespace Vulkan
