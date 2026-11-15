// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

// BC7 block encoder for the CPU, the same code as host_shaders/bc7_encoder.comp (between its
// BEGIN/END BC7 CORE markers) with a few shims for GLSL's types and built-ins, so a texture comes
// out the same whichever path compresses it. Keep the two in step.

#include <bit>
#include <cmath>
#include <cstring>

#include "video_core/textures/bc7_encoder.h"

namespace Tegra::Texture::BCN {
namespace {

// GLSL's types and built-ins, as the shader's code uses them.
using uint = unsigned;

struct uvec4 {
    uvec4(uint x_, uint y_, uint z_, uint w_) : x{x_}, y{y_}, z{z_}, w{w_} {}
    uint x;
    uint y;
    uint z;
    uint w;
};

int min(int a, int b) {
    return a < b ? a : b;
}
int max(int a, int b) {
    return a > b ? a : b;
}
int clamp(int x, int lo, int hi) {
    return x < lo ? lo : (x > hi ? hi : x);
}
float min(float a, float b) {
    return a < b ? a : b;
}
float max(float a, float b) {
    return a > b ? a : b;
}
float clamp(float x, float lo, float hi) {
    return x < lo ? lo : (x > hi ? hi : x);
}
float abs(float x) {
    return std::fabs(x);
}
float round(float x) {
    return std::round(x);
}
float sqrt(float x) {
    return std::sqrt(x);
}
int bitCount(uint x) {
    return static_cast<int>(std::popcount(x));
}

// Blocks whose mode 6 error (sum of squares over the 16 texels' channels) is at most this skip
// the other modes: the search for them is most of the work, and on the dumped textures 95% of
// blocks stop here with little to gain.
const int SKIP_THRESHOLD = 1000;

const int WEIGHTS2[4] = {0, 21, 43, 64};
const int WEIGHTS3[8] = {0, 9, 18, 27, 37, 46, 55, 64};
const int WEIGHTS4[16] = {0, 4, 9, 13, 17, 21, 26, 30, 34, 38, 43, 47, 51, 55, 60, 64};

const uint PARTITIONS2[64] = {
    0x50505050u, 0x40404040u, 0x54545454u, 0x54505040u, 0x50404000u, 0x55545450u, 0x55545040u, 0x54504000u,
    0x50400000u, 0x55555450u, 0x55544000u, 0x54400000u, 0x55555440u, 0x55550000u, 0x55555500u, 0x55000000u,
    0x55150100u, 0x00004054u, 0x15010000u, 0x00405054u, 0x00004050u, 0x15050100u, 0x05010000u, 0x40505054u,
    0x00404050u, 0x05010100u, 0x14141414u, 0x05141450u, 0x01155440u, 0x00555500u, 0x15014054u, 0x05414150u,
    0x44444444u, 0x55005500u, 0x11441144u, 0x05055050u, 0x05500550u, 0x11114444u, 0x41144114u, 0x44111144u,
    0x15055054u, 0x01055040u, 0x05041050u, 0x05455150u, 0x14414114u, 0x50050550u, 0x41411414u, 0x00141400u,
    0x00041504u, 0x00105410u, 0x10541000u, 0x04150400u, 0x50410514u, 0x41051450u, 0x05415014u, 0x14054150u,
    0x41050514u, 0x41505014u, 0x40011554u, 0x54150140u, 0x50505500u, 0x00555050u, 0x15151010u, 0x54540404u};
const uint PARTITIONS3[64] = {
    0xAA685050u, 0x6A5A5040u, 0x5A5A4200u, 0x5450A0A8u, 0xA5A50000u, 0xA0A05050u, 0x5555A0A0u, 0x5A5A5050u,
    0xAA550000u, 0xAA555500u, 0xAAAA5500u, 0x90909090u, 0x94949494u, 0xA4A4A4A4u, 0xA9A59450u, 0x2A0A4250u,
    0xA5945040u, 0x0A425054u, 0xA5A5A500u, 0x55A0A0A0u, 0xA8A85454u, 0x6A6A4040u, 0xA4A45000u, 0x1A1A0500u,
    0x0050A4A4u, 0xAAA59090u, 0x14696914u, 0x69691400u, 0xA08585A0u, 0xAA821414u, 0x50A4A450u, 0x6A5A0200u,
    0xA9A58000u, 0x5090A0A8u, 0xA8A09050u, 0x24242424u, 0x00AA5500u, 0x24924924u, 0x24499224u, 0x50A50A50u,
    0x500AA550u, 0xAAAA4444u, 0x66660000u, 0xA5A0A5A0u, 0x50A050A0u, 0x69286928u, 0x44AAAA44u, 0x66666600u,
    0xAA444444u, 0x54A854A8u, 0x95809580u, 0x96969600u, 0xA85454A8u, 0x80959580u, 0xAA141414u, 0x96960000u,
    0xAAAA1414u, 0xA05050A0u, 0xA0A5A5A0u, 0x96000000u, 0x40804080u, 0xA9A8A9A8u, 0xAAAAAA44u, 0x2A4A5254u};
const int ANCHORS2[64] = {
    15, 15, 15, 15, 15, 15, 15, 15, 15, 15, 15, 15, 15, 15, 15, 15,
    15, 2, 8, 2, 2, 8, 8, 15, 2, 8, 2, 2, 8, 8, 2, 2,
    15, 15, 6, 8, 2, 8, 15, 15, 2, 8, 2, 2, 2, 15, 15, 6,
    6, 2, 6, 8, 15, 15, 2, 2, 15, 15, 15, 15, 15, 2, 2, 15};
const int ANCHORS3A[64] = {
    3, 3, 15, 15, 8, 3, 15, 15, 8, 8, 6, 6, 6, 5, 3, 3,
    3, 3, 8, 15, 3, 3, 6, 10, 5, 8, 8, 6, 8, 5, 15, 15,
    8, 15, 3, 5, 6, 10, 8, 15, 15, 3, 15, 5, 15, 15, 15, 15,
    3, 15, 5, 5, 5, 8, 5, 10, 5, 10, 8, 13, 15, 12, 3, 3};
const int ANCHORS3B[64] = {
    15, 8, 8, 3, 15, 15, 3, 8, 15, 15, 15, 15, 15, 15, 15, 8,
    15, 8, 15, 3, 15, 8, 15, 8, 3, 15, 6, 10, 15, 15, 10, 8,
    15, 3, 15, 10, 10, 8, 9, 10, 6, 15, 8, 15, 3, 6, 6, 8,
    15, 3, 15, 15, 15, 15, 15, 15, 15, 15, 15, 15, 3, 15, 15, 8};

// The same shapes as bit masks of the texels in subset 1 (and, for three, subset 2 in the top
// half), for matching shapes to clusters.
const uint MASKS2[64] = {
    0x0000CCCCu, 0x00008888u, 0x0000EEEEu, 0x0000ECC8u, 0x0000C880u, 0x0000FEECu, 0x0000FEC8u, 0x0000EC80u,
    0x0000C800u, 0x0000FFECu, 0x0000FE80u, 0x0000E800u, 0x0000FFE8u, 0x0000FF00u, 0x0000FFF0u, 0x0000F000u,
    0x0000F710u, 0x0000008Eu, 0x00007100u, 0x000008CEu, 0x0000008Cu, 0x00007310u, 0x00003100u, 0x00008CCEu,
    0x0000088Cu, 0x00003110u, 0x00006666u, 0x0000366Cu, 0x000017E8u, 0x00000FF0u, 0x0000718Eu, 0x0000399Cu,
    0x0000AAAAu, 0x0000F0F0u, 0x00005A5Au, 0x000033CCu, 0x00003C3Cu, 0x000055AAu, 0x00009696u, 0x0000A55Au,
    0x000073CEu, 0x000013C8u, 0x0000324Cu, 0x00003BDCu, 0x00006996u, 0x0000C33Cu, 0x00009966u, 0x00000660u,
    0x00000272u, 0x000004E4u, 0x00004E40u, 0x00002720u, 0x0000C936u, 0x0000936Cu, 0x000039C6u, 0x0000639Cu,
    0x00009336u, 0x00009CC6u, 0x0000817Eu, 0x0000E718u, 0x0000CCF0u, 0x00000FCCu, 0x00007744u, 0x0000EE22u};
const uint MASKS3[64] = {
    0xF60008CCu, 0x73008CC8u, 0x3310CC80u, 0x00CEEC00u, 0xCC003300u, 0xCC0000CCu, 0x00CCFF00u, 0x3300CCCCu,
    0xF0000F00u, 0xF0000FF0u, 0xFF0000F0u, 0x88884444u, 0x88886666u, 0xCCCC2222u, 0xEC80136Cu, 0x7310008Cu,
    0xC80036C8u, 0x310008CEu, 0xCCC03330u, 0x0CCCF000u, 0xEE0000EEu, 0x77008888u, 0xCC0022C0u, 0x33004430u,
    0x00CC0C22u, 0xFC880344u, 0x06606996u, 0x66009960u, 0xC88C0330u, 0xF9000066u, 0x0CC0C22Cu, 0x73108C00u,
    0xEC801300u, 0x08CEC400u, 0xEC80004Cu, 0x44442222u, 0x0F0000F0u, 0x49242492u, 0x42942942u, 0x0C30C30Cu,
    0x03C0C03Cu, 0xFF0000AAu, 0x5500AA00u, 0xCCCC3030u, 0x0C0CC0C0u, 0x66669090u, 0x0FF0A00Au, 0x5550AAA0u,
    0xF0000AAAu, 0x0E0EE0E0u, 0x88887070u, 0x99906660u, 0xE00E0EE0u, 0x88880770u, 0xF0000666u, 0x99006600u,
    0xFF000066u, 0xC00C0CC0u, 0xCCCC0330u, 0x90006000u, 0x08088080u, 0xEEEE1010u, 0xFFF0000Au, 0x731008CEu};

thread_local int texel[16][4];

struct Mode {
    int mode;
    int ns;  // subsets
    int cb;  // colour bits
    int ab;  // alpha bits
    int epb; // a p-bit per endpoint
    int spb; // a p-bit per subset
    int ib;  // index bits
};

Mode MakeMode(int mode, int ns, int cb, int ab, int epb, int spb, int ib) {
    Mode m;
    m.mode = mode;
    m.ns = ns;
    m.cb = cb;
    m.ab = ab;
    m.epb = epb;
    m.spb = spb;
    m.ib = ib;
    return m;
}

// Quantised endpoints of one subset: q[endpoint * 4 + channel] without the p-bits.
struct Endpoints {
    int q[8];
    int p[2];
};

struct Bits {
    uint w[4];
    int pos;
};

void Put(Bits& b, uint value, int count) {
    // Fields are at most 8 bits, so one spans at most two words.
    const int word = b.pos >> 5;
    const int bit = b.pos & 31;
    value &= (1u << uint(count)) - 1u;
    b.w[word] |= value << uint(bit);
    if (bit + count > 32) {
        b.w[word + 1] |= value >> uint(32 - bit);
    }
    b.pos += count;
}

int Weight(int ib, int j) {
    return ib == 2 ? WEIGHTS2[j] : (ib == 3 ? WEIGHTS3[j] : WEIGHTS4[j]);
}

int Subset(int ns, int shape, int i) {
    if (ns == 1) {
        return 0;
    }
    const uint pattern = ns == 2 ? PARTITIONS2[shape] : PARTITIONS3[shape];
    return int((pattern >> uint(2 * i)) & 3u);
}

int Anchor(int ns, int shape, int sub) {
    if (sub == 0) {
        return 0;
    }
    if (sub == 1) {
        return ns == 2 ? ANCHORS2[shape] : ANCHORS3A[shape];
    }
    return ANCHORS3B[shape];
}

// An n-bit value widened to 8 bits by repeating its top bits.
int Expand(int x, int n) {
    x <<= (8 - n);
    return x | (x >> n);
}

int Interpolate(int e0, int e1, int w) {
    return ((64 - w) * e0 + w * e1 + 32) >> 6;
}

// Nearest cb-bit value to v (0-255) with p-bit p appended when pbits is 1.
int Quantize(float v, int cb, int pbits, int p) {
    const int bits = cb + pbits;
    const int top = (1 << cb) - 1;
    if (bits == 8) {
        // Stored as is (with the p-bit as its lowest bit): no search.
        const float q = pbits != 0 ? (v - float(p)) * 0.5f : v;
        return clamp(int(round(q)), 0, top);
    }
    const int target = int(round(v));
    const int guess = int(round(v / 255.0f * float(top)));
    int best = 0;
    int best_error = 1 << 30;
    for (int q = max(0, guess - 1); q <= min(top, guess + 1); ++q) {
        const int x = pbits != 0 ? ((q << 1) | p) : q;
        const int d = Expand(x, bits) - target;
        if (d * d < best_error) {
            best_error = d * d;
            best = q;
        }
    }
    return best;
}

void DecodeEndpoints(Mode m, Endpoints e, int (&colour)[8]) {
    const int pb = m.epb + m.spb;
    for (int k = 0; k < 2; ++k) {
        for (int c = 0; c < 3; ++c) {
            const int x = pb != 0 ? ((e.q[k * 4 + c] << 1) | e.p[k]) : e.q[k * 4 + c];
            colour[k * 4 + c] = Expand(x, m.cb + pb);
        }
        if (m.ab != 0) {
            const int x = pb != 0 ? ((e.q[k * 4 + 3] << 1) | e.p[k]) : e.q[k * 4 + 3];
            colour[k * 4 + 3] = Expand(x, m.ab + pb);
        } else {
            colour[k * 4 + 3] = 255;
        }
    }
}

// Nearest palette entry for each texel of the subset (other texels keep their index); returns
// the summed squared error. The texel's position along the endpoint line gives the entry, and
// its neighbours are checked as the weights aren't quite even.
int AssignIndices(Mode m, int colour[8], int shape, int sub, int (&idx)[16]) {
    const int top = (1 << m.ib) - 1;
    int palette[16][4];
    for (int j = 0; j <= top; ++j) {
        const int w = Weight(m.ib, j);
        for (int c = 0; c < 4; ++c) {
            palette[j][c] = Interpolate(colour[c], colour[4 + c], w);
        }
    }
    float d[4];
    float len2 = 0.0f;
    for (int c = 0; c < 4; ++c) {
        d[c] = float(colour[4 + c] - colour[c]);
        len2 += d[c] * d[c];
    }
    const float scale = len2 > 0.0f ? float(top) / len2 : 0.0f;
    int sse = 0;
    for (int i = 0; i < 16; ++i) {
        if (Subset(m.ns, shape, i) != sub) {
            continue;
        }
        float t = 0.0f;
        for (int c = 0; c < 4; ++c) {
            t += float(texel[i][c] - colour[c]) * d[c];
        }
        const int guess = clamp(int(round(t * scale)), 0, top);
        int best = guess;
        int best_error = 1 << 30;
        for (int j = max(guess - 1, 0); j <= min(guess + 1, top); ++j) {
            int e = 0;
            for (int c = 0; c < 4; ++c) {
                const int diff = palette[j][c] - texel[i][c];
                e += diff * diff;
            }
            if (e < best_error) {
                best_error = e;
                best = j;
            }
        }
        idx[i] = best;
        sse += best_error;
    }
    return sse;
}

// The subset's line: through its mean along the covariance's row for its widest channel (near
// enough the principal axis, without iterating), from the lowest to the highest projection.
void FitLine(int ns, int shape, int sub, bool alpha, float (&lo)[4], float (&hi)[4]) {
    const int nc = alpha ? 4 : 3;
    float sum[4];
    float prod[10];
    for (int c = 0; c < 4; ++c) {
        sum[c] = 0.0f;
    }
    for (int c = 0; c < 10; ++c) {
        prod[c] = 0.0f;
    }
    int count = 0;
    for (int i = 0; i < 16; ++i) {
        if (Subset(ns, shape, i) != sub) {
            continue;
        }
        ++count;
        int j = 0;
        for (int a = 0; a < 4; ++a) {
            const float va = float(texel[i][a]);
            sum[a] += va;
            for (int b = a; b < 4; ++b) {
                prod[j] += va * float(texel[i][b]);
                ++j;
            }
        }
    }
    const float n = float(max(count, 1));
    float mean[4];
    for (int c = 0; c < 4; ++c) {
        mean[c] = sum[c] / n;
    }
    float cov[4][4];
    int k = 0;
    for (int a = 0; a < 4; ++a) {
        for (int b = a; b < 4; ++b) {
            const float c = a < nc && b < nc ? prod[k] - sum[a] * sum[b] / n : 0.0f;
            cov[a][b] = c;
            cov[b][a] = c;
            ++k;
        }
    }
    float axis[4];
    int major = 0;
    for (int c = 1; c < nc; ++c) {
        if (cov[c][c] > cov[major][major]) {
            major = c;
        }
    }
    for (int c = 0; c < 4; ++c) {
        axis[c] = c < nc ? cov[major][c] : 0.0f;
    }
    float len = 0.0f;
    for (int c = 0; c < nc; ++c) {
        len += axis[c] * axis[c];
    }
    if (len < 1e-8f) {
        for (int c = 0; c < 4; ++c) {
            lo[c] = mean[c];
            hi[c] = mean[c];
        }
    } else {
        len = sqrt(len);
        for (int c = 0; c < 4; ++c) {
            axis[c] /= len;
        }
        float tmin = 1e9f;
        float tmax = -1e9f;
        for (int i = 0; i < 16; ++i) {
            if (Subset(ns, shape, i) != sub) {
                continue;
            }
            float t = 0.0f;
            for (int c = 0; c < nc; ++c) {
                t += (float(texel[i][c]) - mean[c]) * axis[c];
            }
            tmin = min(tmin, t);
            tmax = max(tmax, t);
        }
        for (int c = 0; c < 4; ++c) {
            lo[c] = clamp(mean[c] + axis[c] * tmin, 0.0f, 255.0f);
            hi[c] = clamp(mean[c] + axis[c] * tmax, 0.0f, 255.0f);
        }
    }
    if (!alpha) {
        lo[3] = 255.0f;
        hi[3] = 255.0f;
    }
}

// How far an endpoint lands from v once quantised with p-bit p (squared, over its channels).
float PbitError(Mode m, float v[4], int p) {
    const int pb = m.epb + m.spb;
    float e = 0.0f;
    for (int c = 0; c < 4; ++c) {
        if (c == 3 && m.ab == 0) {
            continue;
        }
        const int bits = c == 3 ? m.ab : m.cb;
        const int q = Quantize(v[c], bits, pb, p);
        const float diff = float(Expand((q << 1) | p, bits + pb)) - v[c];
        e += diff * diff;
    }
    return e;
}

// Quantises the line to the mode, with the p-bits that land the endpoints nearest; returns the
// subset's error.
int QuantizeSubset(Mode m, float lo[4], float hi[4], int shape, int sub,
                   Endpoints& e, int (&idx)[16]) {
    const int pb = m.epb + m.spb;
    e.p[0] = 0;
    e.p[1] = 0;
    if (pb != 0) {
        // Opaque texels need p-bits of 1 to keep alpha exactly 255 (no faint see-through).
        bool opaque = m.ab != 0;
        for (int i = 0; i < 16 && opaque; ++i) {
            if (Subset(m.ns, shape, i) == sub && texel[i][3] != 255) {
                opaque = false;
            }
        }
        if (opaque) {
            e.p[0] = 1;
            e.p[1] = 1;
        } else if (m.epb != 0) {
            e.p[0] = PbitError(m, lo, 1) < PbitError(m, lo, 0) ? 1 : 0;
            e.p[1] = PbitError(m, hi, 1) < PbitError(m, hi, 0) ? 1 : 0;
        } else {
            const float e0 = PbitError(m, lo, 0) + PbitError(m, hi, 0);
            const float e1 = PbitError(m, lo, 1) + PbitError(m, hi, 1);
            e.p[0] = e1 < e0 ? 1 : 0;
            e.p[1] = e.p[0];
        }
    }
    for (int c = 0; c < 3; ++c) {
        e.q[c] = Quantize(lo[c], m.cb, pb, e.p[0]);
        e.q[4 + c] = Quantize(hi[c], m.cb, pb, e.p[1]);
    }
    e.q[3] = m.ab != 0 ? Quantize(lo[3], m.ab, pb, e.p[0]) : 0;
    e.q[7] = m.ab != 0 ? Quantize(hi[3], m.ab, pb, e.p[1]) : 0;
    int colour[8];
    DecodeEndpoints(m, e, colour);
    return AssignIndices(m, colour, shape, sub, idx);
}

int EncodeSubset(Mode m, int shape, int sub, Endpoints& e, int (&idx)[16]) {
    float lo[4];
    float hi[4];
    FitLine(m.ns, shape, sub, m.ab != 0, lo, hi);
    return QuantizeSubset(m, lo, hi, shape, sub, e, idx);
}

// How far texels sit from their principal line (squared), from their count, sums and sums of
// products: the scatter left over once the variance along the line is taken out.
float LineScatter(float n, float sum[4], float prod[10], int nc) {
    if (n < 1.5f) {
        return 0.0f;
    }
    float cov[4][4];
    int k = 0;
    for (int a = 0; a < 4; ++a) {
        for (int b = a; b < 4; ++b) {
            const float c = a < nc && b < nc ? prod[k] - sum[a] * sum[b] / n : 0.0f;
            cov[a][b] = c;
            cov[b][a] = c;
            ++k;
        }
    }
    float trace = 0.0f;
    int major = 0;
    for (int c = 0; c < nc; ++c) {
        trace += cov[c][c];
        if (cov[c][c] > cov[major][major]) {
            major = c;
        }
    }
    float axis[4];
    for (int c = 0; c < 4; ++c) {
        axis[c] = cov[major][c];
    }
    for (int it = 0; it < 3; ++it) {
        float t[4];
        float l = 0.0f;
        for (int a = 0; a < 4; ++a) {
            t[a] = cov[a][0] * axis[0] + cov[a][1] * axis[1] + cov[a][2] * axis[2] +
                   cov[a][3] * axis[3];
            l = max(l, abs(t[a]));
        }
        if (l < 1e-6f) {
            return 0.0f;
        }
        for (int c = 0; c < 4; ++c) {
            axis[c] = t[c] / l;
        }
    }
    float num = 0.0f;
    float den = 0.0f;
    for (int a = 0; a < 4; ++a) {
        float t = 0.0f;
        for (int b = 0; b < 4; ++b) {
            t += cov[a][b] * axis[b];
        }
        num += axis[a] * t;
        den += axis[a] * axis[a];
    }
    return max(trace - num / den, 0.0f);
}

// The line scatter of one subset of a partition shape.
float ShapeScatter(int ns, int shape, int sub, int nc) {
    float n = 0.0f;
    float sum[4];
    float prod[10];
    for (int c = 0; c < 4; ++c) {
        sum[c] = 0.0f;
    }
    for (int c = 0; c < 10; ++c) {
        prod[c] = 0.0f;
    }
    for (int i = 0; i < 16; ++i) {
        if (Subset(ns, shape, i) != sub) {
            continue;
        }
        n += 1.0f;
        int j = 0;
        for (int a = 0; a < 4; ++a) {
            const float va = float(texel[i][a]);
            sum[a] += va;
            for (int b = a; b < 4; ++b) {
                prod[j] += va * float(texel[i][b]);
                ++j;
            }
        }
    }
    return LineScatter(n, sum, prod, nc);
}

float Distance2(int i, float centre[4], int nc) {
    float d2 = 0.0f;
    for (int c = 0; c < nc; ++c) {
        const float d = float(texel[i][c]) - centre[c];
        d2 += d * d;
    }
    return d2;
}

// The partition shape for ns subsets: the texels are split into ns groups by colour (k-means
// from the ends of the block's line, and for three the texel farthest from both), the shapes
// that match the groups best are short-listed, and of those the one whose subsets sit nearest
// their lines wins. Scoring every shape by its lines cost the most of all the encoding.
int BestPartition(int ns, bool alpha) {
    const int nc = alpha ? 4 : 3;
    float lo[4];
    float hi[4];
    FitLine(1, 0, 0, alpha, lo, hi);
    float centre[3][4];
    for (int c = 0; c < 4; ++c) {
        centre[0][c] = lo[c];
        centre[1][c] = hi[c];
        centre[2][c] = 0.0f;
    }
    if (ns == 3) {
        float farthest = -1.0f;
        int pick = 0;
        for (int i = 0; i < 16; ++i) {
            const float d = min(Distance2(i, centre[0], nc), Distance2(i, centre[1], nc));
            if (d > farthest) {
                farthest = d;
                pick = i;
            }
        }
        for (int c = 0; c < 4; ++c) {
            centre[2][c] = float(texel[pick][c]);
        }
    }
    uint group[3];
    for (int it = 0; it < 3; ++it) {
        float sum[3][4];
        float count[3];
        for (int k = 0; k < 3; ++k) {
            group[k] = 0u;
            count[k] = 0.0f;
            for (int c = 0; c < 4; ++c) {
                sum[k][c] = 0.0f;
            }
        }
        for (int i = 0; i < 16; ++i) {
            int nearest = 0;
            float nearest_d = Distance2(i, centre[0], nc);
            for (int k = 1; k < ns; ++k) {
                const float d = Distance2(i, centre[k], nc);
                if (d < nearest_d) {
                    nearest_d = d;
                    nearest = k;
                }
            }
            group[nearest] |= 1u << uint(i);
            count[nearest] += 1.0f;
            for (int c = 0; c < 4; ++c) {
                sum[nearest][c] += float(texel[i][c]);
            }
        }
        for (int k = 0; k < ns; ++k) {
            if (count[k] > 0.0f) {
                for (int c = 0; c < 4; ++c) {
                    centre[k][c] = sum[k][c] / count[k];
                }
            }
        }
    }
    // Short-list: the 4 shapes covering the most texels with the groups (in the best order).
    int list[4];
    int list_score[4];
    for (int k = 0; k < 4; ++k) {
        list[k] = 0;
        list_score[k] = -1;
    }
    for (int p = 0; p < 64; ++p) {
        uint s[3];
        if (ns == 2) {
            s[1] = MASKS2[p];
            s[0] = ~s[1] & 0xFFFFu;
            s[2] = 0u;
        } else {
            s[1] = MASKS3[p] & 0xFFFFu;
            s[2] = MASKS3[p] >> 16;
            s[0] = ~(s[1] | s[2]) & 0xFFFFu;
        }
        int score;
        if (ns == 2) {
            score = max(bitCount(group[0] & s[0]) + bitCount(group[1] & s[1]),
                        bitCount(group[0] & s[1]) + bitCount(group[1] & s[0]));
        } else {
            score = 0;
            for (int a = 0; a < 3; ++a) {
                for (int b = 0; b < 3; ++b) {
                    if (b == a) {
                        continue;
                    }
                    const int c = 3 - a - b;
                    score = max(score, bitCount(group[0] & s[a]) + bitCount(group[1] & s[b]) +
                                           bitCount(group[2] & s[c]));
                }
            }
        }
        for (int k = 0; k < 4; ++k) {
            if (score > list_score[k]) {
                for (int j = 3; j > k; --j) {
                    list[j] = list[j - 1];
                    list_score[j] = list_score[j - 1];
                }
                list[k] = p;
                list_score[k] = score;
                break;
            }
        }
    }
    int best = list[0];
    float best_error = 1e30f;
    for (int k = 0; k < 4; ++k) {
        float e = 0.0f;
        for (int sub = 0; sub < ns; ++sub) {
            e += ShapeScatter(ns, list[k], sub, nc);
        }
        if (e < best_error) {
            best_error = e;
            best = list[k];
        }
    }
    return best;
}

struct Result {
    int sse;
    Bits bits;
};

// Anchor texels store their index without its top bit, which must be 0: flip the subset if not.
void FixAnchor(Mode m, Endpoints& e, int (&idx)[16], int shape, int sub, int anchor) {
    const int top = 1 << (m.ib - 1);
    if ((idx[anchor] & top) == 0) {
        return;
    }
    for (int c = 0; c < 4; ++c) {
        const int t = e.q[c];
        e.q[c] = e.q[4 + c];
        e.q[4 + c] = t;
    }
    const int tp = e.p[0];
    e.p[0] = e.p[1];
    e.p[1] = tp;
    const int highest = (1 << m.ib) - 1;
    for (int i = 0; i < 16; ++i) {
        if (Subset(m.ns, shape, i) == sub) {
            idx[i] = highest - idx[i];
        }
    }
}

// Modes 1, 2, 3, 6 and 7: up to three subsets, each a line with its own endpoints.
Result EncodeLines(Mode m, int shape) {
    Endpoints e[3];
    int idx[16];
    for (int i = 0; i < 16; ++i) {
        idx[i] = 0;
    }
    Result r;
    r.sse = 0;
    for (int k = 0; k < m.ns; ++k) {
        r.sse += EncodeSubset(m, shape, k, e[k], idx);
    }
    int anchors[3];
    for (int k = 0; k < 3; ++k) {
        anchors[k] = k < m.ns ? Anchor(m.ns, shape, k) : -1;
    }
    for (int k = 0; k < m.ns; ++k) {
        FixAnchor(m, e[k], idx, shape, k, anchors[k]);
    }
    for (int i = 0; i < 4; ++i) {
        r.bits.w[i] = 0u;
    }
    r.bits.pos = 0;
    Put(r.bits, 1u << uint(m.mode), m.mode + 1);
    if (m.ns > 1) {
        Put(r.bits, uint(shape), 6);
    }
    for (int c = 0; c < 3; ++c) {
        for (int k = 0; k < m.ns; ++k) {
            Put(r.bits, uint(e[k].q[c]), m.cb);
            Put(r.bits, uint(e[k].q[4 + c]), m.cb);
        }
    }
    if (m.ab != 0) {
        for (int k = 0; k < m.ns; ++k) {
            Put(r.bits, uint(e[k].q[3]), m.ab);
            Put(r.bits, uint(e[k].q[7]), m.ab);
        }
    }
    if (m.epb != 0) {
        for (int k = 0; k < m.ns; ++k) {
            Put(r.bits, uint(e[k].p[0]), 1);
            Put(r.bits, uint(e[k].p[1]), 1);
        }
    }
    if (m.spb != 0) {
        for (int k = 0; k < m.ns; ++k) {
            Put(r.bits, uint(e[k].p[0]), 1);
        }
    }
    for (int i = 0; i < 16; ++i) {
        const bool anchor = i == anchors[0] || i == anchors[1] || i == anchors[2];
        Put(r.bits, uint(idx[i]), m.ib - (anchor ? 1 : 0));
    }
    return r;
}

// One channel with its own indices (modes 4 and 5): `bits`-bit endpoints, `ib`-bit indices.
int EncodeScalar(int v[16], int bits, int ib, int& qlo, int& qhi, int (&idx)[16]) {
    const int n = 1 << ib;
    float lo = 255.0f;
    float hi = 0.0f;
    for (int i = 0; i < 16; ++i) {
        lo = min(lo, float(v[i]));
        hi = max(hi, float(v[i]));
        idx[i] = 0;
    }
    qlo = 0;
    qhi = 0;
    int best = 1 << 30;
    for (int it = 0; it < 3; ++it) {
        const int q0 = Quantize(lo, bits, 0, 0);
        const int q1 = Quantize(hi, bits, 0, 0);
        const int e0 = Expand(q0, bits);
        const int e1 = Expand(q1, bits);
        int e = 0;
        int trial[16];
        for (int i = 0; i < 16; ++i) {
            int bj = 0;
            int be = 1 << 30;
            for (int j = 0; j < n; ++j) {
                const int d = Interpolate(e0, e1, Weight(ib, j)) - v[i];
                if (d * d < be) {
                    be = d * d;
                    bj = j;
                }
            }
            trial[i] = bj;
            e += be;
        }
        if (e >= best) {
            break;
        }
        best = e;
        qlo = q0;
        qhi = q1;
        for (int i = 0; i < 16; ++i) {
            idx[i] = trial[i];
        }
        float aa = 0.0f;
        float ab = 0.0f;
        float bb = 0.0f;
        float ax = 0.0f;
        float bx = 0.0f;
        for (int i = 0; i < 16; ++i) {
            const float w = float(Weight(ib, trial[i])) / 64.0f;
            const float a = 1.0f - w;
            aa += a * a;
            ab += a * w;
            bb += w * w;
            ax += a * float(v[i]);
            bx += w * float(v[i]);
        }
        const float det = aa * bb - ab * ab;
        if (abs(det) < 1e-6f) {
            break;
        }
        lo = clamp((ax * bb - bx * ab) / det, 0.0f, 255.0f);
        hi = clamp((bx * aa - ax * ab) / det, 0.0f, 255.0f);
    }
    return best;
}

// Modes 4 and 5: an RGB line plus one channel (rotated into alpha) with its own indices.
// Mode 4: colour 5-bit, the channel 6-bit, one of them 3-bit indices (idxsel). Mode 5: colour
// 7-bit, the channel 8-bit, both 2-bit indices.
Result EncodeSeparate(int mode, int rotation, int idxsel) {
    int saved[16][4];
    int scalar[16];
    for (int i = 0; i < 16; ++i) {
        for (int c = 0; c < 4; ++c) {
            saved[i][c] = texel[i][c];
        }
        if (rotation != 0) {
            const int t = texel[i][rotation - 1];
            texel[i][rotation - 1] = texel[i][3];
            texel[i][3] = t;
        }
        scalar[i] = texel[i][3];
        texel[i][3] = 255;
    }
    const int cbits = mode == 4 ? 5 : 7;
    const int sbits = mode == 4 ? 6 : 8;
    const int cib = mode == 4 && idxsel != 0 ? 3 : 2;
    const int sib = mode == 4 && idxsel == 0 ? 3 : 2;
    const Mode m = MakeMode(mode, 1, cbits, 0, 0, 0, cib);
    Endpoints e;
    int idx[16];
    for (int i = 0; i < 16; ++i) {
        idx[i] = 0;
    }
    Result r;
    r.sse = EncodeSubset(m, 0, 0, e, idx);
    for (int i = 0; i < 16; ++i) {
        for (int c = 0; c < 4; ++c) {
            texel[i][c] = saved[i][c];
        }
    }
    int qlo;
    int qhi;
    int sidx[16];
    r.sse += EncodeScalar(scalar, sbits, sib, qlo, qhi, sidx);
    FixAnchor(m, e, idx, 0, 0, 0);
    if ((sidx[0] & (1 << (sib - 1))) != 0) {
        const int t = qlo;
        qlo = qhi;
        qhi = t;
        for (int i = 0; i < 16; ++i) {
            sidx[i] = (1 << sib) - 1 - sidx[i];
        }
    }
    for (int i = 0; i < 4; ++i) {
        r.bits.w[i] = 0u;
    }
    r.bits.pos = 0;
    Put(r.bits, 1u << uint(mode), mode + 1);
    Put(r.bits, uint(rotation), 2);
    if (mode == 4) {
        Put(r.bits, uint(idxsel), 1);
    }
    for (int c = 0; c < 3; ++c) {
        Put(r.bits, uint(e.q[c]), cbits);
        Put(r.bits, uint(e.q[4 + c]), cbits);
    }
    Put(r.bits, uint(qlo), sbits);
    Put(r.bits, uint(qhi), sbits);
    // The 2-bit index block comes first, then the 3-bit one (mode 4).
    const bool colour_first = !(mode == 4 && idxsel != 0);
    for (int i = 0; i < 16; ++i) {
        const int bits = colour_first ? cib : sib;
        Put(r.bits, uint(colour_first ? idx[i] : sidx[i]), bits - (i == 0 ? 1 : 0));
    }
    for (int i = 0; i < 16; ++i) {
        const int bits = colour_first ? sib : cib;
        Put(r.bits, uint(colour_first ? sidx[i] : idx[i]), bits - (i == 0 ? 1 : 0));
    }
    return r;
}

// The rotation (0: alpha, 1-3: R, G or B) whose channel set apart leaves the other three
// channels nearest a line.
int BestRotation() {
    float sum[4];
    float prod[10];
    for (int c = 0; c < 4; ++c) {
        sum[c] = 0.0f;
    }
    for (int c = 0; c < 10; ++c) {
        prod[c] = 0.0f;
    }
    for (int i = 0; i < 16; ++i) {
        int j = 0;
        for (int a = 0; a < 4; ++a) {
            const float va = float(texel[i][a]);
            sum[a] += va;
            for (int b = a; b < 4; ++b) {
                prod[j] += va * float(texel[i][b]);
                ++j;
            }
        }
    }
    int best = 0;
    float best_error = 1e30f;
    for (int rotation = 0; rotation < 4; ++rotation) {
        const int apart = rotation == 0 ? 3 : rotation - 1;
        // The other three channels' sums and products, in LineScatter's order.
        int keep[3];
        int n = 0;
        for (int c = 0; c < 4; ++c) {
            if (c != apart) {
                keep[n] = c;
                ++n;
            }
        }
        float sub_sum[4];
        float sub_prod[10];
        for (int c = 0; c < 10; ++c) {
            sub_prod[c] = 0.0f;
        }
        sub_sum[3] = 0.0f;
        int k = 0;
        for (int a = 0; a < 4; ++a) {
            if (a < 3) {
                sub_sum[a] = sum[keep[a]];
            }
            for (int b = a; b < 4; ++b) {
                if (a < 3 && b < 3) {
                    const int x = min(keep[a], keep[b]);
                    const int y = max(keep[a], keep[b]);
                    // Index of (x, y), x <= y, among the 10 products of 4 channels.
                    sub_prod[k] = prod[x * 4 - (x * (x - 1)) / 2 + (y - x)];
                }
                ++k;
            }
        }
        const float e = LineScatter(16.0f, sub_sum, sub_prod, 3);
        if (e < best_error) {
            best_error = e;
            best = rotation;
        }
    }
    return best;
}

uvec4 EncodeBlock() {
    bool opaque = true;
    for (int i = 0; i < 16; ++i) {
        if (texel[i][3] != 255) {
            opaque = false;
        }
    }
    Result best = EncodeLines(MakeMode(6, 1, 7, 7, 1, 0, 4), 0);
    if (best.sse > SKIP_THRESHOLD) {
        if (opaque) {
            const int p2 = BestPartition(2, false);
            Result r = EncodeLines(MakeMode(1, 2, 6, 0, 0, 1, 3), p2);
            if (r.sse < best.sse) {
                best = r;
            }
            r = EncodeLines(MakeMode(3, 2, 7, 0, 1, 0, 2), p2);
            if (r.sse < best.sse) {
                best = r;
            }
            r = EncodeLines(MakeMode(2, 3, 5, 0, 0, 0, 2), BestPartition(3, false));
            if (r.sse < best.sse) {
                best = r;
            }
        } else {
            // Modes 4 and 5 for the channel that, set apart, leaves the other three nearest a
            // line (alpha, or a mask packed into one of R, G, B).
            const int rotation = BestRotation();
            Result r = EncodeSeparate(5, rotation, 0);
            if (r.sse < best.sse) {
                best = r;
            }
            for (int idxsel = 0; idxsel < 2; ++idxsel) {
                r = EncodeSeparate(4, rotation, idxsel);
                if (r.sse < best.sse) {
                    best = r;
                }
            }
        }
    }
    return uvec4(best.bits.w[0], best.bits.w[1], best.bits.w[2], best.bits.w[3]);
}


} // Anonymous namespace

void EncodeBc7Block(u8* output, const u8* input) {
    for (int i = 0; i < 16; ++i) {
        for (int c = 0; c < 4; ++c) {
            texel[i][c] = input[i * 4 + c];
        }
    }
    const uvec4 block = EncodeBlock();
    const uint words[4] = {block.x, block.y, block.z, block.w};
    std::memcpy(output, words, sizeof(words));
}

} // namespace Tegra::Texture::BCN
