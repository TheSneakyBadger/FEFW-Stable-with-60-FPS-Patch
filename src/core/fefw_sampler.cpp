// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include "core/fefw_sampler.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <utility>
#include <vector>

#include <fmt/format.h>

#include "common/common_types.h"
#include "common/logging.h"

#ifdef _WIN32
#include <windows.h>
#endif

namespace FefwSampler {

#ifdef _WIN32
namespace {

constexpr std::size_t MAX_CORES = 8;

struct Slot {
    std::atomic<HANDLE> handle{nullptr};
    std::atomic<const void*> current{nullptr};
};

std::array<Slot, MAX_CORES> slots;
std::atomic<const void*> render_thread{nullptr};
thread_local Slot* my_slot = nullptr;
std::once_flag start_once;

bool Enabled() {
    static const bool enabled = [] {
        const char* const value = std::getenv("EDEN_SAMPLER");
        return value == nullptr || value[0] != '0';
    }();
    return enabled;
}

void Run() {
    HMODULE exe = GetModuleHandleW(nullptr);
    std::unordered_map<HMODULE, std::string> module_names;
    std::unordered_map<u64, u64> exe_buckets; // RVA / 64 -> samples
    std::unordered_map<std::string, u64> modules;
    u64 jit = 0;
    u64 in_exe = 0;
    u64 total = 0;
    u64 failed = 0;
    auto window_start = std::chrono::steady_clock::now();

    const auto module_name = [&](HMODULE module) -> const std::string& {
        auto it = module_names.find(module);
        if (it != module_names.end()) {
            return it->second;
        }
        wchar_t path[MAX_PATH]{};
        GetModuleFileNameW(module, path, MAX_PATH);
        std::wstring wide{path};
        const auto slash = wide.find_last_of(L"\\/");
        if (slash != std::wstring::npos) {
            wide = wide.substr(slash + 1);
        }
        std::string narrow;
        for (const wchar_t c : wide) {
            narrow.push_back(c < 128 ? static_cast<char>(c) : '?');
        }
        return module_names.emplace(module, std::move(narrow)).first->second;
    };

    while (true) {
        std::this_thread::sleep_for(std::chrono::milliseconds{2});
        const void* const render = render_thread.load(std::memory_order_relaxed);
        if (render != nullptr) {
            for (Slot& slot : slots) {
                const HANDLE handle = slot.handle.load(std::memory_order_acquire);
                if (handle == nullptr ||
                    slot.current.load(std::memory_order_relaxed) != render) {
                    continue;
                }
                // Nothing that could take a lock between suspending and resuming.
                if (SuspendThread(handle) == static_cast<DWORD>(-1)) {
                    ++failed;
                    continue;
                }
                CONTEXT context{};
                context.ContextFlags = CONTEXT_CONTROL;
                const BOOL ok = GetThreadContext(handle, &context);
                ResumeThread(handle);
                if (!ok) {
                    ++failed;
                    continue;
                }
#if defined(_M_X64)
                const u64 ip = context.Rip;
#elif defined(_M_ARM64)
                const u64 ip = context.Pc;
#else
                const u64 ip = 0;
#endif
                ++total;
                HMODULE module = nullptr;
                if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                                            GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                                        reinterpret_cast<LPCWSTR>(ip), &module) ||
                    module == nullptr) {
                    ++jit;
                } else if (module == exe) {
                    ++in_exe;
                    ++exe_buckets[(ip - reinterpret_cast<u64>(exe)) >> 6];
                } else {
                    ++modules[module_name(module)];
                }
            }
        }
        const auto now = std::chrono::steady_clock::now();
        if (now - window_start < std::chrono::seconds{30}) {
            continue;
        }
        const double seconds = std::chrono::duration<double>(now - window_start).count();
        window_start = now;
        if (total != 0) {
            const auto pct = [total](u64 n) { return static_cast<double>(n) * 100.0 / total; };
            std::vector<std::pair<std::string, u64>> mods(modules.begin(), modules.end());
            std::ranges::sort(mods, [](const auto& a, const auto& b) { return a.second > b.second; });
            std::string mod_text;
            for (std::size_t i = 0; i < mods.size() && i < 8; ++i) {
                mod_text += fmt::format("{}{} {:.1f}%", i ? ", " : "", mods[i].first, pct(mods[i].second));
            }
            std::vector<std::pair<u64, u64>> buckets(exe_buckets.begin(), exe_buckets.end());
            std::ranges::sort(buckets, [](const auto& a, const auto& b) { return a.second > b.second; });
            std::string bucket_text;
            for (std::size_t i = 0; i < buckets.size() && i < 40; ++i) {
                bucket_text += fmt::format("{}{:x} {:.1f}", i ? " " : "", buckets[i].first << 6,
                                           pct(buckets[i].second));
            }
            LOG_INFO(Kernel,
                     "Main thread samples, last {:.0f} s: {} ({} failed); game code compiled by the "
                     "JIT {:.1f}%, Fortune's Eden.exe {:.1f}%; other: {}",
                     seconds, total, failed, pct(jit), pct(in_exe), mod_text.empty() ? "none" : mod_text);
            LOG_INFO(Kernel, "Main thread samples in Fortune's Eden.exe, top by address (RVA %): {}",
                     bucket_text);
        }
        exe_buckets.clear();
        modules.clear();
        jit = in_exe = total = failed = 0;
    }
}

} // Anonymous namespace

void RegisterCoreThread(std::size_t core) {
    if (!Enabled() || core >= MAX_CORES) {
        return;
    }
    HANDLE handle = nullptr;
    if (!DuplicateHandle(GetCurrentProcess(), GetCurrentThread(), GetCurrentProcess(), &handle,
                         THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT | THREAD_QUERY_INFORMATION,
                         FALSE, 0)) {
        return;
    }
    // A new boot replaces the last one's thread; its old handle is left open (a few per boot).
    slots[core].current.store(nullptr, std::memory_order_relaxed);
    slots[core].handle.store(handle, std::memory_order_release);
    my_slot = &slots[core];
    std::call_once(start_once, [] { std::thread{Run}.detach(); });
}

void NoteCurrentThread(const void* thread) {
    if (my_slot != nullptr) {
        my_slot->current.store(thread, std::memory_order_relaxed);
    }
}

void SetRenderThread(const void* thread) {
    render_thread.store(thread, std::memory_order_relaxed);
}

#else

void RegisterCoreThread(std::size_t) {}
void NoteCurrentThread(const void*) {}
void SetRenderThread(const void*) {}

#endif

} // namespace FefwSampler
