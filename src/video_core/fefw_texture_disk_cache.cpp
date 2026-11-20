// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include "video_core/fefw_texture_disk_cache.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <thread>
#include <unordered_map>
#include <unordered_set>

#include <bc_decoder.h>
#include <fmt/format.h>

#include "common/cityhash.h"
#include "common/fs/path_util.h"
#include "common/logging.h"
#include "common/settings.h"
#include "common/zstd_compression.h"
#include "video_core/gpu_split.h"

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
    bool used{};   ///< Loaded this run: its file's time is brought up to date when it stops.
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
std::thread color_pass;
std::atomic<u32> loaded_count{0};
std::atomic<u64> read_ns{0};
std::atomic<u64> read_max_ns{0};
std::atomic<u32> stored_count{0};
/// Stand-in colours by entry name (StandInColor), kept in standins.bin between runs.
std::unordered_map<u64, u32> colors;
/// Colours added since standins.bin was last written.
bool colors_dirty = false;
/// The same colours by the game's texture alone (the key without what it was encoded to), the
/// same on every PC: shown while a texture the cache doesn't have yet is converted over the next
/// frames instead of in the frame that needs it. Kept in colours.bin, with the list that comes
/// with Fortune's Eden (texture-colours next to the program) read in too.
std::unordered_map<u64, u32> guest_colors;
constexpr u32 STAND_IN_MAGIC = 0x49534546; // "FESI"
constexpr u32 STAND_IN_VERSION = 1;
constexpr u32 GUEST_COLOR_MAGIC = 0x43474546; // "FEGC"
constexpr std::size_t MAX_GUEST_COLORS = 1U << 21;

u64 NameOf(const Key& key);

u64 GuestNameOf(Key key) {
    key.encoding = 0;
    return NameOf(key);
}

/// Name and colour pairs (u32 magic, u32 version, u32 count, then the pairs).
std::vector<std::pair<u64, u32>> ReadColorFile(const fs::path& path, u32 magic) {
    std::ifstream file{path, std::ios::binary};
    u32 head[3]{};
    if (!file.read(reinterpret_cast<char*>(head), sizeof(head)) || head[0] != magic ||
        head[1] != STAND_IN_VERSION || head[2] > (1U << 24)) {
        return {};
    }
    std::vector<std::pair<u64, u32>> items(head[2]);
    for (auto& [name, color] : items) {
        if (!file.read(reinterpret_cast<char*>(&name), sizeof(name)) ||
            !file.read(reinterpret_cast<char*>(&color), sizeof(color))) {
            return {};
        }
    }
    return items;
}

bool WriteColorFile(const fs::path& path, u32 magic,
                    const std::vector<std::pair<u64, u32>>& items) {
    fs::path temp = path;
    temp += ".tmp";
    bool written = false;
    {
        std::ofstream file{temp, std::ios::binary | std::ios::trunc};
        const u32 head[3]{magic, STAND_IN_VERSION, static_cast<u32>(items.size())};
        file.write(reinterpret_cast<const char*>(head), sizeof(head));
        for (const auto& [name, color] : items) {
            file.write(reinterpret_cast<const char*>(&name), sizeof(name));
            file.write(reinterpret_cast<const char*>(&color), sizeof(color));
        }
        written = file.good();
    }
    std::error_code ec;
    if (written) {
        fs::rename(temp, path, ec);
        return !ec;
    }
    fs::remove(temp, ec);
    return false;
}

fs::path ShippedColorPath() {
#ifdef _WIN32
    return Common::FS::GetExeDirectory() / "texture-colours" / "colours.bin";
#else
    std::error_code ec;
    const fs::path exe = fs::read_symlink("/proc/self/exe", ec);
    return ec ? fs::path{} : exe.parent_path() / "texture-colours" / "colours.bin";
#endif
}

/// With the mutex held: records a colour by entry and by game texture.
void RecordColorLocked(u64 name, const Key& key, u32 color) {
    colors.insert_or_assign(name, color);
    if (guest_colors.size() < MAX_GUEST_COLORS) {
        guest_colors.insert_or_assign(GuestNameOf(key), color);
    }
    colors_dirty = true;
}

bool StandInsAllowed() {
    static const bool allowed = [] {
        const char* const value = std::getenv("EDEN_TC_STAND_IN");
        return !(value && value[0] == '0');
    }();
    return allowed;
}

/// The average colour of the texture's smallest level (its first layer), from up to 8x8 of
/// its blocks: alpha 255 where every texel looked at is opaque, else 0 (a cut-out, leaves or
/// hair: hidden for a moment rather than drawn as a solid card).
std::optional<u32> ComputeColor(const Key& key, std::span<const u8> data) {
    if (key.width == 0 || key.height == 0 || key.levels == 0 || key.layers == 0) {
        return std::nullopt;
    }
    const u64 block_bytes = key.encoding == 1 ? 8 : 16;
    u64 offset = 0;
    u32 level_width = 0;
    u32 level_height = 0;
    u32 blocks_x = 0;
    u32 blocks_y = 0;
    for (u32 level = 0; level < key.levels; ++level) {
        level_width = (std::max)(key.width >> level, 1U);
        level_height = (std::max)(key.height >> level, 1U);
        blocks_x = (level_width + 3) / 4;
        blocks_y = (level_height + 3) / 4;
        if (level + 1 < key.levels) {
            offset += u64{blocks_x} * blocks_y * key.layers * block_bytes;
        }
    }
    if (offset + u64{blocks_x} * blocks_y * block_bytes > data.size()) {
        return std::nullopt;
    }
    const u32 step_x = (std::max)(blocks_x / 8, 1U);
    const u32 step_y = (std::max)(blocks_y / 8, 1U);
    u64 sum[4]{};
    u64 count = 0;
    u32 min_alpha = 255;
    std::array<u8, 64> texels{};
    for (u32 by = 0; by < blocks_y; by += step_y) {
        for (u32 bx = 0; bx < blocks_x; bx += step_x) {
            const u8* const block =
                data.data() + offset + (u64{by} * blocks_x + bx) * block_bytes;
            switch (key.encoding) {
            case 1:
                bcn::DecodeBc1(block, texels.data(), 0, 0, 4, 4);
                break;
            case 3:
                bcn::DecodeBc3(block, texels.data(), 0, 0, 4, 4);
                break;
            case 7:
                bcn::DecodeBc7(block, texels.data(), 0, 0, 4, 4);
                break;
            default:
                return std::nullopt;
            }
            const u32 valid_x = (std::min)(4U, level_width - bx * 4);
            const u32 valid_y = (std::min)(4U, level_height - by * 4);
            for (u32 y = 0; y < valid_y; ++y) {
                for (u32 x = 0; x < valid_x; ++x) {
                    const u8* const texel = texels.data() + (y * 4 + x) * 4;
                    for (int c = 0; c < 4; ++c) {
                        sum[c] += texel[c];
                    }
                    min_alpha = (std::min)(min_alpha, u32{texel[3]});
                    ++count;
                }
            }
        }
    }
    if (count == 0) {
        return std::nullopt;
    }
    const auto average = [&](int c) { return static_cast<u32>((sum[c] + count / 2) / count); };
    const u32 alpha = min_alpha >= 250 ? 255U : 0U;
    return average(0) | (average(1) << 8) | (average(2) << 16) | (alpha << 24);
}

fs::path StandInPath() {
    return folder / "standins.bin";
}

/// With the mutex held, after the index is read: the colours of the entries it has, and the
/// colours by game texture (this install's, then the list that comes with Fortune's Eden).
void LoadStandInsLocked() {
    colors.clear();
    for (const auto& [name, color] : ReadColorFile(StandInPath(), STAND_IN_MAGIC)) {
        if (index.contains(name)) {
            colors.emplace(name, color);
        }
    }
    guest_colors.clear();
    for (const auto& [name, color] : ReadColorFile(folder / "colours.bin", GUEST_COLOR_MAGIC)) {
        guest_colors.emplace(name, color);
    }
    const std::size_t own = guest_colors.size();
    if (const fs::path shipped = ShippedColorPath(); !shipped.empty()) {
        for (const auto& [name, color] : ReadColorFile(shipped, GUEST_COLOR_MAGIC)) {
            if (guest_colors.size() >= MAX_GUEST_COLORS) {
                break;
            }
            guest_colors.emplace(name, color);
        }
    }
    LOG_INFO(Render_Vulkan,
             "Texture colours: {} from this install, {} more from the list that comes with "
             "Fortune's Eden",
             own, guest_colors.size() - own);
}

/// With the mutex held: writes the colours of the entries there are, and by game texture.
void SaveStandInsLocked() {
    if (folder.empty()) {
        return;
    }
    std::vector<std::pair<u64, u32>> items;
    items.reserve(colors.size());
    for (const auto& [name, color] : colors) {
        if (index.contains(name)) {
            items.emplace_back(name, color);
        }
    }
    const std::vector<std::pair<u64, u32>> by_texture(guest_colors.begin(), guest_colors.end());
    if (WriteColorFile(StandInPath(), STAND_IN_MAGIC, items) &&
        WriteColorFile(folder / "colours.bin", GUEST_COLOR_MAGIC, by_texture)) {
        colors_dirty = false;
    }
}

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
        colors.erase(oldest->first);
        index.erase(oldest);
    }
}

/// Once per install, on a thread of its own: the colours of the textures already in the cache,
/// by game texture too. Before v1.7.5.10 they were kept by entry only (and an install whose
/// sessions ended abruptly kept none), so the whole cache counts towards colours.bin (the list
/// shared with other players) at once, not only as each texture is loaded again. An entry with a
/// colour known gives it from its header (its key); one without is read and its colour worked
/// out. colours-by-texture in the folder marks it done.
void ColorPassLoop() {
    const fs::path marker = folder / "colours-by-texture";
    std::error_code ec;
    if (fs::exists(marker, ec)) {
        return;
    }
    std::vector<std::pair<u64, std::optional<u32>>> entries;
    {
        std::scoped_lock lock{mutex};
        entries.reserve(index.size());
        for (const auto& [name, entry] : index) {
            const auto known = colors.find(name);
            entries.emplace_back(name, known != colors.end() ? std::optional<u32>{known->second}
                                                             : std::nullopt);
        }
    }
    const auto start = std::chrono::steady_clock::now();
    std::size_t added = 0;
    std::size_t worked_out = 0;
    std::vector<u8> compressed;
    std::vector<u8> raw;
    for (const auto& [name, known] : entries) {
        {
            std::scoped_lock lock{mutex};
            if (stopping) {
                return;
            }
        }
        std::ifstream file{PathOf(name), std::ios::binary};
        Header header{};
        if (!file.read(reinterpret_cast<char*>(&header), sizeof(header)) ||
            header.magic != MAGIC || header.version != VERSION || NameOf(header.key) != name) {
            continue;
        }
        std::optional<u32> color = known;
        if (!color) {
            if (header.compressed_size >= (1ULL << 31) || header.raw_size >= (1ULL << 31)) {
                continue;
            }
            compressed.resize(header.compressed_size);
            raw.resize(header.raw_size);
            if (!file.read(reinterpret_cast<char*>(compressed.data()),
                           static_cast<std::streamsize>(compressed.size())) ||
                Common::Compression::DecompressDataZSTD(compressed, raw) != raw.size()) {
                continue;
            }
            color = ComputeColor(header.key, raw);
            if (!color) {
                continue;
            }
            ++worked_out;
            // A short pause after each read and decode: this runs while the game does.
            std::this_thread::sleep_for(std::chrono::milliseconds{1});
        }
        std::scoped_lock lock{mutex};
        if (!index.contains(name)) {
            continue;
        }
        if (!known) {
            colors.try_emplace(name, *color);
        }
        if (guest_colors.size() < MAX_GUEST_COLORS &&
            guest_colors.try_emplace(GuestNameOf(header.key), *color).second) {
            ++added;
        }
        colors_dirty = true;
    }
    std::scoped_lock lock{mutex};
    if (stopping) {
        return;
    }
    SaveStandInsLocked();
    std::ofstream{marker, std::ios::trunc} << "1\n";
    LOG_INFO(Render_Vulkan,
             "Texture colours: {} added from the {} textures already in the cache ({} of them "
             "read to work out their colour) in {} s",
             added, entries.size(), worked_out,
             std::chrono::duration_cast<std::chrono::seconds>(std::chrono::steady_clock::now() -
                                                              start)
                 .count());
}

void WorkerLoop() {
    std::unique_lock lock{mutex};
    while (true) {
        // The stand-in colours are written every minute they've changed, not only when the
        // renderer stops: sessions that ended with Fortune's Eden closing abruptly kept none, so
        // every session on that install was a first one (#63).
        if (!wake.wait_for(lock, std::chrono::minutes{1},
                           [] { return stopping || !queue.empty(); })) {
            if (colors_dirty) {
                SaveStandInsLocked();
            }
            continue;
        }
        if (queue.empty()) {
            return;
        }
        Job job = std::move(queue.front());
        queue.pop_front();
        queued_bytes -= (std::min)(queued_bytes, static_cast<u64>(job.data.size()));
        lock.unlock();

        const std::optional<u32> color = ComputeColor(job.key, job.data);
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
            if (color) {
                RecordColorLocked(job.name, job.key, *color);
            }
            ++stored_count;
            TrimLocked();
        }
    }
}

} // Anonymous namespace

bool OnHardDrive(const std::filesystem::path& cache_folder) {
#ifdef _WIN32
    // The volume the folder is on, and whether its disk has a seek penalty (spins).
    wchar_t volume[MAX_PATH]{};
    if (!GetVolumePathNameW(cache_folder.c_str(), volume, MAX_PATH)) {
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
    const fs::path existing = fs::exists(cache_folder, ec) ? cache_folder
                                                         : cache_folder.parent_path();
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

u32 ReadThreads() {
    static const u32 threads = [] {
        if (const char* value = std::getenv("EDEN_DISK_READERS"); value && value[0] != '\0') {
            const u32 wanted = static_cast<u32>(std::strtoul(value, nullptr, 10));
            if (wanted >= 1 && wanted <= 16) {
                LOG_INFO(Render_Vulkan, "Texture cache on disk: {} read threads (EDEN_DISK_READERS)",
                         wanted);
                return wanted;
            }
        }
        const u32 fast_cores = VideoCommon::FastCoreCount();
        if (fast_cores != 0 && fast_cores <= 4) {
            LOG_INFO(Render_Vulkan,
                     "Texture cache on disk: 1 read thread (this CPU has {} fast cores)",
                     fast_cores);
            return 1U;
        }
        // All but 4 of the CPU's threads (the game's busiest), 2 to 16. Half of them, up to 8,
        // fell behind when a new area needed 600-1,500 textures at once: frames waited 0.5 s in
        // 10 s and 400-760 were converted on the GPU instead (#63, 8 threads on a 16-thread
        // Ryzen 7 9800X3D and a 32-thread i9-14900HX). The readers only work during such bursts,
        // when frames are waiting for them anyway.
        const u32 hardware = std::thread::hardware_concurrency();
        const u32 count = std::clamp(hardware > 4 ? hardware - 4 : 2U, 2U, 16U);
        LOG_INFO(Render_Vulkan, "Texture cache on disk: {} read threads", count);
        return count;
    }();
    return threads;
}

void Start(u64 video_memory) {
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
    colors.clear();
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
    // Too little video memory: textures are freed and reloaded so often that reading them back
    // from disk falls behind converting them on the GPU (#75). Remembered for the home screen.
    const bool small_card = video_memory != 0 && video_memory < SMALL_CARD_MEMORY;
    Settings::values.fefw_texture_cache_small_card.SetValue(small_card);
    if (small_card) {
        const char* const keep = std::getenv("EDEN_TEXTURE_CACHE_SMALL_CARD");
        if (!keep || keep[0] != '1') {
            LOG_INFO(Render_Vulkan,
                     "Texture cache on disk: off, the graphics card has {} MiB of video memory "
                     "to use (under {} MiB; EDEN_TEXTURE_CACHE_SMALL_CARD=1 keeps it on)",
                     video_memory >> 20, SMALL_CARD_MEMORY >> 20);
            return;
        }
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
    LoadStandInsLocked();
    limit_bytes = limit;
    TrimLocked();
    enabled = true;
    stopping = false;
    worker = std::thread{WorkerLoop};
    color_pass = std::thread{ColorPassLoop};
    LOG_INFO(Render_Vulkan,
             "Texture cache on disk: {} textures ({} with a stand-in colour), {} MiB of {} MiB",
             index.size(), colors.size(), total_bytes >> 20, limit >> 20);
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
    if (color_pass.joinable()) {
        color_pass.join();
    }
    std::scoped_lock lock{mutex};
    enabled = false;
    stopping = false;
    SaveStandInsLocked();
    // The entries loaded this run get file times in the order they were last used (after the
    // newest written), so the least recently used go first after a restart. (Loads used to do
    // this one by one, a file system write in each read.)
    std::vector<std::pair<u64, u64>> used;
    for (auto& [name, entry] : index) {
        if (entry.used) {
            used.emplace_back(entry.last_used, name);
            entry.used = false;
        }
    }
    std::ranges::sort(used);
    const auto now = fs::file_time_type::clock::now();
    std::error_code ec;
    for (size_t i = 0; i < used.size(); ++i) {
        fs::last_write_time(PathOf(used[i].second),
                            now + std::chrono::milliseconds{static_cast<s64>(i)}, ec);
    }
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

bool LoadInto(const Key& key, std::span<u8> output) {
    const auto start = std::chrono::steady_clock::now();
    const u64 name = NameOf(key);
    fs::path path;
    {
        std::scoped_lock lock{mutex};
        if (!enabled || !index.contains(name)) {
            return false;
        }
        path = PathOf(name);
    }
    bool good = false;
    {
        thread_local std::vector<u8> compressed;
        std::ifstream file{path, std::ios::binary};
        Header header{};
        if (file.read(reinterpret_cast<char*>(&header), sizeof(header)) &&
            header.magic == MAGIC && header.version == VERSION && SameKey(header.key, key) &&
            header.raw_size == output.size() && header.compressed_size < (1ULL << 31)) {
            compressed.resize(header.compressed_size);
            if (file.read(reinterpret_cast<char*>(compressed.data()),
                          static_cast<std::streamsize>(compressed.size()))) {
                good = Common::Compression::DecompressDataZSTD(compressed, output) ==
                       output.size();
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
        return false;
    }
    if (it != index.end()) {
        it->second.last_used = ++use_counter;
        it->second.used = true;
        if (const auto known = colors.find(name); known != colors.end()) {
            // A colour from before they were kept by game texture too (v1.7.5.10).
            if (guest_colors.size() < MAX_GUEST_COLORS &&
                guest_colors.try_emplace(GuestNameOf(key), known->second).second) {
                colors_dirty = true;
            }
        } else if (const std::optional<u32> color = ComputeColor(key, output)) {
            RecordColorLocked(name, key, *color);
        }
    }
    ++loaded_count;
    const u64 ns = static_cast<u64>(std::chrono::duration_cast<std::chrono::nanoseconds>(
                                        std::chrono::steady_clock::now() - start)
                                        .count());
    read_ns += ns;
    u64 max = read_max_ns.load();
    while (ns > max && !read_max_ns.compare_exchange_weak(max, ns)) {
    }
    return true;
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
    return Counts{loaded_count.exchange(0), stored_count.exchange(0), total_bytes,
                  read_ns.exchange(0), read_max_ns.exchange(0),
                  static_cast<u32>(colors.size())};
}

u64 GuestName(const Key& key) {
    return GuestNameOf(key);
}

std::optional<u32> GuestColor(const Key& key) {
    if (!StandInsAllowed()) {
        return std::nullopt;
    }
    const u64 name = GuestNameOf(key);
    std::scoped_lock lock{mutex};
    if (!enabled) {
        return std::nullopt;
    }
    const auto it = guest_colors.find(name);
    if (it == guest_colors.end()) {
        return std::nullopt;
    }
    return it->second;
}

std::optional<u32> StandInColor(const Key& key) {
    if (!StandInsAllowed()) {
        return std::nullopt;
    }
    const u64 name = NameOf(key);
    std::scoped_lock lock{mutex};
    if (!enabled) {
        return std::nullopt;
    }
    const auto it = colors.find(name);
    if (it == colors.end()) {
        return std::nullopt;
    }
    return it->second;
}

u32 SolidBlock(u32 encoding, u32 rgba, std::span<u8, 16> block) {
    std::ranges::fill(block, u8{0});
    const u32 r = rgba & 0xFF;
    const u32 g = (rgba >> 8) & 0xFF;
    const u32 b = (rgba >> 16) & 0xFF;
    const u32 a = rgba >> 24;
    const auto put16 = [&](size_t at, u32 value) {
        block[at] = static_cast<u8>(value);
        block[at + 1] = static_cast<u8>(value >> 8);
    };
    // RGB565, rounded.
    const u32 rgb565 = (((r * 31 + 127) / 255) << 11) | (((g * 63 + 127) / 255) << 5) |
                       ((b * 31 + 127) / 255);
    if (encoding == 1) {
        // Equal endpoints: the three-colour mode, where index 0 is the colour and 3 is
        // transparent black.
        put16(0, rgb565);
        put16(2, rgb565);
        if (a < 128) {
            std::fill(block.begin() + 4, block.begin() + 8, u8{0xFF});
        }
        return 8;
    }
    if (encoding == 3) {
        block[0] = static_cast<u8>(a);
        block[1] = static_cast<u8>(a);
        put16(8, rgb565);
        put16(10, rgb565);
        return 16;
    }
    // BC7 mode 6: 7-bit RGBA endpoints and a p-bit each, both endpoints the colour, indices 0.
    u32 bit = 0;
    const auto write = [&](u32 value, u32 bits) {
        for (u32 i = 0; i < bits; ++i, ++bit) {
            if ((value >> i) & 1) {
                block[bit / 8] |= static_cast<u8>(1U << (bit % 8));
            }
        }
    };
    write(1U << 6, 7);
    for (const u32 channel : {r, g, b, a}) {
        write(channel >> 1, 7);
        write(channel >> 1, 7);
    }
    // The p-bit is each endpoint's lowest bit for all four channels: alpha's (0 or 255) is
    // exact, the colour within 1.
    write(a & 1, 1);
    write(a & 1, 1);
    return 16;
}

} // namespace FefwTextureDiskCache
