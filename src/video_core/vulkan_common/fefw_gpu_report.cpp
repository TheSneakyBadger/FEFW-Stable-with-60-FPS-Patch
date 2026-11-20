// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include "video_core/vulkan_common/fefw_gpu_report.h"

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <cstdlib>
#include <iterator>
#include <mutex>
#include <string>
#include <string_view>
#include <type_traits>

#include <fmt/format.h>

#include "common/common_types.h"
#include "common/logging.h"
#include "video_core/vulkan_common/vulkan_device.h"

#ifdef _WIN32
#include <windows.h>
#include <dxgi1_4.h>
#else
#include <dlfcn.h>
#endif

namespace FefwGpuReport {
namespace {

// NVML (NVIDIA's management library, part of the driver), loaded at run time.
using nvmlReturn_t = int;
using nvmlDevice_t = struct nvmlDevice_st*;
struct nvmlUtilization_t {
    unsigned int gpu;
    unsigned int memory;
};
struct nvmlMemory_t {
    unsigned long long total;
    unsigned long long free;
    unsigned long long used;
};
constexpr int NVML_CLOCK_GRAPHICS = 0;
constexpr int NVML_TEMPERATURE_GPU = 0;

/// What held the clock back (nvmlClocksThrottleReasons), worth naming in the log.
constexpr std::pair<unsigned long long, std::string_view> HELD_BACK[]{
    {0x4, "power limit"},
    {0x8, "hardware slowdown"},
    {0x20, "temperature"},
    {0x40, "temperature (hardware)"},
    {0x80, "power brake"},
};

struct Nvml {
    nvmlReturn_t (*Init)();
    nvmlReturn_t (*Shutdown)();
    nvmlReturn_t (*GetCount)(unsigned int*);
    nvmlReturn_t (*GetHandle)(unsigned int, nvmlDevice_t*);
    nvmlReturn_t (*GetName)(nvmlDevice_t, char*, unsigned int);
    nvmlReturn_t (*GetUtilization)(nvmlDevice_t, nvmlUtilization_t*);
    nvmlReturn_t (*GetClock)(nvmlDevice_t, int, unsigned int*);
    nvmlReturn_t (*GetMaxClock)(nvmlDevice_t, int, unsigned int*);
    nvmlReturn_t (*GetTemperature)(nvmlDevice_t, int, unsigned int*);
    nvmlReturn_t (*GetPower)(nvmlDevice_t, unsigned int*);
    nvmlReturn_t (*GetReasons)(nvmlDevice_t, unsigned long long*);
    nvmlReturn_t (*GetMemory)(nvmlDevice_t, nvmlMemory_t*);
    void* library{};
    nvmlDevice_t device{};
};

void* LoadSymbol(void* library, const char* name) {
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

/// NVML for this card, if it's NVIDIA's and the driver has it.
bool OpenNvml(Nvml& nvml, std::string_view model) {
#ifdef _WIN32
    nvml.library = LoadLibraryExW(L"nvml.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
#else
    nvml.library = dlopen("libnvidia-ml.so.1", RTLD_NOW | RTLD_LOCAL);
#endif
    if (nvml.library == nullptr) {
        return false;
    }
    const auto load = [&](auto& function, const char* name) {
        function = reinterpret_cast<std::remove_reference_t<decltype(function)>>(
            LoadSymbol(nvml.library, name));
        return function != nullptr;
    };
    if (!load(nvml.Init, "nvmlInit_v2") || !load(nvml.Shutdown, "nvmlShutdown") ||
        !load(nvml.GetCount, "nvmlDeviceGetCount_v2") ||
        !load(nvml.GetHandle, "nvmlDeviceGetHandleByIndex_v2") ||
        !load(nvml.GetName, "nvmlDeviceGetName") ||
        !load(nvml.GetUtilization, "nvmlDeviceGetUtilizationRates") ||
        !load(nvml.GetClock, "nvmlDeviceGetClockInfo") ||
        !load(nvml.GetMaxClock, "nvmlDeviceGetMaxClockInfo") ||
        !load(nvml.GetTemperature, "nvmlDeviceGetTemperature") ||
        !load(nvml.GetPower, "nvmlDeviceGetPowerUsage") ||
        !load(nvml.GetReasons, "nvmlDeviceGetCurrentClocksThrottleReasons") ||
        !load(nvml.GetMemory, "nvmlDeviceGetMemoryInfo") || nvml.Init() != 0) {
        CloseLibrary(nvml.library);
        nvml.library = nullptr;
        return false;
    }
    unsigned int count = 0;
    nvml.GetCount(&count);
    for (unsigned int i = 0; i < count; ++i) {
        nvmlDevice_t device{};
        char name[96]{};
        if (nvml.GetHandle(i, &device) == 0 && nvml.GetName(device, name, sizeof(name)) == 0 &&
            (model == name || count == 1)) {
            nvml.device = device;
            return true;
        }
    }
    nvml.Shutdown();
    CloseLibrary(nvml.library);
    nvml.library = nullptr;
    return false;
}

#ifdef _WIN32
/// The DXGI adapter for this card (by name), for the memory Windows has put where.
IDXGIAdapter3* OpenAdapter(std::string_view model) {
    const HMODULE dxgi = LoadLibraryExW(L"dxgi.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
    if (dxgi == nullptr) {
        return nullptr;
    }
    using CreateFactory = HRESULT(WINAPI*)(REFIID, void**);
    const auto create =
        reinterpret_cast<CreateFactory>(GetProcAddress(dxgi, "CreateDXGIFactory1"));
    IDXGIFactory1* factory{};
    if (create == nullptr || FAILED(create(__uuidof(IDXGIFactory1),
                                           reinterpret_cast<void**>(&factory)))) {
        return nullptr;
    }
    const std::wstring wanted(model.begin(), model.end());
    IDXGIAdapter3* found{};
    IDXGIAdapter1* adapter{};
    for (UINT i = 0; found == nullptr && factory->EnumAdapters1(i, &adapter) != DXGI_ERROR_NOT_FOUND;
         ++i) {
        DXGI_ADAPTER_DESC1 desc{};
        if (SUCCEEDED(adapter->GetDesc1(&desc)) && wanted == desc.Description) {
            adapter->QueryInterface(__uuidof(IDXGIAdapter3), reinterpret_cast<void**>(&found));
        }
        adapter->Release();
    }
    factory->Release();
    return found;
}
#endif

bool Enabled() {
    const char* const value = std::getenv("EDEN_GPU_REPORT");
    return !(value && value[0] == '0');
}

void Run(std::stop_token token, const Vulkan::Device& device) {
    const std::string model{device.GetModelName()};
    Nvml nvml{};
    const bool has_nvml = OpenNvml(nvml, model);
    unsigned int max_clock = 0;
    if (has_nvml) {
        nvml.GetMaxClock(nvml.device, NVML_CLOCK_GRAPHICS, &max_clock);
    }
#ifdef _WIN32
    IDXGIAdapter3* const adapter = OpenAdapter(model);
#endif
    std::mutex mutex;
    std::condition_variable_any wake;
    std::unique_lock lock{mutex};

    u32 samples = 0;
    u64 busy_sum = 0;
    u32 busy_min = 100;
    u64 clock_sum = 0;
    u32 clock_min = 0xFFFFFFFF;
    u32 temperature_max = 0;
    u64 power_sum = 0;
    u32 held_back[std::size(HELD_BACK)]{};
    u32 second = 0;
    while (true) {
        wake.wait_for(lock, token, std::chrono::seconds{1}, [] { return false; });
        if (token.stop_requested()) {
            break;
        }
        if (has_nvml) {
            nvmlUtilization_t utilization{};
            unsigned int clock = 0;
            unsigned int temperature = 0;
            unsigned int power = 0;
            unsigned long long reasons = 0;
            if (nvml.GetUtilization(nvml.device, &utilization) == 0 &&
                nvml.GetClock(nvml.device, NVML_CLOCK_GRAPHICS, &clock) == 0) {
                ++samples;
                busy_sum += utilization.gpu;
                busy_min = (std::min)(busy_min, utilization.gpu);
                clock_sum += clock;
                clock_min = (std::min)(clock_min, clock);
                if (nvml.GetTemperature(nvml.device, NVML_TEMPERATURE_GPU, &temperature) == 0) {
                    temperature_max = (std::max)(temperature_max, temperature);
                }
                if (nvml.GetPower(nvml.device, &power) == 0) {
                    power_sum += power;
                }
                if (nvml.GetReasons(nvml.device, &reasons) == 0) {
                    for (std::size_t i = 0; i < std::size(HELD_BACK); ++i) {
                        held_back[i] += (reasons & HELD_BACK[i].first) != 0 ? 1 : 0;
                    }
                }
            }
        }
        if (++second < 10) {
            continue;
        }
        second = 0;
        std::string card;
        if (samples != 0) {
            std::string reasons;
            for (std::size_t i = 0; i < std::size(HELD_BACK); ++i) {
                if (held_back[i] != 0) {
                    reasons += fmt::format("{}{} ({} s)", reasons.empty() ? "" : ", ",
                                           HELD_BACK[i].second, held_back[i]);
                }
            }
            nvmlMemory_t memory{};
            const bool has_memory = nvml.GetMemory(nvml.device, &memory) == 0;
            card = fmt::format(
                "busy {}% on average (lowest {}%), clock {} MHz on average (lowest {}, highest "
                "possible {}), up to {} C, {} W; clock held back by: {}; {}",
                busy_sum / samples, busy_min, clock_sum / samples, clock_min, max_clock,
                temperature_max, power_sum / samples / 1000, reasons.empty() ? "nothing" : reasons,
                has_memory ? fmt::format("card memory used by all programs {} of {} MiB; ",
                                         memory.used >> 20, memory.total >> 20)
                           : std::string{});
        }
        const Vulkan::Device::MemoryResidency residency = device.GetMemoryResidency();
        std::string shared;
#ifdef _WIN32
        if (adapter != nullptr) {
            DXGI_QUERY_VIDEO_MEMORY_INFO local{};
            DXGI_QUERY_VIDEO_MEMORY_INFO non_local{};
            if (SUCCEEDED(adapter->QueryVideoMemoryInfo(0, DXGI_MEMORY_SEGMENT_GROUP_LOCAL,
                                                        &local)) &&
                SUCCEEDED(adapter->QueryVideoMemoryInfo(0, DXGI_MEMORY_SEGMENT_GROUP_NON_LOCAL,
                                                        &non_local))) {
                shared = fmt::format("; Windows counts {} MiB of ours on the card (budget {} "
                                     "MiB) and {} MiB in shared GPU memory",
                                     local.CurrentUsage >> 20, local.Budget >> 20,
                                     non_local.CurrentUsage >> 20);
            }
        }
#endif
        LOG_INFO(Render_Vulkan,
                 "GPU, last 10 s: {}ours: {} MiB allocated on the card ({} MiB counted there by "
                 "the driver), {} MiB in system memory ({} MiB counted){}",
                 card, residency.card_allocated >> 20, residency.card_in_use >> 20,
                 residency.system_allocated >> 20, residency.system_in_use >> 20, shared);
        samples = 0;
        busy_sum = 0;
        busy_min = 100;
        clock_sum = 0;
        clock_min = 0xFFFFFFFF;
        temperature_max = 0;
        power_sum = 0;
        std::fill(std::begin(held_back), std::end(held_back), 0U);
    }
#ifdef _WIN32
    if (adapter != nullptr) {
        adapter->Release();
    }
#endif
    if (has_nvml) {
        nvml.Shutdown();
        CloseLibrary(nvml.library);
    }
}

} // Anonymous namespace

Reporter::Reporter(const Vulkan::Device& device) {
    if (Enabled()) {
        thread = std::jthread{[&device](std::stop_token token) { Run(token, device); }};
    }
}

Reporter::~Reporter() = default;

} // namespace FefwGpuReport
