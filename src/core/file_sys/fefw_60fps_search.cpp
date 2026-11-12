// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include "core/file_sys/fefw_60fps_search.h"

#include <algorithm>
#include <array>
#include <cstring>
#include <unordered_map>
#include <vector>

#include <fmt/format.h>

#include "core/file_sys/fefw_60fps_search_data.h"

namespace FefwSixtySearch {
namespace {

constexpr u64 Align(u64 value, u64 alignment) {
    return (value + alignment - 1) & ~(alignment - 1);
}

u32 Read32(std::span<const u8> image, u64 offset) {
    u32 value;
    std::memcpy(&value, image.data() + offset, sizeof(value));
    return value;
}

void Write32(std::span<u8> image, u64 offset, u32 value) {
    std::memcpy(image.data() + offset, &value, sizeof(value));
}

// The anchors' positions in `words`, one each, or the first anchor that isn't found exactly once.
bool FindAnchors(const std::vector<u32>& words, std::vector<u64>& found, std::string& why) {
    std::unordered_map<u32, std::vector<u32>> by_pivot;
    for (u32 index = 0; index < Anchors.size(); ++index) {
        const Anchor& anchor = Anchors[index];
        by_pivot[Pattern[anchor.first + anchor.pivot]].push_back(index);
    }
    std::vector<u32> count(Anchors.size(), 0);
    found.assign(Anchors.size(), 0);
    for (u64 i = 0; i < words.size(); ++i) {
        const auto it = by_pivot.find(words[i]);
        if (it == by_pivot.end()) {
            continue;
        }
        for (const u32 index : it->second) {
            const Anchor& anchor = Anchors[index];
            if (i < anchor.pivot || i - anchor.pivot + anchor.length > words.size()) {
                continue;
            }
            const u64 start = i - anchor.pivot;
            bool match = true;
            for (u32 k = 0; k < anchor.length && match; ++k) {
                match = (words[start + k] & Mask[anchor.first + k]) == Pattern[anchor.first + k];
            }
            if (match) {
                ++count[index];
                found[index] = (start + anchor.at) * 4;
            }
        }
    }
    for (u32 index = 0; index < Anchors.size(); ++index) {
        if (count[index] != 1) {
            why = fmt::format("the code at 1.1.0's {:#x} was found {} times (needs exactly once)",
                              Anchors[index].ref, count[index]);
            return false;
        }
    }
    return true;
}

// adrp at `pc` and the add / load / store after it: the address they build.
std::optional<u64> DecodeAdrpPair(const std::vector<u32>& words, u64 pc) {
    const u64 index = pc / 4;
    if (index + 1 >= words.size()) {
        return std::nullopt;
    }
    const u32 adrp = words[index];
    const u32 next = words[index + 1];
    if ((adrp & 0x9F000000) != 0x90000000 || ((next >> 5) & 0x1F) != (adrp & 0x1F)) {
        return std::nullopt;
    }
    s64 imm = (((adrp >> 5) & 0x7FFFF) << 2) | ((adrp >> 29) & 3);
    if (imm & (1 << 20)) {
        imm -= 1 << 21;
    }
    const u64 page = (pc & ~u64{0xFFF}) + static_cast<u64>(imm << 12);
    const u64 imm12 = (next >> 10) & 0xFFF;
    if ((next & 0x7F800000) == 0x11000000) {
        return page + imm12;
    }
    if ((next & 0x3B000000) == 0x39000000) {
        u32 size = next >> 30;
        if ((next & 0x04800000) == 0x04800000) {
            size = 4;
        }
        return page + (imm12 << size);
    }
    return std::nullopt;
}

bool Relocate(u32& word, u32 type, u64 s, u64 p) {
    const s64 delta = static_cast<s64>(s) - static_cast<s64>(p);
    switch (type) {
    case 0:
    case 1: {
        const s64 d = delta >> 2;
        if (d < -(s64{1} << 25) || d >= (s64{1} << 25)) {
            return false;
        }
        word = (word & 0xFC000000) | (static_cast<u32>(d) & 0x3FFFFFF);
        return true;
    }
    case 2: {
        const s64 d = delta >> 2;
        if (d < -(s64{1} << 18) || d >= (s64{1} << 18)) {
            return false;
        }
        word = (word & 0xFF00001F) | ((static_cast<u32>(d) & 0x7FFFF) << 5);
        return true;
    }
    case 3: {
        const s64 d = (static_cast<s64>(s & ~u64{0xFFF}) - static_cast<s64>(p & ~u64{0xFFF})) >> 12;
        word = (word & 0x9F00001F) | ((static_cast<u32>(d) & 3) << 29) |
               (((static_cast<u32>(d) >> 2) & 0x7FFFF) << 5);
        return true;
    }
    case 4:
    case 5:
    case 6:
    case 7:
    case 8: {
        constexpr std::array<u32, 9> shifts{0, 0, 0, 0, 0, 0, 1, 2, 3};
        const u64 lo = s & 0xFFF;
        if (lo & ((u64{1} << shifts[type]) - 1)) {
            return false;
        }
        word = (word & 0xFFC003FF) | (static_cast<u32>(lo >> shifts[type]) << 10);
        return true;
    }
    default:
        return false;
    }
}

std::vector<u32> TextWords(std::span<const u8> image, u32 text_size) {
    std::vector<u32> words(std::min<u64>(text_size, image.size()) / 4);
    std::memcpy(words.data(), image.data(), words.size() * 4);
    return words;
}

} // Anonymous namespace

std::optional<Layout> Apply(std::span<u8> image, u32 text_size, std::string& why, u64 reserved,
                            std::span<u8> room, bool prefer_room) {
    if (text_size == 0 || text_size > image.size() || image.size() < 0x10) {
        why = "the game's code segment is not where it should be";
        return std::nullopt;
    }
    const std::vector<u32> words = TextWords(image, text_size);
    std::vector<u64> found;
    if (!FindAnchors(words, found, why)) {
        return std::nullopt;
    }
    for (const Site& site : Sites) {
        if (words[found[site.anchor] / 4] != site.expect) {
            why = fmt::format("the instruction at 1.1.0's {:#x} is different",
                              Anchors[site.anchor].ref);
            return std::nullopt;
        }
    }
    std::array<std::optional<u64>, 2> data{};
    for (const DataRef& ref : DataRefs) {
        const auto address = DecodeAdrpPair(words, found[ref.anchor]);
        if (!address || (data[ref.what] && *data[ref.what] != *address)) {
            why = "the game data the patch uses could not be located";
            return std::nullopt;
        }
        data[ref.what] = address;
    }
    if (!data[0] || !data[1]) {
        why = "the game data the patch uses could not be located";
        return std::nullopt;
    }

    // The table: right after the game's bss (MOD0's bss end), in the rest of that page.
    const u32 mod0 = Read32(image, 4);
    if (u64{mod0} + 16 > image.size() || std::memcmp(image.data() + mod0, "MOD0", 4) != 0) {
        why = "the game's module header was not found";
        return std::nullopt;
    }
    const u64 bss_end = mod0 + static_cast<s64>(static_cast<s32>(Read32(image, mod0 + 12)));
    // The loader's added room (when there is some), after the last module so no module moves
    // (#45: a mod's code reached other modules at fixed distances from main): routines in its
    // first page, table in its second. Used only where the game's own free space is taken.
    const bool has_room = reserved != 0 && reserved % 0x1000 == 0 && room.size() >= ReserveBytes;
    const auto room_free = [&](u64 start, u64 size) {
        return has_room && start >= reserved && start - reserved + size <= room.size() &&
               std::all_of(room.begin() + (start - reserved),
                           room.begin() + (start - reserved + size), [](u8 b) { return b == 0; });
    };
    bool used_room = false;

    // The routines: in the .text page padding, as high as it is still all zero (built-in
    // patches and mods put their own code at its start), or else in the room.
    const u64 padding_end = Align(text_size, 0x1000);
    const u64 padding_start = Align(text_size, 16);
    std::optional<u64> free_start;
    if (prefer_room && Cave.size() <= 0x1000 && room_free(reserved, Cave.size())) {
        free_start = reserved;
    } else if (padding_end <= image.size()) {
        for (u64 start = (padding_end - Cave.size()) & ~u64{0xF};
             start >= padding_start && start + Cave.size() <= padding_end; start -= 16) {
            if (std::all_of(image.begin() + start, image.begin() + start + Cave.size(),
                            [](u8 b) { return b == 0; })) {
                free_start = start;
                break;
            }
            if (start < 16) {
                break;
            }
        }
    }
    if (!free_start && Cave.size() <= 0x1000 && room_free(reserved, Cave.size())) {
        free_start = reserved;
    }
    if (!free_start) {
        why = fmt::format("the {} bytes of free space after the game's code it needs are in use",
                          Cave.size());
        return std::nullopt;
    }
    const bool routines_in_room = has_room && *free_start == reserved;
    used_room = routines_in_room;

    // The table: right after the game's bss (MOD0's bss end), in the rest of that page; in the
    // room with the routines when a mod has the game's free space (it may keep its own data in
    // the zeroed memory after the bss too).
    u64 table = Align(bss_end, 0x100);
    if ((prefer_room || routines_in_room) && room_free(reserved + 0x1000, TableBytes)) {
        table = reserved + 0x1000;
        used_room = true;
    } else if (table + TableBytes > Align(bss_end, 0x1000) || table + TableBytes > image.size() ||
               std::any_of(image.begin() + table, image.begin() + table + TableBytes,
                           [](u8 b) { return b != 0; })) {
        if (!room_free(reserved + 0x1000, TableBytes)) {
            why = "there is no free room after the game's data for the patch's table";
            return std::nullopt;
        }
        table = reserved + 0x1000;
        used_room = true;
    }
    const u64 cave_start = *free_start;

    std::vector<u8> cave(Cave.begin(), Cave.end());
    for (const Reloc& reloc : Relocs) {
        u64 s = 0;
        switch (reloc.target) {
        case 0:
            s = found[reloc.anchor];
            break;
        case 1:
            s = table;
            break;
        case 2:
            s = *data[1];
            break;
        default:
            s = cave_start;
            break;
        }
        s += static_cast<s64>(reloc.addend);
        u32 word;
        std::memcpy(&word, cave.data() + reloc.offset, 4);
        if (!Relocate(word, reloc.type, s, cave_start + reloc.offset)) {
            why = fmt::format("a routine can't reach its target from {:#x}", cave_start);
            return std::nullopt;
        }
        std::memcpy(cave.data() + reloc.offset, &word, 4);
    }
    std::vector<std::pair<u64, u32>> site_words;
    for (const Site& site : Sites) {
        u32 word = site.value;
        if (site.branch) {
            word = 0x14000000;
            if (!Relocate(word, 0, cave_start + site.value, found[site.anchor])) {
                why = "a site can't reach its routine";
                return std::nullopt;
            }
        }
        site_words.emplace_back(found[site.anchor], word);
    }

    if (routines_in_room) {
        std::memcpy(room.data() + (cave_start - reserved), cave.data(), cave.size());
    } else {
        std::memcpy(image.data() + cave_start, cave.data(), cave.size());
    }
    for (const auto& [offset, word] : site_words) {
        Write32(image, offset, word);
    }
    return Layout{
        .app_object = *data[0], .table = table, .routines = cave_start, .reserved = used_room};
}

bool MatchesReference(std::span<const u8> image, u32 text_size, std::string& why) {
    if (text_size == 0 || text_size > image.size()) {
        why = "no code segment";
        return false;
    }
    const std::vector<u32> words = TextWords(image, text_size);
    std::vector<u64> found;
    if (!FindAnchors(words, found, why)) {
        return false;
    }
    for (u32 index = 0; index < Anchors.size(); ++index) {
        if (found[index] != Anchors[index].ref) {
            why = fmt::format("1.1.0's {:#x} was found at {:#x}", Anchors[index].ref, found[index]);
            return false;
        }
    }
    return true;
}

} // namespace FefwSixtySearch
