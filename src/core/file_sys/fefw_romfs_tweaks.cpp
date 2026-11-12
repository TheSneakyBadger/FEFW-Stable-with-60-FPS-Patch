// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include "core/file_sys/fefw_romfs_tweaks.h"

#include <bit>
#include <cstring>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "common/logging.h"
#include "common/settings.h"
#include "core/file_sys/vfs/vfs.h"
#include "core/file_sys/vfs/vfs_vector.h"

namespace FileSys::FefwRomfsTweaks {
namespace {

// A leaf of a MessagePack document: its key path, offset and type byte.
struct Leaf {
    std::vector<std::string> path; // map keys; array indices as "#<n>"
    size_t offset;
    u8 type;
};

// Walks a MessagePack document (several top-level values allowed). Returns false on anything it
// doesn't understand, in which case the file is left alone.
class Walker {
public:
    explicit Walker(std::span<const u8> data_) : data{data_} {}

    bool Run(std::vector<Leaf>& out) {
        std::vector<std::string> path;
        while (pos < data.size()) {
            if (!Value(path, out, 0)) {
                return false;
            }
        }
        return true;
    }

private:
    bool Need(size_t n) const {
        return pos + n <= data.size();
    }
    u64 Big(size_t n) {
        u64 v = 0;
        for (size_t i = 0; i < n; ++i) {
            v = (v << 8) | data[pos + i];
        }
        pos += n;
        return v;
    }
    std::optional<std::string> Key() {
        // Keys are short strings in these files.
        if (!Need(1)) {
            return std::nullopt;
        }
        const u8 t = data[pos];
        size_t n;
        if (t >= 0xa0 && t <= 0xbf) {
            n = t & 0x1f;
            pos += 1;
        } else if (t == 0xd9 && Need(2)) {
            n = data[pos + 1];
            pos += 2;
        } else {
            return std::nullopt;
        }
        if (!Need(n)) {
            return std::nullopt;
        }
        std::string key(reinterpret_cast<const char*>(data.data() + pos), n);
        pos += n;
        return key;
    }
    bool Value(std::vector<std::string>& path, std::vector<Leaf>& out, int depth) {
        if (depth > 32 || !Need(1)) {
            return false;
        }
        const size_t at = pos;
        const u8 t = data[pos];
        const auto leaf = [&](size_t size) {
            if (!Need(size)) {
                return false;
            }
            out.push_back({path, at, t});
            pos += size;
            return true;
        };
        if (t <= 0x7f || t >= 0xe0 || t == 0xc0 || t == 0xc2 || t == 0xc3) {
            return leaf(1);
        }
        if ((t >= 0x80 && t <= 0x8f) || t == 0xde || t == 0xdf) {
            size_t n;
            if (t <= 0x8f) {
                n = t & 0xf;
                pos += 1;
            } else {
                const size_t w = t == 0xde ? 2 : 4;
                if (!Need(1 + w)) {
                    return false;
                }
                pos += 1;
                n = static_cast<size_t>(Big(w));
            }
            for (size_t i = 0; i < n; ++i) {
                auto key = Key();
                if (!key) {
                    return false;
                }
                path.push_back(std::move(*key));
                const bool ok = Value(path, out, depth + 1);
                path.pop_back();
                if (!ok) {
                    return false;
                }
            }
            return true;
        }
        if ((t >= 0x90 && t <= 0x9f) || t == 0xdc || t == 0xdd) {
            size_t n;
            if (t <= 0x9f) {
                n = t & 0xf;
                pos += 1;
            } else {
                const size_t w = t == 0xdc ? 2 : 4;
                if (!Need(1 + w)) {
                    return false;
                }
                pos += 1;
                n = static_cast<size_t>(Big(w));
            }
            for (size_t i = 0; i < n; ++i) {
                path.push_back("#" + std::to_string(i));
                const bool ok = Value(path, out, depth + 1);
                path.pop_back();
                if (!ok) {
                    return false;
                }
            }
            return true;
        }
        if (t >= 0xa0 && t <= 0xbf) {
            return leaf(1 + (t & 0x1f));
        }
        if (t == 0xd9 || t == 0xda || t == 0xdb || t == 0xc4 || t == 0xc5 || t == 0xc6) {
            const size_t w = (t == 0xd9 || t == 0xc4) ? 1 : (t == 0xda || t == 0xc5) ? 2 : 4;
            if (!Need(1 + w)) {
                return false;
            }
            pos += 1;
            const size_t n = static_cast<size_t>(Big(w));
            pos = at;
            return leaf(1 + w + n);
        }
        switch (t) {
        case 0xca:
        case 0xce:
        case 0xd2:
            return leaf(5);
        case 0xcb:
        case 0xcf:
        case 0xd3:
            return leaf(9);
        case 0xcc:
        case 0xd0:
            return leaf(2);
        case 0xcd:
        case 0xd1:
            return leaf(3);
        default:
            return false;
        }
    }

    std::span<const u8> data;
    size_t pos = 0;
};

bool PathIs(const Leaf& leaf, std::string_view section, std::string_view key) {
    // <section>.<key>[n].value, as the .set files store a keyframed value.
    return leaf.path.size() >= 2 && leaf.path[0] == section && leaf.path[1] == key &&
           !leaf.path.empty() && leaf.path.back() == "value";
}

struct Changes {
    int ssr_off = 0;
    int ssr_rays = 0;
    int ssao_off = 0;
    int lod = 0;
    int scenery = 0;
};

// Scales a number in place, keeping its MessagePack encoding (and so the file's layout): positive
// fixint, uint8/16/32 and float32/64. False when it can't (other types, or out of range).
bool ScaleInPlace(std::vector<u8>& bytes, size_t offset, u8 type, double factor) {
    const auto read_be = [&](size_t n) {
        u64 v = 0;
        for (size_t i = 0; i < n; ++i) {
            v = (v << 8) | bytes[offset + 1 + i];
        }
        return v;
    };
    const auto write_be = [&](size_t n, u64 v) {
        for (size_t i = n; i-- > 0;) {
            bytes[offset + 1 + i] = static_cast<u8>(v);
            v >>= 8;
        }
    };
    if (type <= 0x7f) {
        bytes[offset] = static_cast<u8>(static_cast<double>(type) * factor + 0.5);
        return bytes[offset] <= 0x7f;
    }
    const size_t width = type == 0xcc ? 1 : type == 0xcd ? 2 : type == 0xce ? 4 : 0;
    if (width != 0) {
        const double scaled = static_cast<double>(read_be(width)) * factor + 0.5;
        if (scaled >= static_cast<double>(u64{1} << (8 * width))) {
            return false;
        }
        write_be(width, static_cast<u64>(scaled));
        return true;
    }
    if (type == 0xcb) {
        write_be(8, std::bit_cast<u64>(std::bit_cast<double>(read_be(8)) * factor));
        return true;
    }
    if (type == 0xca) {
        const float value = std::bit_cast<float>(static_cast<u32>(read_be(4)));
        write_be(4, std::bit_cast<u32>(static_cast<float>(value * factor)));
        return true;
    }
    return false;
}

// Applies the settings to one .set file; true when it changed.
bool PatchSet(std::vector<u8>& bytes, Changes& changes) {
    std::vector<Leaf> leaves;
    if (!Walker{bytes}.Run(leaves)) {
        return false;
    }
    const auto reflections = Settings::values.fefw_reflections.GetValue();
    const bool ssao = !Settings::values.fefw_no_ambient_occlusion.GetValue();
    bool changed = false;
    for (const Leaf& leaf : leaves) {
        u8& b = bytes[leaf.offset];
        if (reflections == Settings::FefwReflections::Off && PathIs(leaf, "SSR", "Enabled") &&
            b == 0xc3) {
            b = 0xc2; // true -> false
            ++changes.ssr_off;
            changed = true;
        } else if (reflections == Settings::FefwReflections::Reduced &&
                   PathIs(leaf, "SSR", "RaySamplingCount") && b <= 0x7f && b > 8) {
            b = 8; // a positive fixint, so the same size
            ++changes.ssr_rays;
            changed = true;
        } else if (!ssao && PathIs(leaf, "AO", "SSAO_Enabled") && b == 0xc3) {
            b = 0xc2;
            ++changes.ssao_off;
            changed = true;
        }
    }
    return changed;
}

// lodPresets.lod: presets.<name>.Thresholds[i] (screen-size fractions, float64) for the crowd
// presets, doubled so their simpler models come in twice as soon.
bool PatchLod(std::vector<u8>& bytes, Changes& changes) {
    std::vector<Leaf> leaves;
    if (!Walker{bytes}.Run(leaves)) {
        return false;
    }
    bool changed = false;
    // Scenery: presets.Map*.Thresholds[i] are distances in metres (CalculateType 1); scaled to
    // 60%, so 50/100/150/200 m become 30/60/90/120 m and the hiding level's 1000 m becomes 600 m.
    if (Settings::values.fefw_reduced_scenery_detail.GetValue()) {
        for (const Leaf& leaf : leaves) {
            if (leaf.path.size() == 4 && leaf.path[0] == "presets" &&
                leaf.path[1].starts_with("Map") && leaf.path[2] == "Thresholds" &&
                bytes[leaf.offset] != 0 &&
                ScaleInPlace(bytes, leaf.offset, leaf.type, 0.6)) {
                ++changes.scenery;
                changed = true;
            }
        }
    }
    if (!Settings::values.fefw_reduced_crowd_detail.GetValue()) {
        return changed;
    }
    for (const Leaf& leaf : leaves) {
        if (leaf.path.size() != 4 || leaf.path[0] != "presets" ||
            (leaf.path[1] != "CharacterMob" && leaf.path[1] != "CharacterLegion") ||
            leaf.path[2] != "Thresholds" || leaf.type != 0xcb) {
            continue;
        }
        u64 raw = 0;
        for (int i = 0; i < 8; ++i) {
            raw = (raw << 8) | bytes[leaf.offset + 1 + i];
        }
        const double doubled = std::bit_cast<double>(raw) * 2.0;
        raw = std::bit_cast<u64>(doubled);
        for (int i = 7; i >= 0; --i) {
            bytes[leaf.offset + 1 + i] = static_cast<u8>(raw);
            raw >>= 8;
        }
        ++changes.lod;
        changed = true;
    }
    return changed;
}

// A tree of in-memory directories mirroring romfs paths.
struct Node {
    std::map<std::string, std::unique_ptr<Node>> dirs;
    std::vector<VirtualFile> files;

    VirtualDir Make(const std::string& name) const {
        std::vector<VirtualDir> subdirs;
        for (const auto& [dir_name, node] : dirs) {
            subdirs.push_back(node->Make(dir_name));
        }
        return std::make_shared<VectorVfsDirectory>(files, std::move(subdirs), name);
    }
};

void Walk(const VirtualDir& dir, const std::vector<std::string>& path,
          const std::function<void(const VirtualFile&, const std::vector<std::string>&)>& visit) {
    for (const auto& file : dir->GetFiles()) {
        visit(file, path);
    }
    for (const auto& sub : dir->GetSubdirectories()) {
        auto sub_path = path;
        sub_path.push_back(sub->GetName());
        Walk(sub, sub_path, visit);
    }
}

} // Anonymous namespace

bool Any() {
    return Settings::values.fefw_reflections.GetValue() != Settings::FefwReflections::Full ||
           Settings::values.fefw_no_ambient_occlusion.GetValue() ||
           Settings::values.fefw_reduced_crowd_detail.GetValue() ||
           Settings::values.fefw_reduced_scenery_detail.GetValue();
}

VirtualDir Build(const VirtualDir& romfs) {
    if (romfs == nullptr || !Any()) {
        return nullptr;
    }
    const auto graphics = romfs->GetSubdirectory("Graphics");
    const auto settings = graphics ? graphics->GetSubdirectory("Settings") : nullptr;
    if (settings == nullptr) {
        LOG_WARNING(Loader, "Fortune's Weave graphics settings not found in the RomFS; not changed");
        return nullptr;
    }
    Node root;
    Changes changes;
    size_t files = 0;
    const auto add = [&](const std::vector<std::string>& path, std::vector<u8> bytes,
                         const std::string& name) {
        Node* node = &root;
        for (const auto& part : path) {
            auto& next = node->dirs[part];
            if (!next) {
                next = std::make_unique<Node>();
            }
            node = next.get();
        }
        node->files.push_back(std::make_shared<VectorVfsFile>(std::move(bytes), name));
        ++files;
    };
    const bool set_files = Settings::values.fefw_reflections.GetValue() !=
                               Settings::FefwReflections::Full ||
                           Settings::values.fefw_no_ambient_occlusion.GetValue();
    const bool lod = Settings::values.fefw_reduced_crowd_detail.GetValue() ||
                     Settings::values.fefw_reduced_scenery_detail.GetValue();
    Walk(settings, {"Graphics", "Settings"},
         [&](const VirtualFile& file, const std::vector<std::string>& path) {
             const auto name = file->GetName();
             const bool is_set = set_files && name.ends_with(".set");
             const bool is_lod = lod && name == "lodPresets.lod";
             if (!is_set && !is_lod) {
                 return;
             }
             auto bytes = file->ReadAllBytes();
             const bool changed = is_set ? PatchSet(bytes, changes) : PatchLod(bytes, changes);
             if (changed) {
                 add(path, std::move(bytes), name);
             }
         });
    LOG_INFO(Loader,
             "Fortune's Weave graphics settings: {} files changed (reflections off in {}, 8 rays "
             "in {}, ambient occlusion off in {}, {} crowd detail thresholds, {} scenery detail "
             "distances)",
             files, changes.ssr_off, changes.ssr_rays, changes.ssao_off, changes.lod,
             changes.scenery);
    if (files == 0) {
        return nullptr;
    }
    return root.Make("");
}

} // namespace FileSys::FefwRomfsTweaks
