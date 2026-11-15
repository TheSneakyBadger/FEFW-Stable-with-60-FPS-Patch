// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

// BC7 block encoder for the CPU, the same code as host_shaders/bc7_encoder.comp (between its
// BEGIN/END BC7 CORE markers) with a few shims for GLSL's types and built-ins, so a texture comes
// out the same whichever path compresses it. Keep the two in step.

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

// Blocks whose mode 6 error (sum of squares over the 16 texels' channels) is at most this skip
// the multi-group modes: a little quality for much less work on smooth blocks.
const int SKIP_THRESHOLD = 300;

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
    for (int i = 0; i < count; ++i) {
        b.w[b.pos >> 5] |= ((value >> uint(i)) & 1u) << uint(b.pos & 31);
        ++b.pos;
    }
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
    const int target = int(round(v));
    const int top = (1 << cb) - 1;
    const int guess = int(round(v / 255.0f * float(top)));
    int best = 0;
    int best_error = 1 << 30;
    for (int q = max(0, guess - 2); q <= min(top, guess + 2); ++q) {
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
// the summed squared error.
int AssignIndices(Mode m, int colour[8], int shape, int sub, int (&idx)[16]) {
    const int n = 1 << m.ib;
    int palette[16][4];
    for (int j = 0; j < n; ++j) {
        const int w = Weight(m.ib, j);
        for (int c = 0; c < 4; ++c) {
            palette[j][c] = Interpolate(colour[c], colour[4 + c], w);
        }
    }
    int sse = 0;
    for (int i = 0; i < 16; ++i) {
        if (Subset(m.ns, shape, i) != sub) {
            continue;
        }
        int best = 0;
        int best_error = 1 << 30;
        for (int j = 0; j < n; ++j) {
            int e = 0;
            for (int c = 0; c < 4; ++c) {
                const int d = palette[j][c] - texel[i][c];
                e += d * d;
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

// The subset's principal axis through its mean, from the lowest to the highest projection.
void FitLine(int ns, int shape, int sub, bool alpha, float (&lo)[4], float (&hi)[4]) {
    float mean[4];
    for (int c = 0; c < 4; ++c) {
        mean[c] = 0.0f;
    }
    int count = 0;
    for (int i = 0; i < 16; ++i) {
        if (Subset(ns, shape, i) == sub) {
            for (int c = 0; c < 4; ++c) {
                mean[c] += float(texel[i][c]);
            }
            ++count;
        }
    }
    for (int c = 0; c < 4; ++c) {
        mean[c] /= float(max(count, 1));
    }
    const int nc = alpha ? 4 : 3;
    float cov[4][4];
    for (int a = 0; a < 4; ++a) {
        for (int b = 0; b < 4; ++b) {
            cov[a][b] = 0.0f;
        }
    }
    for (int i = 0; i < 16; ++i) {
        if (Subset(ns, shape, i) != sub) {
            continue;
        }
        for (int a = 0; a < nc; ++a) {
            for (int b = 0; b < nc; ++b) {
                cov[a][b] += (float(texel[i][a]) - mean[a]) * (float(texel[i][b]) - mean[b]);
            }
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
    for (int it = 0; it < 8; ++it) {
        float t[4];
        float l = 0.0f;
        for (int a = 0; a < 4; ++a) {
            t[a] = 0.0f;
            for (int b = 0; b < nc; ++b) {
                t[a] += cov[a][b] * axis[b];
            }
            l = max(l, abs(t[a]));
        }
        if (l < 1e-6f) {
            break;
        }
        for (int c = 0; c < 4; ++c) {
            axis[c] = t[c] / l;
        }
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

// Quantises the line to the mode, trying its p-bit choices; returns the subset's error.
int QuantizeSubset(Mode m, float lo[4], float hi[4], int shape, int sub,
                   Endpoints& best, int (&idx)[16]) {
    const int pb = m.epb + m.spb;
    const int combos = m.epb != 0 ? 4 : (m.spb != 0 ? 2 : 1);
    // Opaque texels need p-bits of 1 to keep alpha exactly 255 (no faint see-through).
    bool opaque = true;
    if (m.ab != 0) {
        for (int i = 0; i < 16; ++i) {
            if (Subset(m.ns, shape, i) == sub && texel[i][3] != 255) {
                opaque = false;
            }
        }
    }
    int best_error = 1 << 30;
    int trial[16];
    for (int i = 0; i < 16; ++i) {
        trial[i] = idx[i];
    }
    for (int k = 0; k < 4; ++k) {
        if (k >= combos || (m.ab != 0 && opaque && k != combos - 1)) {
            continue;
        }
        Endpoints e;
        e.p[0] = k & 1;
        e.p[1] = m.epb != 0 ? (k >> 1) : (k & 1);
        for (int c = 0; c < 3; ++c) {
            e.q[c] = Quantize(lo[c], m.cb, pb, e.p[0]);
            e.q[4 + c] = Quantize(hi[c], m.cb, pb, e.p[1]);
        }
        e.q[3] = m.ab != 0 ? Quantize(lo[3], m.ab, pb, e.p[0]) : 0;
        e.q[7] = m.ab != 0 ? Quantize(hi[3], m.ab, pb, e.p[1]) : 0;
        int colour[8];
        DecodeEndpoints(m, e, colour);
        const int sse = AssignIndices(m, colour, shape, sub, trial);
        if (sse < best_error) {
            best_error = sse;
            best = e;
            for (int i = 0; i < 16; ++i) {
                if (Subset(m.ns, shape, i) == sub) {
                    idx[i] = trial[i];
                }
            }
        }
    }
    return best_error;
}

// Least-squares endpoints for the subset's current indices.
bool RefineLine(Mode m, int shape, int sub, int idx[16], float (&lo)[4], float (&hi)[4]) {
    float aa = 0.0f;
    float ab = 0.0f;
    float bb = 0.0f;
    float ax[4];
    float bx[4];
    for (int c = 0; c < 4; ++c) {
        ax[c] = 0.0f;
        bx[c] = 0.0f;
    }
    for (int i = 0; i < 16; ++i) {
        if (Subset(m.ns, shape, i) != sub) {
            continue;
        }
        const float w = float(Weight(m.ib, idx[i])) / 64.0f;
        const float a = 1.0f - w;
        aa += a * a;
        ab += a * w;
        bb += w * w;
        for (int c = 0; c < 4; ++c) {
            ax[c] += a * float(texel[i][c]);
            bx[c] += w * float(texel[i][c]);
        }
    }
    const float det = aa * bb - ab * ab;
    for (int c = 0; c < 4; ++c) {
        lo[c] = 0.0f;
        hi[c] = 0.0f;
    }
    if (abs(det) < 1e-6f) {
        return false;
    }
    for (int c = 0; c < 4; ++c) {
        lo[c] = clamp((ax[c] * bb - bx[c] * ab) / det, 0.0f, 255.0f);
        hi[c] = clamp((bx[c] * aa - ax[c] * ab) / det, 0.0f, 255.0f);
    }
    if (m.ab == 0) {
        lo[3] = 255.0f;
        hi[3] = 255.0f;
    }
    return true;
}

int EncodeSubset(Mode m, int shape, int sub, Endpoints& e, int (&idx)[16]) {
    float lo[4];
    float hi[4];
    FitLine(m.ns, shape, sub, m.ab != 0, lo, hi);
    int sse = QuantizeSubset(m, lo, hi, shape, sub, e, idx);
    for (int it = 0; it < 2 && sse > 0; ++it) {
        float lo2[4];
        float hi2[4];
        if (!RefineLine(m, shape, sub, idx, lo2, hi2)) {
            break;
        }
        Endpoints e2;
        int idx2[16];
        for (int i = 0; i < 16; ++i) {
            idx2[i] = idx[i];
        }
        const int sse2 = QuantizeSubset(m, lo2, hi2, shape, sub, e2, idx2);
        if (sse2 >= sse) {
            break;
        }
        sse = sse2;
        e = e2;
        for (int i = 0; i < 16; ++i) {
            idx[i] = idx2[i];
        }
    }
    return sse;
}

// How far the subset's texels sit from its principal line (squared), to rank partitions.
float LineError(int ns, int shape, int sub, bool alpha) {
    float lo[4];
    float hi[4];
    FitLine(ns, shape, sub, alpha, lo, hi);
    const int nc = alpha ? 4 : 3;
    float d[4];
    float l2 = 0.0f;
    for (int c = 0; c < 4; ++c) {
        d[c] = hi[c] - lo[c];
        if (c < nc) {
            l2 += d[c] * d[c];
        }
    }
    float error = 0.0f;
    for (int i = 0; i < 16; ++i) {
        if (Subset(ns, shape, i) != sub) {
            continue;
        }
        float v[4];
        float t = 0.0f;
        for (int c = 0; c < nc; ++c) {
            v[c] = float(texel[i][c]) - lo[c];
            t += v[c] * d[c];
        }
        t = l2 > 0.0f ? clamp(t / l2, 0.0f, 1.0f) : 0.0f;
        for (int c = 0; c < nc; ++c) {
            const float r = v[c] - t * d[c];
            error += r * r;
        }
    }
    return error;
}

int BestPartition(int ns, bool alpha) {
    int best = 0;
    float best_error = 1e30f;
    for (int p = 0; p < 64; ++p) {
        float e = 0.0f;
        for (int k = 0; k < ns; ++k) {
            e += LineError(ns, p, k, alpha);
        }
        if (e < best_error) {
            best_error = e;
            best = p;
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
            for (int rotation = 0; rotation < 4; ++rotation) {
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
            const Result r = EncodeLines(MakeMode(7, 2, 5, 5, 1, 0, 2), BestPartition(2, true));
            if (r.sse < best.sse) {
                best = r;
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
