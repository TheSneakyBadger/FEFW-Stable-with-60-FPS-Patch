# SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
# SPDX-License-Identifier: GPL-3.0-or-later
"""The 60 FPS patch for game versions other than 1.1.0 (used by make_60fps_patch.py).

Every address the 1.1.0 patch uses (the 57 sites, the code its routines jump back to, and the
game data they read) becomes an anchor: the instructions around it in 1.1.0, with the bits that
move between builds masked out (branch and page offsets, and the low 12 address bits after an
adrp). The emulator (fefw_60fps_search.cpp) looks for every anchor in the game it loads, needs
exactly one match each and the exact original instruction at every site, places the routines in
the .text page padding and fixes their relocations, or leaves the game alone.

simulate() is the same search and placement in Python, used to check the data against 1.1.0.
"""
import re, struct
import numpy as np

# Relocation types the routines use, as the C++ side numbers them.
RELOC_TYPES = {
    'R_AARCH64_JUMP26': 0, 'R_AARCH64_CALL26': 1, 'R_AARCH64_CONDBR19': 2,
    'R_AARCH64_ADR_PREL_PG_HI21': 3, 'R_AARCH64_ADD_ABS_LO12_NC': 4,
    'R_AARCH64_LDST8_ABS_LO12_NC': 5, 'R_AARCH64_LDST16_ABS_LO12_NC': 6,
    'R_AARCH64_LDST32_ABS_LO12_NC': 7, 'R_AARCH64_LDST64_ABS_LO12_NC': 8,
}
TARGET_ANCHOR, TARGET_TABLE, TARGET_FRAME, TARGET_CAVE = 0, 1, 2, 3
DATA_APP_OBJECT, DATA_FRAME_COUNTER = 0, 1
TABLE_BYTES = 0x100  # the table and the 30 Hz state (fefw_frame_rate.cpp, make_60fps_patch.py)
MIN_SIDE, MAX_SIDE, MARGIN = 4, 48, 2


def _rd(w): return w & 0x1F
def _rn(w): return (w >> 5) & 0x1F


def window_masks(words, start, end):
    """Masks for words[start:end]: 1 bits are compared."""
    tracked = set()
    for k in range(max(0, start - 4), start):  # an adrp just before the window
        tracked = _step(words[k], tracked)[1]
    masks = []
    for k in range(start, end):
        mask, tracked = _step(words[k], tracked)
        masks.append(mask)
    return masks


def _step(w, tracked):
    tracked = set(tracked)
    mask = 0xFFFFFFFF
    if (w & 0x1F000000) == 0x10000000:            # adr / adrp
        mask = 0x9F00001F
        if w & 0x80000000:
            tracked.add(_rd(w))
        else:
            tracked.discard(_rd(w))
        return mask, tracked
    if (w & 0x7C000000) == 0x14000000:            # b / bl
        return 0xFC000000, tracked
    if (w & 0xFF000010) == 0x54000000:            # b.cond
        return 0xFF00001F, tracked
    if (w & 0x7E000000) == 0x34000000:            # cbz / cbnz
        return 0xFF00001F, tracked
    if (w & 0x7E000000) == 0x36000000:            # tbz / tbnz
        return 0xFFF8001F, tracked
    if (w & 0x3B000000) == 0x18000000:            # ldr (literal)
        mask = 0xFF00001F
        tracked.discard(_rd(w))
        return mask, tracked
    if (w & 0x7F800000) == 0x11000000:            # add (immediate)
        if _rn(w) in tracked:
            mask = 0xFFC003FF
        tracked.discard(_rd(w))
        return mask, tracked
    if (w & 0x3B000000) == 0x39000000:            # load / store (unsigned offset)
        if _rn(w) in tracked:
            mask = 0xFFC003FF
        if w & 0x00400000:                         # a load writes Rt
            tracked.discard(_rd(w))
        return mask, tracked
    tracked.discard(_rd(w))
    return mask, tracked


class Text:
    def __init__(self, data):
        self.words = np.frombuffer(data[:len(data) & ~3], dtype='<u4')
        values, counts = np.unique(self.words, return_counts=True)
        self._values, self._counts = values, counts

    def frequency(self, w):
        i = np.searchsorted(self._values, w)
        return int(self._counts[i]) if i < len(self._values) and self._values[i] == w else 0

    def matches(self, pattern, mask, pivot):
        T = self.words
        n = len(pattern)
        cand = np.nonzero(T == pattern[pivot])[0] - pivot
        cand = cand[(cand >= 0) & (cand + n <= len(T))]
        for k in range(n):
            if k == pivot or len(cand) == 0:
                continue
            cand = cand[(T[cand + k] & mask[k]) == pattern[k]]
        return cand


def make_anchor(text, addr):
    """Smallest window around addr (plus a margin) that matches once in the reference."""
    words = [int(x) for x in text.words]
    idx = addr // 4
    side = MIN_SIDE
    while side <= MAX_SIDE:
        start, end = max(0, idx - side), min(len(words), idx + side + 1)
        found = _signature(text, words, start, end)
        if found is not None and len(text.matches(*found)) == 1:
            start, end = max(0, start - MARGIN), min(len(words), end + MARGIN)
            pattern, mask, pivot = _signature(text, words, start, end)
            assert len(text.matches(pattern, mask, pivot)) == 1
            return dict(ref=addr, pattern=pattern, mask=mask, pivot=pivot, at=idx - start)
        side += 2
    raise SystemExit(f'{addr:#x}: no unique window within {MAX_SIDE} instructions')


def _signature(text, words, start, end):
    masks = window_masks(words, start, end)
    pattern = [w & m for w, m in zip(words[start:end], masks)]
    full = [k for k, m in enumerate(masks) if m == 0xFFFFFFFF]
    if not full:
        return None
    pivot = min(full, key=lambda k: (text.frequency(words[start + k]), abs(k - (end - start) // 2)))
    return pattern, masks, pivot


def data_refs(text, target, kind, count=2):
    """Code that builds `target` with adrp + add / load / store (the next instruction)."""
    words = text.words
    page = target & ~0xFFF
    pcs = np.arange(len(words), dtype=np.int64) * 4
    is_adrp = (words & 0x9F000000) == 0x90000000
    immlo = (words >> 29) & 3
    immhi = (words >> 5) & 0x7FFFF
    imm = ((immhi.astype(np.int64) << 2) | immlo.astype(np.int64))
    imm = np.where(imm & (1 << 20), imm - (1 << 21), imm) << 12
    hits = np.nonzero(is_adrp & (((pcs & ~0xFFF) + imm) == page))[0]
    out = []
    for i in hits:
        if i + 1 >= len(words):
            continue
        a, b = int(words[i]), int(words[i + 1])
        if decode_lo12(b, _rd(a)) == (target & 0xFFF):
            out.append(int(i) * 4)
    return out


def decode_lo12(w, reg):
    if _rn(w) != reg:
        return None
    imm12 = (w >> 10) & 0xFFF
    if (w & 0x7F800000) == 0x11000000:
        return imm12
    if (w & 0x3B000000) == 0x39000000:
        size = w >> 30
        if (w & 0x04800000) == 0x04800000:  # 128-bit SIMD
            size = 4
        return imm12 << size
    return None


def decode_adrp_pair(words, index):
    a = words[index]
    if (a & 0x9F000000) != 0x90000000:
        return None
    imm = (((a >> 5) & 0x7FFFF) << 2) | ((a >> 29) & 3)
    if imm & (1 << 20):
        imm -= 1 << 21
    page = ((index * 4) & ~0xFFF) + (imm << 12)
    lo = decode_lo12(words[index + 1], _rd(a))
    return None if lo is None else page + lo


def build(ref_image, ref_text_end, sites, singles, cave_raw, relocs, app_object, frame_counter,
          preferred=None):
    preferred = preferred or {}
    """sites: [(site, cave offset)], singles: [(site, word)]; relocs from object_info()."""
    text = Text(ref_image[:ref_text_end])
    anchors, index = [], {}

    def anchor(addr):
        if addr not in index:
            index[addr] = len(anchors)
            anchors.append(make_anchor(text, addr))
        return index[addr]

    def word_at(addr): return struct.unpack_from('<I', ref_image, addr)[0]

    site_rows = []
    for site, offset in sites:
        site_rows.append((anchor(site), 1, offset, word_at(site)))
    for site, word in singles:
        site_rows.append((anchor(site), 0, word, word_at(site)))
    reloc_rows = []
    for offset, kind, symbol, addend in relocs['cave']:
        if symbol.startswith('abs_'):
            reloc_rows.append((offset, RELOC_TYPES[kind], TARGET_ANCHOR, anchor(int(symbol[4:], 16)), addend))
        elif symbol == 'TABLE':
            reloc_rows.append((offset, RELOC_TYPES[kind], TARGET_TABLE, 0, addend))
        elif symbol == 'FRAMECOUNTER':
            reloc_rows.append((offset, RELOC_TYPES[kind], TARGET_FRAME, 0, addend))
        elif symbol == '.cave':
            reloc_rows.append((offset, RELOC_TYPES[kind], TARGET_CAVE, 0, addend))
        else:
            raise SystemExit(f'unexpected relocation target {symbol}')
    data_rows = []
    for what, target in ((DATA_APP_OBJECT, app_object), (DATA_FRAME_COUNTER, frame_counter)):
        refs = []
        found = data_refs(text, target, what)
        # The game's start-up store of the 30 FPS rate first: its window also pins the rate's
        # offset in the application object (str w9, [x8, #0x1dc]).
        found.sort(key=lambda pc: pc != preferred.get(what))
        for pc in found:
            try:
                refs.append(anchor(pc))
            except SystemExit:
                continue
            if len(refs) == 2:
                break
        if not refs:
            raise SystemExit(f'no usable reference to {target:#x}')
        data_rows += [(a, what) for a in refs]
    return dict(anchors=anchors, sites=site_rows, relocs=reloc_rows, data=data_rows,
                cave=cave_raw)


def _align(x, a): return (x + a - 1) & ~(a - 1)


def simulate(image, text_end, t, cave_start=None):
    """The emulator's search and placement. Returns (patched image, layout) or raises."""
    words = np.frombuffer(image[:text_end & ~3], dtype='<u4')
    text = Text(image[:text_end])
    found = []
    for a in t['anchors']:
        m = text.matches(a['pattern'], a['mask'], a['pivot'])
        if len(m) != 1:
            raise ValueError(f"anchor {a['ref']:#x}: {len(m)} matches")
        found.append((int(m[0]) + a['at']) * 4)
    for a, branch, value, expect in t['sites']:
        if int(words[found[a] // 4]) != expect:
            raise ValueError(f'site {found[a]:#x} holds another instruction')
    data = {}
    for a, what in t['data']:
        addr = decode_adrp_pair([int(x) for x in words[found[a] // 4:found[a] // 4 + 2]], 0)
        addr = None if addr is None else addr + (found[a] & ~0xFFF)
        if addr is None or data.setdefault(what, addr) != addr:
            raise ValueError('game data references disagree')
    mod0 = struct.unpack_from('<I', image, 4)[0]
    bss_end = mod0 + struct.unpack_from('<i', image, mod0 + 12)[0]
    table = _align(bss_end, 0x100)
    if table + TABLE_BYTES > _align(bss_end, 0x1000) or any(image[table:table + TABLE_BYTES]):
        raise ValueError('no room for the table')
    cave = bytearray(t['cave'])
    cave_end = _align(text_end, 0x1000)
    if cave_start is None:
        cave_start = (cave_end - len(cave)) & ~0xF
    if cave_start < _align(text_end, 4) or any(image[cave_start:cave_end]):
        raise ValueError('no room for the routines')
    for offset, kind, target, a, addend in t['relocs']:
        s = {TARGET_ANCHOR: lambda: found[a], TARGET_TABLE: lambda: table,
             TARGET_FRAME: lambda: data[DATA_FRAME_COUNTER], TARGET_CAVE: lambda: cave_start}[target]()
        s += addend
        p = cave_start + offset
        w = struct.unpack_from('<I', cave, offset)[0]
        w = relocate(w, kind, s, p)
        struct.pack_into('<I', cave, offset, w)
    out = bytearray(image)
    out[cave_start:cave_start + len(cave)] = cave
    for a, branch, value, expect in t['sites']:
        site = found[a]
        w = relocate(0x14000000, 0, cave_start + value, site) if branch else value
        struct.pack_into('<I', out, site, w)
    return bytes(out), dict(table=table, app_object=data[DATA_APP_OBJECT],
                            frame_counter=data[DATA_FRAME_COUNTER], cave=cave_start)


def relocate(w, kind, s, p):
    if kind in (0, 1):
        d = (s - p) >> 2
        assert -(1 << 25) <= d < (1 << 25)
        return (w & 0xFC000000) | (d & 0x3FFFFFF)
    if kind == 2:
        d = (s - p) >> 2
        assert -(1 << 18) <= d < (1 << 18)
        return (w & 0xFF00001F) | ((d & 0x7FFFF) << 5)
    if kind == 3:
        d = ((s & ~0xFFF) - (p & ~0xFFF)) >> 12
        return (w & 0x9F00001F) | ((d & 3) << 29) | (((d >> 2) & 0x7FFFF) << 5)
    lo = s & 0xFFF
    shift = {4: 0, 5: 0, 6: 1, 7: 2, 8: 3}[kind]
    assert lo % (1 << shift) == 0
    return (w & 0xFFC003FF) | ((lo >> shift) << 10)


def header(t, build_id):
    pattern, mask, anchors = [], [], []
    for a in t['anchors']:
        anchors.append(f"{{{a['ref']:#x}, {len(pattern)}, {len(a['pattern'])}, {a['at']}, {a['pivot']}}}")
        pattern += a['pattern']
        mask += a['mask']

    def words(xs):
        body = ', '.join(f'0x{x:08X}' for x in xs)
        return re.sub(r'((?:0x[0-9A-F]{8}, ){8})', r'\1\n    ', body)

    def rows(xs):
        return ',\n    '.join('{' + ', '.join(str(v) for v in x) + '}' for x in xs)

    cave = ', '.join(f'0x{b:02X}' for b in t['cave'])
    cave = re.sub(r'((?:0x[0-9A-F]{2}, ){16})', r'\1\n    ', cave)
    return f'''// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

// Generated by tools/fefw/make_60fps_patch.py (fefw_search.py): the 60 FPS game patch made from
// 1.1.0 (build {build_id}) as anchors, routines and relocations, for fefw_60fps_search.cpp to
// place in other game versions. Do not edit by hand.

#pragma once

#include <array>

#include "common/common_types.h"

namespace FefwSixtySearch {{

struct Anchor {{
    u32 ref;     ///< the address in 1.1.0 (for the log)
    u32 first;   ///< into Pattern / Mask
    u32 length;  ///< instructions
    u32 at;      ///< the anchored instruction, from the first one
    u32 pivot;   ///< an instruction compared whole, used to find candidates
}};
struct Site {{
    u32 anchor;
    u32 branch;  ///< 1: branch to the routine at Cave + value; 0: write the instruction value
    u32 value;
    u32 expect;  ///< the instruction the site must hold
}};
struct Reloc {{
    u32 offset;  ///< in Cave
    u32 type;    ///< 0 JUMP26, 1 CALL26, 2 CONDBR19, 3 ADR_PREL_PG_HI21, 4 ADD_LO12, 5-8 LDST8-64_LO12
    u32 target;  ///< 0 anchor, 1 table, 2 frame counter, 3 the routines' start
    u32 anchor;
    s32 addend;
}};
struct DataRef {{
    u32 anchor;  ///< an adrp, and the add / load / store after it
    u32 what;    ///< 0 the application object pointer, 1 the frame counter
}};

inline constexpr u32 TableBytes = {TABLE_BYTES:#x};

inline constexpr std::array<u32, {len(pattern)}> Pattern{{
    {words(pattern)}
}};
inline constexpr std::array<u32, {len(mask)}> Mask{{
    {words(mask)}
}};
inline constexpr std::array<Anchor, {len(anchors)}> Anchors{{{{
    {rows([tuple(x.strip('{}').split(', ')) for x in anchors])}
}}}};
inline constexpr std::array<Site, {len(t['sites'])}> Sites{{{{
    {rows([(a, b, hex(v), hex(e)) for a, b, v, e in t['sites']])}
}}}};
inline constexpr std::array<Reloc, {len(t['relocs'])}> Relocs{{{{
    {rows([(hex(o), k, tg, a, ad) for o, k, tg, a, ad in t['relocs']])}
}}}};
inline constexpr std::array<DataRef, {len(t['data'])}> DataRefs{{{{
    {rows(t['data'])}
}}}};
inline constexpr std::array<u8, {len(t['cave'])}> Cave{{
    {cave}
}};

}} // namespace FefwSixtySearch
'''
