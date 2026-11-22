// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include "core/fefw_sampler.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cstdio>
#include <chrono>
#include <cstdlib>
#include <mutex>
#include <string>
#include <map>
#include <thread>
#include <unordered_map>
#include <utility>
#include <vector>

#include <fmt/format.h>

#include "dynarmic/interface/fefw_jit_stats.h"

#include "common/common_types.h"
#include "common/fs/path_util.h"
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

// Test 94b: host code range -> guest PC for every compiled block, so JIT samples can be
// attributed to game code. Ranges reused after a cache clear replace the old entries.
std::mutex blocks_mutex;
std::map<u64, std::pair<u64, u64>> blocks; // host begin -> (host end, guest PC)

void OnBlock(const void* begin_ptr, const void* end_ptr, std::uint64_t location) {
    const u64 begin = reinterpret_cast<u64>(begin_ptr);
    const u64 end = reinterpret_cast<u64>(end_ptr);
    if (end <= begin) {
        return;
    }
    const u64 pc = location & ((u64{1} << 56) - 1);
    std::scoped_lock lock{blocks_mutex};
    auto it = blocks.lower_bound(begin);
    if (it != blocks.begin()) {
        auto prev = std::prev(it);
        if (prev->second.first > begin) {
            it = prev;
        }
    }
    while (it != blocks.end() && it->first < end) {
        it = blocks.erase(it);
    }
    blocks.emplace(begin, std::make_pair(end, pc));
}

struct BlockHit {
    u64 begin = 0;
    u64 end = 0;
    u64 pc = 0;
};

BlockHit BlockOf(u64 ip) {
    std::scoped_lock lock{blocks_mutex};
    auto it = blocks.upper_bound(ip);
    if (it == blocks.begin()) {
        return {};
    }
    --it;
    if (ip >= it->second.first) {
        return {};
    }
    return {it->first, it->second.first, it->second.second};
}

// Test 94c: per block, where in its PC code the samples land; and samples outside any block (the
// recompiler's slow paths and helpers). Written to log/jit-hot-blocks.txt every 30 s.
struct BlockSamples {
    u64 end = 0;
    u64 pc = 0;
    u64 samples = 0;
    std::unordered_map<u32, u32> offsets;
};

bool Readable(const void* ptr, std::size_t size) {
    MEMORY_BASIC_INFORMATION info{};
    if (VirtualQuery(ptr, &info, sizeof(info)) == 0 || info.State != MEM_COMMIT ||
        (info.Protect & (PAGE_NOACCESS | PAGE_GUARD)) != 0) {
        return false;
    }
    const auto end = reinterpret_cast<const u8*>(info.BaseAddress) + info.RegionSize;
    return reinterpret_cast<const u8*>(ptr) + size <= end;
}

void WalkGuestStack(u64 state, u64 memory_base, std::unordered_map<u64, u64>& callers, u64& stacks,
                    std::map<std::vector<u64>, u64>& paths) {
    if (state == 0 || memory_base == 0) {
        return;
    }
    const HANDLE self = GetCurrentProcess();
    const auto read = [&](u64 address, u64& value) {
        SIZE_T done = 0;
        return ReadProcessMemory(self, reinterpret_cast<const void*>(address), &value, sizeof(value),
                                 &done) != 0 &&
               done == sizeof(value);
    };
    u64 fp = 0;
    u64 lr = 0;
    if (!read(state + 0xe8, fp) || !read(state + 0xf0, lr)) {
        return;
    }
    ++stacks;
    std::array<u64, 24> seen{};
    std::size_t count = 0;
    const auto add = [&](u64 address) {
        address &= (u64{1} << 48) - 1;
        if (address == 0) {
            return;
        }
        for (std::size_t i = 0; i < count; ++i) {
            if (seen[i] == address) {
                return;
            }
        }
        if (count < seen.size()) {
            seen[count++] = address;
            ++callers[address];
        }
    };
    std::vector<u64> path;
    path.push_back(lr & ((u64{1} << 48) - 1));
    add(lr);
    for (int depth = 0; depth < 20 && fp != 0 && (fp & 0xf) == 0; ++depth) {
        u64 next = 0;
        u64 ret = 0;
        if (!read(memory_base + fp, next) || !read(memory_base + fp + 8, ret)) {
            break;
        }
        add(ret);
        path.push_back(ret & ((u64{1} << 48) - 1));
        if (next <= fp) {
            break;
        }
        fp = next;
    }
    ++paths[path];
}

void WriteHotBlocks(const std::unordered_map<u64, BlockSamples>& hot,
                    const std::unordered_map<u64, u64>& outside, u64 total,
                    const std::unordered_map<u64, u64>& callers, u64 stacks,
                    const std::map<std::vector<u64>, u64>& paths, int window) {
    const auto path = Common::FS::GetEdenPath(Common::FS::EdenPath::LogDir) /
                      fmt::format("jit-hot-blocks-{}.txt", window);
    std::FILE* const out = std::fopen(path.string().c_str(), "w");
    if (out == nullptr) {
        return;
    }
    std::fprintf(out, "Main thread samples: %llu, with a game call stack: %llu\n",
                 static_cast<unsigned long long>(total), static_cast<unsigned long long>(stacks));
    {
        std::vector<std::pair<u64, u64>> list(callers.begin(), callers.end());
        std::ranges::sort(list, [](const auto& a, const auto& b) { return a.second > b.second; });
        std::fprintf(out, "CALLERS");
        for (std::size_t i = 0; i < list.size() && i < 3000; ++i) {
            std::fprintf(out, " %llx:%llu", static_cast<unsigned long long>(list[i].first),
                         static_cast<unsigned long long>(list[i].second));
        }
        std::fprintf(out, "\n");
    }
    {
        std::vector<std::pair<const std::vector<u64>*, u64>> list;
        for (const auto& [stack, n] : paths) {
            list.emplace_back(&stack, n);
        }
        std::ranges::sort(list, [](const auto& a, const auto& b) { return a.second > b.second; });
        for (std::size_t i = 0; i < list.size() && i < 4000; ++i) {
            std::fprintf(out, "STACK %llu", static_cast<unsigned long long>(list[i].second));
            for (const u64 frame : *list[i].first) {
                std::fprintf(out, " %llx", static_cast<unsigned long long>(frame));
            }
            std::fprintf(out, "\n");
        }
    }
    std::vector<std::pair<u64, const BlockSamples*>> order;
    for (const auto& [begin, stats] : hot) {
        order.emplace_back(begin, &stats);
    }
    std::ranges::sort(order, [](const auto& a, const auto& b) { return a.second->samples > b.second->samples; });
    for (std::size_t i = 0; i < order.size() && i < 40; ++i) {
        const u64 begin = order[i].first;
        const BlockSamples& stats = *order[i].second;
        const u64 size = std::min<u64>(stats.end - begin, 16384);
        std::fprintf(out, "BLOCK guest %llx host %llx size %llu samples %llu\n",
                     static_cast<unsigned long long>(stats.pc), static_cast<unsigned long long>(begin),
                     static_cast<unsigned long long>(stats.end - begin),
                     static_cast<unsigned long long>(stats.samples));
        std::vector<std::pair<u32, u32>> offs(stats.offsets.begin(), stats.offsets.end());
        std::ranges::sort(offs, [](const auto& a, const auto& b) { return a.first < b.first; });
        std::fprintf(out, "OFFSETS");
        for (const auto& [off, n] : offs) {
            std::fprintf(out, " %x:%u", off, n);
        }
        std::fprintf(out, "\nCODE ");
        const auto* const code = reinterpret_cast<const u8*>(begin);
        const bool readable = Readable(code, size);
        for (u64 k = 0; readable && k < size; ++k) {
            std::fprintf(out, "%02x", code[k]);
        }
        std::fprintf(out, "\n");
    }
    std::vector<std::pair<u64, u64>> regions(outside.begin(), outside.end());
    std::ranges::sort(regions, [](const auto& a, const auto& b) { return a.second > b.second; });
    for (std::size_t i = 0; i < regions.size() && i < 30; ++i) {
        const u64 base = regions[i].first << 8;
        std::fprintf(out, "OUTSIDE host %llx samples %llu\nCODE ", static_cast<unsigned long long>(base),
                     static_cast<unsigned long long>(regions[i].second));
        const auto* const code = reinterpret_cast<const u8*>(base);
        const bool readable = Readable(code, 512);
        for (u64 k = 0; readable && k < 512; ++k) {
            std::fprintf(out, "%02x", code[k]);
        }
        std::fprintf(out, "\n");
    }
    std::fclose(out);
}

void Run() {
    HMODULE exe = GetModuleHandleW(nullptr);
    std::unordered_map<HMODULE, std::string> module_names;
    std::unordered_map<u64, u64> exe_buckets; // RVA / 64 -> samples
    std::unordered_map<std::string, u64> modules;
    std::unordered_map<u64, u64> guest_buckets; // guest PC / 64 -> samples
    std::unordered_map<u64, BlockSamples> hot_blocks; // host block start -> samples
    std::unordered_map<u64, u64> outside_blocks;      // host address / 256 -> samples
    std::unordered_map<u64, u64> callers;             // guest return address -> samples (inclusive)
    std::map<std::vector<u64>, u64> paths;            // whole call stacks -> samples
    int window = 0;
    u64 stacks = 0;
    u64 jit_resolved = 0;
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
                context.ContextFlags = CONTEXT_CONTROL | CONTEXT_INTEGER;
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
                    if (const BlockHit hit = BlockOf(ip); hit.begin != 0) {
                        ++jit_resolved;
                        ++guest_buckets[hit.pc >> 6];
                        BlockSamples& stats = hot_blocks[hit.begin];
                        stats.end = hit.end;
                        stats.pc = hit.pc;
                        ++stats.samples;
                        ++stats.offsets[static_cast<u32>(ip - hit.begin)];
#if defined(_M_X64)
                        // Test 94d: the game's call stack. In recompiled code R15 holds the
                        // emulated CPU state (X29 at +0xe8, X30 at +0xf0) and R13 the base of
                        // the game's memory; follow the frame records (fp, lr) from X29.
                        WalkGuestStack(context.R15, context.R13, callers, stacks, paths);
#endif
                    } else {
                        ++outside_blocks[ip >> 8];
                    }
                } else if (module == exe) {
                    ++in_exe;
                    ++exe_buckets[(ip - reinterpret_cast<u64>(exe)) >> 6];
                } else {
                    ++modules[module_name(module)];
                }
            }
        }
        const auto now = std::chrono::steady_clock::now();
        if (now - window_start < std::chrono::seconds{20}) {
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
            std::vector<std::pair<u64, u64>> guest(guest_buckets.begin(), guest_buckets.end());
            std::ranges::sort(guest, [](const auto& a, const auto& b) { return a.second > b.second; });
            std::string guest_text;
            for (std::size_t i = 0; i < guest.size() && i < 80; ++i) {
                guest_text += fmt::format("{}{:x} {:.2f}", i ? " " : "", guest[i].first << 6,
                                          pct(guest[i].second));
            }
            LOG_INFO(Kernel,
                     "Main thread samples in game code ({:.1f}% of samples matched to a block), top "
                     "by guest address (64-byte blocks, %): {}",
                     pct(jit_resolved), guest_text);
        }
        if (total != 0) {
            // Blocks freed since they were sampled may have been reused; good enough for a test.
            std::scoped_lock lock{blocks_mutex};
            WriteHotBlocks(hot_blocks, outside_blocks, total, callers, stacks, paths, window++ % 20);
        }
        hot_blocks.clear();
        outside_blocks.clear();
        callers.clear();
        paths.clear();
        stacks = 0;
        guest_buckets.clear();
        jit_resolved = 0;
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
    std::call_once(start_once, [] {
        Dynarmic::FefwJitStats::BlockRegistered().store(&OnBlock, std::memory_order_relaxed);
        std::thread{Run}.detach();
    });
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
