// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include "video_core/fefw_texture_disk_cache.h"

#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <cstring>
#include <deque>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <thread>
#include <unordered_map>
#include <unordered_set>

#include <fmt/format.h>

#include "common/cityhash.h"
#include "common/fs/path_util.h"
#include "common/logging.h"
#include "common/settings.h"
#include "common/zstd_compression.h"

#ifdef _WIN32
#include <windows.h>
#include <winioctl.h>
#else
#include <climits>
#include <cstdlib>
#include <sys/stat.h>
#include <sys/sysmacros.h>
#endif

namespace FefwTextureDiskCache {
namespace {

namespace fs = std::filesystem;

/// Bumped whenever what the GPU encodes changes (the BC encoders, the ASTC decoder), so entries
/// from an older build are never used.
constexpr u32 VERSION = 1;
constexpr u32 MAGIC = 0x43544546; // "FETC"
/// What may wait to be written: more is dropped (and stored the next time it's converted).
constexpr u64 MAX_QUEUED_BYTES = 256ULL << 20;

struct Header {
    u32 magic;
    u32 version;
    Key key;
    u64 raw_size;
    u64 compressed_size;
};

struct Entry {
    u64 size;      ///< Bytes on disk.
    u64 last_used; ///< Larger is newer.
};

struct Job {
    Key key;
    u64 name;
    std::vector<u8> data;
};

std::mutex mutex;
std::condition_variable wake;
bool enabled = false;
bool stopping = false;
u64 limit_bytes = 0;
fs::path folder;
std::unordered_map<u64, Entry> index;
u64 total_bytes = 0;
u64 use_counter = 0;
std::deque<Job> queue;
std::unordered_set<u64> queued;
u64 queued_bytes = 0;
std::thread worker;
std::atomic<u32> loaded_count{0};
std::atomic<u32> stored_count{0};

u64 NameOf(const Key& key) {
    static_assert(sizeof(Key) == 32, "Key has no padding (it is hashed and compared as bytes)");
    return Common::CityHash64WithSeed(reinterpret_cast<const char*>(&key), sizeof(Key), VERSION);
}

fs::path PathOf(u64 name) {
    return folder / fmt::format("{:016x}.tex", name);
}

bool SameKey(const Key& a, const Key& b) {
    return std::memcmp(&a, &b, sizeof(Key)) == 0;
}

/// With the mutex held: drops the least recently used entries until the cache fits.
void TrimLocked() {
    while (total_bytes > limit_bytes && !index.empty()) {
        auto oldest = index.begin();
        for (auto it = index.begin(); it != index.end(); ++it) {
            if (it->second.last_used < oldest->second.last_used) {
                oldest = it;
            }
        }
        std::error_code ec;
        fs::remove(PathOf(oldest->first), ec);
        total_bytes -= (std::min)(total_bytes, oldest->second.size);
        index.erase(oldest);
    }
}

void WorkerLoop() {
    std::unique_lock lock{mutex};
    while (true) {
        wake.wait(lock, [] { return stopping || !queue.empty(); });
        if (queue.empty()) {
            return;
        }
        Job job = std::move(queue.front());
        queue.pop_front();
        queued_bytes -= (std::min)(queued_bytes, static_cast<u64>(job.data.size()));
        lock.unlock();

        const std::vector<u8> compressed =
            Common::Compression::CompressDataZSTD(job.data.data(), job.data.size(), 3);
        const Header header{
            .magic = MAGIC,
            .version = VERSION,
            .key = job.key,
            .raw_size = job.data.size(),
            .compressed_size = compressed.size(),
        };
        const fs::path path = PathOf(job.name);
        fs::path temp = path;
        temp += ".tmp";
        bool written = false;
        if (!compressed.empty()) {
            std::ofstream file{temp, std::ios::binary | std::ios::trunc};
            file.write(reinterpret_cast<const char*>(&header), sizeof(header));
            file.write(reinterpret_cast<const char*>(compressed.data()),
                       static_cast<std::streamsize>(compressed.size()));
            written = file.good();
        }
        std::error_code ec;
        if (written) {
            fs::rename(temp, path, ec);
            written = !ec;
        }
        if (!written) {
            fs::remove(temp, ec);
        }

        lock.lock();
        queued.erase(job.name);
        if (written) {
            const u64 size = sizeof(header) + compressed.size();
            auto [it, inserted] = index.try_emplace(job.name, Entry{size, ++use_counter});
            if (!inserted) {
                total_bytes -= (std::min)(total_bytes, it->second.size);
                it->second = Entry{size, ++use_counter};
            }
            total_bytes += size;
            ++stored_count;
            TrimLocked();
        }
    }
}

} // Anonymous namespace

bool OnHardDrive(const std::filesystem::path& folder) {
#ifdef _WIN32
    // The volume the folder is on, and whether its disk has a seek penalty (spins).
    wchar_t volume[MAX_PATH]{};
    if (!GetVolumePathNameW(folder.c_str(), volume, MAX_PATH)) {
        return false;
    }
    std::wstring device = volume;
    if (!device.empty() && device.back() == L'\\') {
        device.pop_back();
    }
    if (device.size() != 2 || device[1] != L':') {
        return false; // A mounted folder or a network share: assume it doesn't spin.
    }
    device = L"\\\\.\\" + device;
    const HANDLE handle = CreateFileW(device.c_str(), 0, FILE_SHARE_READ | FILE_SHARE_WRITE,
                                      nullptr, OPEN_EXISTING, 0, nullptr);
    if (handle == INVALID_HANDLE_VALUE) {
        return false;
    }
    STORAGE_PROPERTY_QUERY query{};
    query.PropertyId = StorageDeviceSeekPenaltyProperty;
    query.QueryType = PropertyStandardQuery;
    DEVICE_SEEK_PENALTY_DESCRIPTOR penalty{};
    DWORD returned = 0;
    const bool known =
        DeviceIoControl(handle, IOCTL_STORAGE_QUERY_PROPERTY, &query, sizeof(query), &penalty,
                        sizeof(penalty), &returned, nullptr) &&
        returned >= sizeof(penalty);
    CloseHandle(handle);
    return known && penalty.IncursSeekPenalty;
#else
    // /sys/dev/block/MAJOR:MINOR is the disk, or a partition inside the disk's folder.
    struct stat info {};
    std::error_code ec;
    const fs::path existing = fs::exists(folder, ec) ? folder : folder.parent_path();
    if (stat(existing.c_str(), &info) != 0) {
        return false;
    }
    const fs::path block = fmt::format("/sys/dev/block/{}:{}", major(info.st_dev),
                                       minor(info.st_dev));
    const fs::path real = fs::canonical(block, ec);
    if (ec) {
        return false;
    }
    for (const fs::path& dir : {real, real.parent_path()}) {
        std::ifstream file{dir / "queue" / "rotational"};
        int rotational = 0;
        if (file >> rotational) {
            return rotational == 1;
        }
    }
    return false;
#endif
}

void Start() {
    Stop();
    u64 limit = 0;
    switch (Settings::values.texture_disk_cache.GetValue()) {
    case Settings::TextureDiskCache::Gb2:
        limit = 2ULL << 30;
        break;
    case Settings::TextureDiskCache::Gb4:
        limit = 4ULL << 30;
        break;
    case Settings::TextureDiskCache::Gb8:
        limit = 8ULL << 30;
        break;
    default:
        break;
    }
    std::scoped_lock lock{mutex};
    index.clear();
    total_bytes = 0;
    use_counter = 0;
    enabled = false;
    if (limit == 0) {
        LOG_INFO(Render_Vulkan, "Texture cache on disk: off");
        return;
    }
    folder = Common::FS::GetEdenPath(Common::FS::EdenPath::CacheDir) / "textures";
    std::error_code ec;
    fs::create_directories(folder, ec);
    if (ec) {
        LOG_WARNING(Render_Vulkan, "Texture cache on disk: can't create {}: {}",
                    folder.string(), ec.message());
        return;
    }
    // On a hard drive its reads and writes would slow the game's own (its maps, its movies), for
    // freezes of seconds: the cache is off there.
    if (OnHardDrive(folder)) {
        LOG_INFO(Render_Vulkan, "Texture cache on disk: off, {} is on a hard drive",
                 folder.string());
        return;
    }
    // Oldest first by the time each was last written or used, so the order survives restarts.
    std::vector<std::pair<fs::file_time_type, std::pair<u64, u64>>> found;
    for (const auto& item : fs::directory_iterator{folder, ec}) {
        const fs::path& path = item.path();
        if (path.extension() == ".tmp") {
            fs::remove(path, ec);
            continue;
        }
        if (path.extension() != ".tex" || !item.is_regular_file(ec)) {
            continue;
        }
        const std::string stem = path.stem().string();
        if (stem.size() != 16) {
            continue;
        }
        const u64 name = std::strtoull(stem.c_str(), nullptr, 16);
        found.push_back({item.last_write_time(ec), {name, item.file_size(ec)}});
    }
    std::ranges::sort(found, [](const auto& a, const auto& b) { return a.first < b.first; });
    for (const auto& [time, entry] : found) {
        index.emplace(entry.first, Entry{entry.second, ++use_counter});
        total_bytes += entry.second;
    }
    limit_bytes = limit;
    TrimLocked();
    enabled = true;
    stopping = false;
    worker = std::thread{WorkerLoop};
    LOG_INFO(Render_Vulkan, "Texture cache on disk: {} textures, {} MiB of {} MiB", index.size(),
             total_bytes >> 20, limit >> 20);
}

void Stop() {
    {
        std::scoped_lock lock{mutex};
        if (!worker.joinable()) {
            return;
        }
        stopping = true;
    }
    wake.notify_all();
    worker.join();
    std::scoped_lock lock{mutex};
    enabled = false;
    stopping = false;
}

bool Enabled() {
    std::scoped_lock lock{mutex};
    return enabled;
}

bool Contains(const Key& key) {
    const u64 name = NameOf(key);
    std::scoped_lock lock{mutex};
    return enabled && index.contains(name);
}

std::optional<std::vector<u8>> Load(const Key& key, u64 size) {
    const u64 name = NameOf(key);
    fs::path path;
    {
        std::scoped_lock lock{mutex};
        if (!enabled || !index.contains(name)) {
            return std::nullopt;
        }
        path = PathOf(name);
    }
    std::vector<u8> result;
    bool good = false;
    {
        std::ifstream file{path, std::ios::binary};
        Header header{};
        if (file.read(reinterpret_cast<char*>(&header), sizeof(header)) &&
            header.magic == MAGIC && header.version == VERSION && SameKey(header.key, key) &&
            header.raw_size == size && header.compressed_size < (1ULL << 31)) {
            std::vector<u8> compressed(header.compressed_size);
            if (file.read(reinterpret_cast<char*>(compressed.data()),
                          static_cast<std::streamsize>(compressed.size()))) {
                result.resize(size);
                good = Common::Compression::DecompressDataZSTD(compressed, result) == size;
            }
        }
    }
    std::scoped_lock lock{mutex};
    const auto it = index.find(name);
    if (!good) {
        // Damaged, or from another build: converted again (and stored again) instead.
        if (it != index.end()) {
            total_bytes -= (std::min)(total_bytes, it->second.size);
            index.erase(it);
        }
        std::error_code ec;
        fs::remove(path, ec);
        return std::nullopt;
    }
    if (it != index.end()) {
        it->second.last_used = ++use_counter;
    }
    std::error_code ec;
    fs::last_write_time(path, fs::file_time_type::clock::now(), ec);
    ++loaded_count;
    return result;
}

void Store(const Key& key, std::vector<u8> data) {
    const u64 name = NameOf(key);
    {
        std::scoped_lock lock{mutex};
        if (!enabled || index.contains(name) || queued.contains(name) ||
            queued_bytes + data.size() > MAX_QUEUED_BYTES) {
            return;
        }
        queued.insert(name);
        queued_bytes += data.size();
        queue.push_back(Job{key, name, std::move(data)});
    }
    wake.notify_one();
}

Counts TakeCounts() {
    std::scoped_lock lock{mutex};
    return Counts{loaded_count.exchange(0), stored_count.exchange(0), total_bytes};
}

} // namespace FefwTextureDiskCache
