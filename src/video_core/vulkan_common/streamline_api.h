// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

// Streamline's API as loaded from the user's sl.interposer.dll. Only for the
// Streamline sources: it pulls in <windows.h> and Streamline's (MIT-licensed) headers.

#pragma once

#ifdef _WIN32
#include <windows.h>

#include "video_core/vulkan_common/vulkan_wrapper.h"

#include <sl.h>
#include <sl_dlss.h>
#include <sl_dlss_g.h>
#include <sl_pcl.h>
#include <sl_reflex.h>
#include <sl_helpers_vk.h>

namespace Vulkan::Streamline {

struct Api {
    PFun_slInit* init{};
    PFun_slShutdown* shutdown{};
    PFun_slIsFeatureSupported* is_feature_supported{};
    PFun_slGetFeatureRequirements* get_feature_requirements{};
    PFun_slGetFeatureVersion* get_feature_version{};
    PFun_slGetFeatureFunction* get_feature_function{};
    PFun_slGetNewFrameToken* get_new_frame_token{};
    PFun_slSetConstants* set_constants{};
    PFun_slEvaluateFeature* evaluate_feature{};
    PFun_slFreeResources* free_resources{};
    PFun_slSetVulkanInfo* set_vulkan_info{};
    PFun_slSetTagForFrame* set_tag_for_frame{};
};

/// The loaded functions; only valid while IsDlssAvailable().
[[nodiscard]] const Api& GetApi();

} // namespace Vulkan::Streamline
#else
#include <cstdint>
#include "video_core/vulkan_common/vulkan_wrapper.h"

namespace Vulkan::Streamline {

using NgxResult = u32;
constexpr NgxResult NgxSuccess = 0x1u;
[[nodiscard]] constexpr bool NgxSucceeded(NgxResult r) noexcept {
    return (r & 0xFFF00000u) != 0xBAD00000u;
}
[[nodiscard]] constexpr bool NgxFailed(NgxResult r) noexcept {
    return !NgxSucceeded(r);
}

enum class NgxFeature : u32 {
    SuperSampling = 1,
};

enum class NgxPerfQuality : int {
    MaxPerf = 0,
    Balanced = 1,
    MaxQuality = 2,
    UltraPerformance = 3,
    UltraQuality = 4,
    DLAA = 5,
};

enum class NgxDlssPreset : u32 {
    Default = 0,
    J = 10,
    K = 11,
    L = 12,
    M = 13,
};

enum NgxDlssFeatureFlags : int {
    NgxDlssFlagNone = 0,
    NgxDlssFlagIsHDR = 1 << 0,
    NgxDlssFlagMVLowRes = 1 << 1,
    NgxDlssFlagMVJittered = 1 << 2,
    NgxDlssFlagDepthInverted = 1 << 3,
    NgxDlssFlagAutoExposure = 1 << 6,
};

enum class NgxLoggingLevel : u32 {
    Off = 0,
    On = 1,
    Verbose = 2,
};

struct NgxHandle {
    u32 id;
};

struct NgxParameterVtable {
    void (*SetULL)(void*, const char*, unsigned long long);
    void (*SetF)(void*, const char*, float);
    void (*SetD)(void*, const char*, double);
    void (*SetUI)(void*, const char*, unsigned int);
    void (*SetI)(void*, const char*, int);
    void (*SetD3d11Resource)(void*, const char*, void*);
    void (*SetD3d12Resource)(void*, const char*, void*);
    void (*SetVoidPointer)(void*, const char*, void*);
    NgxResult (*GetULL)(void*, const char*, unsigned long long*);
    NgxResult (*GetF)(void*, const char*, float*);
    NgxResult (*GetD)(void*, const char*, double*);
    NgxResult (*GetUI)(void*, const char*, unsigned int*);
    NgxResult (*GetI)(void*, const char*, int*);
    NgxResult (*GetD3d11Resource)(void*, const char*, void**);
    NgxResult (*GetD3d12Resource)(void*, const char*, void**);
    NgxResult (*GetVoidPointer)(void*, const char*, void**);
};

struct NgxParameter {
    const NgxParameterVtable* vtable;

    void SetF(const char* name, float v) { vtable->SetF(this, name, v); }
    void SetUI(const char* name, unsigned int v) { vtable->SetUI(this, name, v); }
    void SetI(const char* name, int v) { vtable->SetI(this, name, v); }
    void SetVoidPointer(const char* name, void* v) { vtable->SetVoidPointer(this, name, v); }
    NgxResult GetUI(const char* name, unsigned int* v) { return vtable->GetUI(this, name, v); }
    NgxResult GetI(const char* name, int* v) { return vtable->GetI(this, name, v); }
};

struct NgxPathListInfo {
    const wchar_t* const* path;
    unsigned int length;
};

using NgxAppLogCallback = void (*)(const char* message, NgxLoggingLevel level, NgxFeature source);

struct NgxLoggingInfo {
    NgxAppLogCallback callback;
    NgxLoggingLevel min_level;
    bool disable_other_sinks;
};

struct NgxFeatureCommonInfo {
    NgxPathListInfo path_list;
    void* internal_data;
    NgxLoggingInfo logging;
};

struct NgxFeatureDiscoveryInfo {
    u32 sdk_version;
    NgxFeature feature_id;
    u32 identifier_type;
    const char* project_id;
    u32 engine_type;
    const char* engine_version;
    const wchar_t* app_data_path;
    const NgxFeatureCommonInfo* feature_info;
};

struct NgxImageViewInfoVk {
    VkImageView image_view;
    VkImage image;
    VkImageSubresourceRange subresource_range;
    VkFormat format;
    unsigned int width;
    unsigned int height;
};

struct NgxBufferInfoVk {
    VkBuffer buffer;
    unsigned int size_in_bytes;
};

struct NgxResourceVk {
    union {
        NgxImageViewInfoVk image_view_info;
        NgxBufferInfoVk buffer_info;
    };
    u32 type;
    bool read_write;
};

inline NgxResourceVk MakeNgxImageViewResource(VkImageView view, VkImage image,
                                              VkImageSubresourceRange range, VkFormat format,
                                              u32 width, u32 height, bool read_write) {
    NgxResourceVk res{};
    res.type = 0; // VK_IMAGEVIEW
    res.image_view_info = {view, image, range, format, width, height};
    res.read_write = read_write;
    return res;
}

using PFN_NgxVulkanInitProjectIdExt =
    NgxResult (*)(const char* project_id, u32 engine_type, const char* engine_version,
                  const wchar_t* app_data_path, VkInstance instance, VkPhysicalDevice physical,
                  VkDevice device, PFN_vkGetInstanceProcAddr gipa, PFN_vkGetDeviceProcAddr gdpa,
                  u32 sdk_version, const NgxFeatureCommonInfo* feature_info);
using PFN_NgxVulkanShutdown1 = NgxResult (*)(VkDevice device, int* out_ref_count);
using PFN_NgxVulkanGetCapabilityParameters = NgxResult (*)(NgxParameter** out_params);
using PFN_NgxVulkanDestroyParameters = NgxResult (*)(NgxParameter* params);
using PFN_NgxVulkanCreateFeature1 = NgxResult (*)(VkDevice device, VkCommandBuffer cmd,
                                                  NgxFeature feature, NgxParameter* params,
                                                  NgxHandle** out_handle);
using PFN_NgxVulkanEvaluateFeature = NgxResult (*)(VkCommandBuffer cmd, const NgxHandle* handle,
                                                   const NgxParameter* params, void* callback);
using PFN_NgxVulkanReleaseFeature = NgxResult (*)(NgxHandle* handle);
using PFN_NgxVulkanGetFeatureInstanceExtensionRequirements =
    NgxResult (*)(const NgxFeatureDiscoveryInfo* info, uint32_t* count,
                  VkExtensionProperties** exts);

struct NgxApi {
    PFN_NgxVulkanInitProjectIdExt init_project_id_ext{};
    PFN_NgxVulkanShutdown1 shutdown1{};
    PFN_NgxVulkanGetCapabilityParameters get_capability_parameters{};
    PFN_NgxVulkanDestroyParameters destroy_parameters{};
    PFN_NgxVulkanCreateFeature1 create_feature1{};
    PFN_NgxVulkanEvaluateFeature evaluate_feature{};
    PFN_NgxVulkanReleaseFeature release_feature{};
    PFN_NgxVulkanGetFeatureInstanceExtensionRequirements get_instance_exts{};
};

[[nodiscard]] const NgxApi& GetNgxApi();
[[nodiscard]] NgxParameter* GetNgxParameters();

} // namespace Vulkan::Streamline
#endif
