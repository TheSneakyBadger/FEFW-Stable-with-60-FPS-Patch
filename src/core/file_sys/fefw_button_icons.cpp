// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include "core/file_sys/fefw_button_icons.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <deque>
#include <map>
#include <optional>
#include <span>
#include <string_view>
#include <tuple>

#include "common/logging.h"
#include "common/settings.h"
#include "common/zstd_compression.h"
#include "core/file_sys/fefw_button_labels.h"
#include "core/file_sys/vfs/vfs.h"

namespace FileSys::FefwButtonIcons {
namespace {

// The characters the game's text uses for buttons (Msg/System.msbt, MID_Sys_EXFont_*).
constexpr u32 ButtonA = 0xE0E0;
constexpr u32 ButtonB = 0xE0E1;
constexpr u32 ButtonX = 0xE0E2;
constexpr u32 ButtonY = 0xE0E3;
constexpr u32 ButtonL = 0xE0E4;
constexpr u32 ButtonR = 0xE0E5;
constexpr u32 ButtonZL = 0xE0E6;
constexpr u32 ButtonZR = 0xE0E7;
constexpr u32 ButtonPlus = 0xE0EF;
constexpr u32 ButtonMinus = 0xE0F0;
// The Joy-Con's SL and SR: the game's mouse-mode switch (SL + SR), Ctrl + Enter in Fortune's Eden.
constexpr u32 ButtonSL = 0xE0E8;
constexpr u32 ButtonSR = 0xE0E9;
// The game's direction buttons (four round buttons, one filled) and the font's own d-pads.
constexpr std::array<std::pair<u32, u32>, 6> DpadCopies{{
    {0xE0AD, 0xE0D1}, // Up
    {0xE0AC, 0xE0D2}, // Down
    {0xE0AE, 0xE0D3}, // Left
    {0xE0AB, 0xE0D4}, // Right
    {0xE0CE, 0xE0D5}, // Up and down
    {0xE0CB, 0xE0D6}, // Left and right
}};

constexpr std::array<std::string_view, 3> FontNames{
    "__nintendo_ext_003_Small.bffnt.zst",
    "__nintendo_ext_003.bffnt.zst",
    "__nintendo_ext_003_Large.bffnt.zst",
};

template <typename T>
bool Read(std::span<const u8> data, size_t offset, T& out) {
    if (offset + sizeof(T) > data.size()) {
        return false;
    }
    std::memcpy(&out, data.data() + offset, sizeof(T));
    return true;
}

/// A glyph's picture: coverage 0-1, rows top to bottom.
struct Picture {
    int width = 0;
    int height = 0;
    std::vector<float> pixels;

    float& At(int x, int y) {
        return pixels[static_cast<size_t>(y) * width + x];
    }
    float At(int x, int y) const {
        return pixels[static_cast<size_t>(y) * width + x];
    }
};

/// A BFFNT font whose sheets are one BC4 texture (an array for several sheets) in a BNTX.
class Font {
public:
    explicit Font(std::vector<u8>& data_) : data{data_} {}

    bool Parse() {
        u32 magic{};
        if (!Read(data, 0, magic) || magic != 0x544E4646) { // "FFNT"
            return false;
        }
        size_t finf = 0;
        for (size_t i = 0; i + 4 <= std::min<size_t>(data.size(), 0x100); ++i) {
            if (std::memcmp(data.data() + i, "FINF", 4) == 0) {
                finf = i;
                break;
            }
        }
        u32 tglp{}, cwdh{}, cmap{};
        if (finf == 0 || !Read(data, finf + 20, tglp) || !Read(data, finf + 24, cwdh) ||
            !Read(data, finf + 28, cmap)) {
            return false;
        }
        u8 cw{}, ch{};
        u16 format{}, columns{}, lines{}, width{}, height{};
        u32 sheet_data{};
        if (!Read(data, tglp + 0, cw) || !Read(data, tglp + 1, ch) ||
            !Read(data, tglp + 10, format) || !Read(data, tglp + 12, columns) ||
            !Read(data, tglp + 14, lines) || !Read(data, tglp + 16, width) ||
            !Read(data, tglp + 18, height) || !Read(data, tglp + 20, sheet_data) || format != 12) {
            return false; // Not BC4
        }
        cell_w = cw;
        cell_h = ch;
        cols = columns;
        rows = lines;
        sheet_w = width;
        sheet_h = height;
        // The sheets: a BNTX holding one BC4 texture.
        const auto find = [&](size_t from, const char* tag) -> size_t {
            for (size_t i = from; i + 4 <= data.size() && i < from + 0x10000; ++i) {
                if (std::memcmp(data.data() + i, tag, 4) == 0) {
                    return i;
                }
            }
            return 0;
        };
        const size_t brti = find(sheet_data, "BRTI");
        const size_t brtd = find(sheet_data, "BRTD");
        u32 tex_format{}, tex_w{}, tex_h{}, array{}, block_height_log2{}, image_size{};
        if (brti == 0 || brtd == 0 || !Read(data, brti + 0x1C, tex_format) ||
            !Read(data, brti + 0x24, tex_w) || !Read(data, brti + 0x28, tex_h) ||
            !Read(data, brti + 0x30, array) || !Read(data, brti + 0x34, block_height_log2) ||
            !Read(data, brti + 0x50, image_size) || (tex_format >> 8) != 0x1D || tex_w != sheet_w ||
            tex_h != sheet_h || array == 0 || block_height_log2 > 5) {
            return false;
        }
        layers = array;
        block_height = 1U << block_height_log2;
        texture = brtd + 0x10;
        layer_size = image_size / array;
        if (texture + static_cast<size_t>(image_size) > data.size() ||
            layer_size < static_cast<size_t>(sheet_w / 4) * (sheet_h / 4) * 8) {
            return false;
        }
        // Character widths, one table.
        u16 first{}, last{};
        if (!Read(data, cwdh, first) || !Read(data, cwdh + 2, last)) {
            return false;
        }
        widths = cwdh + 8;
        widths_first = first;
        widths_last = last;
        // Character map: direct, table and scan ranges, chained.
        for (u32 at = cmap; at != 0;) {
            u32 lo{}, hi{}, next{};
            u16 method{};
            if (!Read(data, at, lo) || !Read(data, at + 4, hi) || !Read(data, at + 8, method) ||
                !Read(data, at + 12, next) || hi < lo || hi - lo > 0x10000) {
                return false;
            }
            const size_t body = at + 16;
            if (method == 0) {
                u16 start{};
                if (!Read(data, body, start)) {
                    return false;
                }
                for (u32 c = lo; c <= hi; ++c) {
                    glyphs[c] = static_cast<u16>(start + (c - lo));
                }
            } else if (method == 1) {
                for (u32 c = lo; c <= hi; ++c) {
                    u16 index{};
                    if (!Read(data, body + 2 * (c - lo), index)) {
                        return false;
                    }
                    if (index != 0xFFFF) {
                        glyphs[c] = index;
                    }
                }
            } else if (method == 2) {
                u16 count{};
                if (!Read(data, body, count)) {
                    return false;
                }
                for (u32 i = 0; i < count; ++i) {
                    u32 c{};
                    u16 index{};
                    if (!Read(data, body + 4 + 8 * i, c) || !Read(data, body + 8 + 8 * i, index)) {
                        return false;
                    }
                    glyphs[c] = index;
                }
            }
            at = next;
        }
        return true;
    }

    bool Has(u32 code) const {
        const auto it = glyphs.find(code);
        return it != glyphs.end() && it->second < layers * cols * rows &&
               it->second >= widths_first && it->second <= widths_last;
    }

    /// The glyph's cell, upright.
    Picture Get(u32 code) const {
        Picture picture{static_cast<int>(cell_w), static_cast<int>(cell_h)};
        picture.pixels.assign(static_cast<size_t>(cell_w) * cell_h, 0.0f);
        const auto [layer, x0, y0] = Cell(code);
        for (u32 j = 0; j < cell_h; ++j) {
            const u32 y = sheet_h - 1 - (y0 + j); // The sheet is stored bottom-up
            for (u32 i = 0; i < cell_w; ++i) {
                std::array<u8, 16> block{};
                DecodeBlock(layer, (x0 + i) / 4, y / 4, block);
                picture.At(i, j) = block[(y % 4) * 4 + (x0 + i) % 4] / 255.0f;
            }
        }
        return picture;
    }

    /// Draws `picture` (upright) into the glyph's cell.
    void Put(u32 code, const Picture& picture) {
        const auto [layer, x0, y0] = Cell(code);
        const u32 top = sheet_h - 1 - (y0 + cell_h - 1);
        const u32 bottom = sheet_h - 1 - y0;
        for (u32 by = top / 4; by <= bottom / 4; ++by) {
            for (u32 bx = x0 / 4; bx <= (x0 + cell_w - 1) / 4; ++bx) {
                std::array<u8, 16> block{};
                DecodeBlock(layer, bx, by, block);
                for (u32 p = 0; p < 16; ++p) {
                    const u32 x = bx * 4 + p % 4;
                    const u32 y = by * 4 + p / 4;
                    if (x < x0 || x >= x0 + cell_w || y < top || y > bottom) {
                        continue;
                    }
                    const float v = picture.At(x - x0, bottom - y);
                    block[p] = static_cast<u8>(std::clamp(std::lround(v * 255.0f), 0L, 255L));
                }
                EncodeBlock(layer, bx, by, block);
            }
        }
    }

    /// Gives `to` the advance and drawn width of `from` (left bearing, glyph width, char width).
    void CopyWidth(u32 to, u32 from) {
        const size_t a = widths + 3 * (glyphs.at(to) - widths_first);
        const size_t b = widths + 3 * (glyphs.at(from) - widths_first);
        if (a + 3 <= data.size() && b + 3 <= data.size()) {
            std::memmove(data.data() + a, data.data() + b, 3);
        }
    }

private:
    std::tuple<u32, u32, u32> Cell(u32 code) const {
        const u32 index = glyphs.at(code);
        const u32 per_sheet = cols * rows;
        const u32 layer = index / per_sheet;
        const u32 in_sheet = index % per_sheet;
        return {layer, 1 + (in_sheet % cols) * (cell_w + 1), 1 + (in_sheet / cols) * (cell_h + 1)};
    }

    /// Where a byte of the BC4 texture is: block-linear, `x` in bytes (8 per 4x4 block) and `y`
    /// in rows of blocks.
    size_t Address(u32 layer, u32 x, u32 y) const {
        const u32 gobs_x = ((sheet_w / 4) * 8 + 63) / 64;
        const size_t base = (static_cast<size_t>(y / (8 * block_height)) * gobs_x + x / 64) * 512 *
                                block_height +
                            static_cast<size_t>((y % (8 * block_height)) / 8) * 512;
        const size_t gob = (x % 16) + (y & 1) * 16 + ((y >> 1) & 1) * 32 + ((x >> 4) & 1) * 64 +
                           ((y >> 2) & 1) * 128 + ((x >> 5) & 1) * 256;
        return texture + layer * layer_size + base + gob;
    }

    void DecodeBlock(u32 layer, u32 bx, u32 by, std::array<u8, 16>& out) const {
        std::array<u8, 8> b{};
        for (u32 k = 0; k < 8; ++k) {
            b[k] = data[Address(layer, bx * 8 + k, by)];
        }
        const int r0 = b[0];
        const int r1 = b[1];
        std::array<int, 8> palette{r0, r1};
        if (r0 > r1) {
            for (int i = 1; i <= 6; ++i) {
                palette[1 + i] = ((7 - i) * r0 + i * r1) / 7;
            }
        } else {
            for (int i = 1; i <= 4; ++i) {
                palette[1 + i] = ((5 - i) * r0 + i * r1) / 5;
            }
            palette[6] = 0;
            palette[7] = 255;
        }
        u64 bits = 0;
        for (int i = 0; i < 6; ++i) {
            bits |= static_cast<u64>(b[2 + i]) << (8 * i);
        }
        for (int p = 0; p < 16; ++p) {
            out[p] = static_cast<u8>(palette[(bits >> (3 * p)) & 7]);
        }
    }

    void EncodeBlock(u32 layer, u32 bx, u32 by, const std::array<u8, 16>& pixels) {
        int lo = 255, hi = 0, inner_lo = 255, inner_hi = 0;
        for (const u8 v : pixels) {
            lo = std::min<int>(lo, v);
            hi = std::max<int>(hi, v);
            if (v != 0 && v != 255) {
                inner_lo = std::min<int>(inner_lo, v);
                inner_hi = std::max<int>(inner_hi, v);
            }
        }
        std::array<u8, 8> out{};
        if (lo == hi) {
            out[0] = static_cast<u8>(hi);
            out[1] = static_cast<u8>(lo);
        } else {
            // Eight steps between the extremes, or six between the inner values plus 0 and 255,
            // whichever is closer.
            const auto fit = [&](int r0, int r1, std::array<int, 8>& palette, u64& bits) {
                palette = {r0, r1};
                if (r0 > r1) {
                    for (int i = 1; i <= 6; ++i) {
                        palette[1 + i] = ((7 - i) * r0 + i * r1) / 7;
                    }
                } else {
                    for (int i = 1; i <= 4; ++i) {
                        palette[1 + i] = ((5 - i) * r0 + i * r1) / 5;
                    }
                    palette[6] = 0;
                    palette[7] = 255;
                }
                int error = 0;
                bits = 0;
                for (int p = 0; p < 16; ++p) {
                    int best = 0;
                    for (int k = 1; k < 8; ++k) {
                        if (std::abs(palette[k] - pixels[p]) < std::abs(palette[best] - pixels[p])) {
                            best = k;
                        }
                    }
                    const int d = palette[best] - pixels[p];
                    error += d * d;
                    bits |= static_cast<u64>(best) << (3 * p);
                }
                return error;
            };
            std::array<int, 8> palette{};
            u64 bits = 0;
            int r0 = hi, r1 = lo;
            int error = fit(hi, lo, palette, bits);
            if (inner_lo <= inner_hi) {
                u64 bits6 = 0;
                const int a = inner_lo == inner_hi ? std::max(inner_lo - 1, 0) : inner_lo;
                const int error6 = fit(a, inner_hi, palette, bits6);
                if (error6 < error) {
                    error = error6;
                    bits = bits6;
                    r0 = a;
                    r1 = inner_hi;
                }
            }
            out[0] = static_cast<u8>(r0);
            out[1] = static_cast<u8>(r1);
            for (int i = 0; i < 6; ++i) {
                out[2 + i] = static_cast<u8>(bits >> (8 * i));
            }
        }
        for (u32 k = 0; k < 8; ++k) {
            data[Address(layer, bx * 8 + k, by)] = out[k];
        }
    }

    std::vector<u8>& data;
    u32 cell_w = 0, cell_h = 0, cols = 0, rows = 0, sheet_w = 0, sheet_h = 0;
    u32 layers = 0, block_height = 1;
    size_t texture = 0, layer_size = 0;
    size_t widths = 0;
    u16 widths_first = 0, widths_last = 0;
    std::map<u32, u16> glyphs;
};

// --- Drawing -----------------------------------------------------------------------------------

struct Box {
    int x0, y0, x1, y1; // Inclusive-exclusive
    float Width() const {
        return static_cast<float>(x1 - x0);
    }
    float Height() const {
        return static_cast<float>(y1 - y0);
    }
};

Box Bounds(const Picture& p) {
    Box box{p.width, p.height, 0, 0};
    for (int y = 0; y < p.height; ++y) {
        for (int x = 0; x < p.width; ++x) {
            if (p.At(x, y) > 0.5f) {
                box.x0 = std::min(box.x0, x);
                box.y0 = std::min(box.y0, y);
                box.x1 = std::max(box.x1, x + 1);
                box.y1 = std::max(box.y1, y + 1);
            }
        }
    }
    if (box.x1 <= box.x0) {
        box = {0, 0, p.width, p.height};
    }
    return box;
}

/// The button's solid shape: what is drawn on it (its letter, cut out) filled in.
Picture Solid(const Picture& glyph) {
    const int w = glyph.width, h = glyph.height;
    std::vector<u8> outside(static_cast<size_t>(w) * h, 0);
    std::deque<std::pair<int, int>> queue;
    const auto ink = [&](int x, int y) { return glyph.At(x, y) > 0.5f; };
    const auto visit = [&](int x, int y) {
        if (x < 0 || y < 0 || x >= w || y >= h || outside[y * w + x] || ink(x, y)) {
            return;
        }
        outside[y * w + x] = 1;
        queue.emplace_back(x, y);
    };
    for (int x = 0; x < w; ++x) {
        visit(x, 0);
        visit(x, h - 1);
    }
    for (int y = 0; y < h; ++y) {
        visit(0, y);
        visit(w - 1, y);
    }
    while (!queue.empty()) {
        const auto [x, y] = queue.front();
        queue.pop_front();
        visit(x + 1, y);
        visit(x - 1, y);
        visit(x, y + 1);
        visit(x, y - 1);
    }
    Picture solid = glyph;
    for (int y = 0; y < h; ++y) {
        for (int x = 0; x < w; ++x) {
            if (outside[y * w + x] || ink(x, y)) {
                continue;
            }
            // A hole: it and the soft edge around it become solid.
            for (int dy = -1; dy <= 1; ++dy) {
                for (int dx = -1; dx <= 1; ++dx) {
                    if ((dx == 0 || dy == 0) && x + dx >= 0 && y + dy >= 0 && x + dx < w &&
                        y + dy < h) {
                        solid.At(x + dx, y + dy) = 1.0f;
                    }
                }
            }
        }
    }
    return solid;
}

const std::vector<u8>& Labels() {
    static const std::vector<u8> labels = [] {
        auto bytes = Common::Compression::DecompressDataZSTD(LabelMasksZstd);
        if (bytes.size() != LabelMasksSize) {
            bytes.clear();
        }
        return bytes;
    }();
    return labels;
}

/// Cuts `text` (one of LabelMasks) out of `base`, centred, `height_share` of its height at most
/// and `width_share` of its width.
Picture CutLabel(const Picture& base, std::string_view text, float height_share, float width_share,
                 float lower_by = 0.0f) {
    const auto& labels = Labels();
    const auto it = std::find_if(LabelMasks.begin(), LabelMasks.end(),
                                 [&](const LabelMask& m) { return m.text == text; });
    if (labels.empty() || it == LabelMasks.end()) {
        return base;
    }
    const Box box = Bounds(base);
    float th = box.Height() * height_share;
    float tw = th * it->width / it->height;
    if (tw > box.Width() * width_share) {
        tw = box.Width() * width_share;
        th = tw * it->height / it->width;
    }
    const float left = (box.x0 + box.x1) / 2.0f - tw / 2.0f;
    const float top = (box.y0 + box.y1) / 2.0f + lower_by * box.Height() - th / 2.0f;
    const float sx = it->width / tw;
    const float sy = it->height / th;
    Picture out = base;
    // Each pixel takes the label's coverage over the part of the label it shows (an area average).
    for (int y = 0; y < out.height; ++y) {
        const float my0 = (y - top) * sy, my1 = (y + 1 - top) * sy;
        if (my1 <= 0 || my0 >= it->height) {
            continue;
        }
        for (int x = 0; x < out.width; ++x) {
            const float mx0 = (x - left) * sx, mx1 = (x + 1 - left) * sx;
            if (mx1 <= 0 || mx0 >= it->width) {
                continue;
            }
            float sum = 0.0f;
            for (int my = std::max(0, static_cast<int>(my0));
                 my < std::min<int>(it->height, static_cast<int>(std::ceil(my1))); ++my) {
                const float wy = std::min(static_cast<float>(my + 1), my1) -
                                 std::max(static_cast<float>(my), my0);
                for (int mx = std::max(0, static_cast<int>(mx0));
                     mx < std::min<int>(it->width, static_cast<int>(std::ceil(mx1))); ++mx) {
                    const float wx = std::min(static_cast<float>(mx + 1), mx1) -
                                     std::max(static_cast<float>(mx), mx0);
                    sum += wx * wy * labels[it->offset + my * it->width + mx] / 255.0f;
                }
            }
            const float pixel_area = sx * sy;
            const float coverage = pixel_area > 0 ? sum / pixel_area : 0.0f;
            out.At(x, y) *= 1.0f - std::clamp(coverage, 0.0f, 1.0f);
        }
    }
    return out;
}

float Segment(float px, float py, float ax, float ay, float bx, float by) {
    const float dx = bx - ax, dy = by - ay;
    const float k = std::clamp(((px - ax) * dx + (py - ay) * dy) / (dx * dx + dy * dy), 0.0f, 1.0f);
    return std::hypot(px - ax - k * dx, py - ay - k * dy);
}

enum class Symbol { Circle, Cross, Triangle, Square, Menu, View, Create, Enter };

/// Cuts a symbol out of a round button: its distance field, anti-aliased by a pixel.
Picture CutSymbol(const Picture& base, Symbol symbol) {
    const Box box = Bounds(base);
    const float cx = (box.x0 + box.x1) / 2.0f, cy = (box.y0 + box.y1) / 2.0f;
    const float r = std::min(box.Width(), box.Height()) / 2.0f;
    const float t = r * 0.15f;
    Picture out = base;
    for (int y = 0; y < out.height; ++y) {
        for (int x = 0; x < out.width; ++x) {
            const float X = x + 0.5f - cx, Y = y + 0.5f - cy;
            float d = 1e9f;
            switch (symbol) {
            case Symbol::Circle:
                d = std::abs(std::hypot(X, Y) - r * 0.46f) - t / 2;
                break;
            case Symbol::Cross: {
                const float l = r * 0.48f;
                d = std::min(Segment(X, Y, -l, -l, l, l), Segment(X, Y, -l, l, l, -l)) - t / 2;
                break;
            }
            case Symbol::Triangle: {
                const float s = r * 0.55f, Y2 = Y - r * 0.10f;
                const float ax = 0, ay = -s, bx = s * 0.866f, by = s * 0.5f, ex = -s * 0.866f,
                            ey = s * 0.5f;
                d = std::min({Segment(X, Y2, ax, ay, bx, by), Segment(X, Y2, bx, by, ex, ey),
                              Segment(X, Y2, ex, ey, ax, ay)}) -
                    t / 2;
                break;
            }
            case Symbol::Square:
                d = std::abs(std::max(std::abs(X), std::abs(Y)) - r * 0.42f) - t / 2;
                break;
            case Symbol::Menu: {
                const float half = r * 0.5f, th = r * 0.12f;
                for (const float oy : {-r * 0.32f, 0.0f, r * 0.32f}) {
                    d = std::min(d, std::max(std::abs(X) - half, std::abs(Y - oy) - th / 2));
                }
                break;
            }
            case Symbol::View: {
                const float s = r * 0.30f, th = r * 0.11f, o = r * 0.14f;
                const auto ring = [&](float ox, float oy) {
                    return std::abs(std::max(std::abs(X - ox), std::abs(Y - oy)) - s) - th / 2;
                };
                const float front = std::max(std::abs(X + o), std::abs(Y - o)) - s;
                d = std::min(std::max(ring(o, -o), -(front + th / 2)), ring(-o, o));
                break;
            }
            case Symbol::Create: {
                const float th = r * 0.12f;
                const float stem =
                    std::max(std::abs(X) - th / 2, std::abs(Y + r * 0.05f) - r * 0.30f);
                const float head = std::min(Segment(X, Y, 0, -r * 0.38f, -r * 0.22f, -r * 0.16f),
                                            Segment(X, Y, 0, -r * 0.38f, r * 0.22f, -r * 0.16f)) -
                                   th / 2;
                const float base_line =
                    std::max(std::abs(X) - r * 0.42f, std::abs(Y - r * 0.42f) - th / 2);
                const float side_l = std::max(std::abs(X + r * 0.42f) - th / 2,
                                              std::abs(Y - r * 0.27f) - r * 0.15f);
                const float side_r = std::max(std::abs(X - r * 0.42f) - th / 2,
                                              std::abs(Y - r * 0.27f) - r * 0.15f);
                d = std::min({stem, head, base_line, side_l, side_r});
                break;
            }
            case Symbol::Enter: {
                // The return arrow: down the right, along the bottom, an arrowhead on the left.
                const float th = r * 0.13f;
                const float down = Segment(X, Y, r * 0.38f, -r * 0.40f, r * 0.38f, r * 0.12f);
                const float along = Segment(X, Y, r * 0.38f, r * 0.12f, -r * 0.30f, r * 0.12f);
                const float head =
                    std::min(Segment(X, Y, -r * 0.42f, r * 0.12f, -r * 0.16f, -r * 0.14f),
                             Segment(X, Y, -r * 0.42f, r * 0.12f, -r * 0.16f, r * 0.38f));
                d = std::min({down, along, head}) - th / 2;
                break;
            }
            }
            const float cover = std::clamp(0.5f - d, 0.0f, 1.0f);
            out.At(x, y) *= 1.0f - cover;
        }
    }
    return out;
}

/// Redraws the button characters of one font; false when the font isn't the expected kind.
bool Redraw(std::vector<u8>& bytes, Settings::FefwButtonIcons icons) {
    Font font{bytes};
    if (!font.Parse()) {
        return false;
    }
    for (const u32 code : {ButtonA, ButtonB, ButtonX, ButtonY, ButtonL, ButtonR, ButtonZL,
                           ButtonZR, ButtonPlus, ButtonMinus}) {
        if (!font.Has(code)) {
            return false;
        }
    }
    const Picture a = font.Get(ButtonA), b = font.Get(ButtonB), x = font.Get(ButtonX),
                  y = font.Get(ButtonY);
    const Picture l = Solid(font.Get(ButtonL)), r = Solid(font.Get(ButtonR));
    const Picture zl = Solid(font.Get(ButtonZL)), zr = Solid(font.Get(ButtonZR));
    const Picture plus = Solid(font.Get(ButtonPlus)), minus = Solid(font.Get(ButtonMinus));
    const bool xbox = icons == Settings::FefwButtonIcons::Xbox;
    if (xbox) {
        // Xbox's letters by position: its A is at the bottom, where Nintendo's B is.
        font.Put(ButtonA, b);
        font.Put(ButtonB, a);
        font.Put(ButtonX, y);
        font.Put(ButtonY, x);
    } else {
        const Picture round = Solid(a);
        font.Put(ButtonA, CutSymbol(round, Symbol::Circle));
        font.Put(ButtonB, CutSymbol(round, Symbol::Cross));
        font.Put(ButtonX, CutSymbol(round, Symbol::Triangle));
        font.Put(ButtonY, CutSymbol(round, Symbol::Square));
    }
    font.Put(ButtonL, CutLabel(l, xbox ? "LB" : "L1", 0.42f, 0.78f));
    font.Put(ButtonR, CutLabel(r, xbox ? "RB" : "R1", 0.42f, 0.78f));
    font.Put(ButtonZL, CutLabel(zl, xbox ? "LT" : "L2", 0.40f, 0.70f, 0.06f));
    font.Put(ButtonZR, CutLabel(zr, xbox ? "RT" : "R2", 0.40f, 0.70f, 0.06f));
    font.Put(ButtonPlus, CutSymbol(plus, Symbol::Menu));
    font.Put(ButtonMinus, CutSymbol(minus, xbox ? Symbol::View : Symbol::Create));
    // SL + SR (switching mouse mode) as Fortune's Eden's Ctrl + Enter.
    if (font.Has(ButtonSL) && font.Has(ButtonSR)) {
        font.Put(ButtonSL, CutLabel(Solid(font.Get(ButtonSL)), "Ctrl", 0.40f, 0.80f));
        font.Put(ButtonSR, CutSymbol(Solid(font.Get(ButtonSR)), Symbol::Enter));
    }
    for (const auto& [to, from] : DpadCopies) {
        if (font.Has(to) && font.Has(from)) {
            font.Put(to, font.Get(from));
            font.CopyWidth(to, from);
        }
    }
    return true;
}

} // Anonymous namespace

bool Active() {
    return Settings::values.fefw_button_icons.GetValue() != Settings::FefwButtonIcons::Switch;
}

size_t Build(const VirtualDir& romfs,
             const std::function<void(const std::vector<std::string>& path, std::vector<u8> bytes,
                                      const std::string& name)>& add) {
    if (romfs == nullptr || !Active()) {
        return 0;
    }
    const auto icons = Settings::values.fefw_button_icons.GetValue();
    const auto fonts = romfs->GetSubdirectory("Font");
    if (fonts == nullptr) {
        LOG_WARNING(Loader, "Fortune's Weave button icons: the Font folder isn't in the RomFS");
        return 0;
    }
    size_t changed = 0;
    for (const std::string_view name : FontNames) {
        const auto file = fonts->GetFile(std::string{name});
        if (file == nullptr) {
            LOG_WARNING(Loader, "Fortune's Weave button icons: {} not found", name);
            continue;
        }
        const auto packed = file->ReadAllBytes();
        auto bytes = Common::Compression::DecompressDataZSTD(packed);
        if (bytes.empty() || !Redraw(bytes, icons)) {
            LOG_WARNING(Loader, "Fortune's Weave button icons: {} isn't the expected font", name);
            continue;
        }
        auto repacked = Common::Compression::CompressDataZSTD(bytes.data(), bytes.size(), 3);
        if (repacked.empty()) {
            continue;
        }
        add({"Font"}, std::move(repacked), std::string{name});
        ++changed;
    }
    LOG_INFO(Loader, "Fortune's Weave button icons: {} ({} of {} button fonts redrawn)",
             icons == Settings::FefwButtonIcons::Xbox ? "Xbox" : "PlayStation", changed,
             FontNames.size());
    return changed;
}

} // namespace FileSys::FefwButtonIcons
