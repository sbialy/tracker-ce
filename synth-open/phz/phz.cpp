// PHZ: clean-room phase distortion engine (see phz.h).
//
// Provenance. Parameter set and pages: public Polyend Synth manual SYN2 p.76-80 (ref/engine-specs.md section 5). The
// phase distortion models, the Character rows and every default are our own design, the manual gives no formulas.
// Nothing of Polyend's closed libraries was read. The block-rate voice skeleton below (envelope / LFO / glide stepping,
// the destination ramp scheme, the TPT state variable stage and the filter flavours, fastExp2 / blockCoef / fastTan /
// lfoShape) follows OpenTracker's VAP engine (synth-open/ot/audio/engines/vap.cpp, Apache-2.0, Copyright OpenTracker
// authors, see licenses/LICENSE-OpenTracker-Apache-2.0.txt), trimmed and adapted. Mutable Instruments Plaits
// (github.com/pichenettes/eurorack, MIT, Copyright 2012 Emilie Gillet) was NOT copied: its phase_distortion_engine.cc
// is the same family of idea (a distorted phase reads a sine) but its code is not in this file, so no MIT text is
// carried. Should Plaits code be added later, its notice must go here and in NOTICE.md.
//
// Oscillator. One phase ramp per oscillator, pushed through a phase distortion map f(phase; X, Y) (the "Model") and
// read by a cosine based cycle (the "Character" row, positioned by "Shape"). With the identity map the oscillator is a
// cosine. f(0) = 0 and f(1) = 1 (mod 1) for every model, so the cycle is continuous. Tuned for 44.1 kHz and a 128
// frame block: the maps are closed form (no tables, no division per sample), the cosine is a parabola with one
// correction step, X / Y / Shape / mix ramp linearly across the block (the derived coefficients are ramped, not
// recomputed per sample) and the steepness of every map is limited as the pitch rises, which is the only anti
// aliasing (no BLEP, no oversampling: a hard setting on a high note still aliases, see the report).
//
// Two passes per oscillator keep the code small: pass 1 (one kernel per model) writes the distorted phase, pass 2
// (one kernel per Character kind) reads the cycle and adds it, weighted, into the voice buffer.
#include "phz/phz.h"

#include <cstring>

#include "audio/dsp.h"

// Per block (not per sample) code is built for size: the file is built -O2 for the sample loops, flash matters more
// than the few hundred cycles per block this code runs (same split as VAP_PER_BLOCK in the OpenTracker engines).
#if defined(__GNUC__) && !defined(__clang__)
#define PHZ_PER_BLOCK __attribute__((optimize("Os")))
#else
#define PHZ_PER_BLOCK
#endif

namespace ot {
namespace audio {
namespace phz {

namespace {

constexpr float kOutTrim = 0.70710678f;   // -3 dB voice output (two cosines in phase peak at 1, the filter adds more)
constexpr float kBlockT = float(kBlock) / kSampleRate;   // seconds per block
constexpr float kInvFs = 1.0f / kSampleRate;
constexpr float kPi = 3.14159265f;
constexpr float kLog2e = 1.44269504f;
constexpr float kCutoffOct = 10.0f;    // Filter Env Amount / cutoff destination: 1 = 10 octaves
constexpr float kOneShotEnd = 0.99999f;
constexpr float kMaxSlope = 0.15f;     // largest output cycles per sample a knee map may reach (aliasing guard)
constexpr float kMaxSlopeFm = 0.07f;   // the same for the Hermite FM deviation (a wider spectrum per slope)
constexpr float kCutoffOff = 19990.0f; // Cutoff at the top = "Off" (unfiltered), SYN2 filter page

// ---------------------------------------------------------------------------
// Small math (no libm in the audio path).

inline float clampf(float x, float lo, float hi) { return x < lo ? lo : (x > hi ? hi : x); }
inline float absf(float x) { return __builtin_fabsf(x); }
inline int enumOf(float x, int max) {
    const int i = int(x + 0.5f);
    return i < 0 ? 0 : (i > max ? max : i);
}

// 2^x for |x| <= 60 (relative error < 3e-6).
inline float fastExp2(float x) {
    x = clampf(x, -60.0f, 60.0f);
    const int i = int(x + (x >= 0.0f ? 0.5f : -0.5f));
    const float f = x - float(i);
    const float p = 1.0f + f * (0.693147181f + f * (0.240226507f + f * (0.0555041087f + f * (0.00961812911f + f * 0.00133335581f))));
    const uint32_t b = uint32_t(i + 127) << 23;
    float s;
    std::memcpy(&s, &b, 4);
    return p * s;
}

// Per-block decay factor reaching about 1 % (-40 dB) after t seconds.
inline float blockCoef(float t) {
    if (t <= 0.0005f) return 0.0f;
    return fastExp2(-4.6f * kLog2e * kBlockT / t);
}

// tan(x) for 0 <= x <= 1.42 (rational approximation, a slightly low cutoff near Nyquist only).
inline float fastTan(float x) {
    const float x2 = x * x;
    return x * (15.0f - x2) / (15.0f - 6.0f * x2);
}

inline uint32_t lcg(uint32_t& r) {
    r = r * 1664525u + 1013904223u;
    return r;
}
inline float rnd(uint32_t& r) { return float(int32_t(lcg(r))) * (1.0f / 2147483648.0f); }   // -1..1

// Soft clip: unity slope at 0, flat at +-3 -> +-2.
inline float softClip2(float x) {
    x = 0.5f * (absf(x + 3.0f) - absf(x - 3.0f));
    return x - (1.0f / 27.0f) * x * x * x;
}

#define PHZ_INLINE inline __attribute__((always_inline))

// Fractional part of v for -64 < v < 1000 (the offset keeps the int cast away from negative values).
PHZ_INLINE float frac64(float v) {
    v += 64.0f;
    return v - float(int(v));
}

// cos(2 pi x) for x in [0, 1): the triangle t = 4 |x - 0.5| - 1 (t = 1 at x = 0) is linear in x, and cos(2 pi x) =
// sin(pi t / 2), which an odd quintic with a flat top (p(1) = 1, p'(1) = 0) follows to 2e-4. Harmonics of the result
// are below -78 dB. Fitted here (minimax over t in [0, 1]), not taken from anywhere.
PHZ_INLINE float triOf(float x) { return 4.0f * absf(x - 0.5f) - 1.0f; }
PHZ_INLINE float cosTri(float t) {
    const float t2 = t * t;
    return t * (1.5697f + t2 * (-0.6394f + t2 * 0.0697f));
}
PHZ_INLINE float cosP(float v) { return cosTri(triOf(frac64(v))); }
// sin(2 pi v)
PHZ_INLINE float sinP(float v) { return cosP(v - 0.25f); }
// sin(2 pi v) with a cubic (3rd harmonic -38 dB), for modulators that need no purity
PHZ_INLINE float sinCheap(float v) {
    const float t = triOf(frac64(v - 0.25f));
    return t * (1.5f - 0.5f * t * t);
}

// ---------------------------------------------------------------------------
// Engine state.

constexpr int kModSlots = 6;
constexpr int kEnvs = 3;   // 0 Amp, 1 Filter, 2 Aux
enum EnvStage : uint8_t { kIdle = 0, kAttack, kDecay, kRelease };
enum LfoRetrig : int { kFree = 0, kOneShot, kNote };
enum LfoWave : int { kTri = 0, kSine, kRampUp, kRampDown, kSquare, kSampleHold };

// LFO Sync divisions (Ratio rounded, 0..15): quarter notes per LFO cycle.
constexpr float kSyncBeats[16] = {16.0f, 12.0f, 8.0f, 6.0f, 4.0f, 3.0f, 2.0f, 1.5f,
                                  1.0f, 0.75f, 0.5f, 1.0f / 3.0f, 0.25f, 1.0f / 6.0f, 0.125f, 1.0f / 12.0f};
float gTempo = 120.0f;

// Read-stage partial numbers of Character Add 1, 2, 3, 5, 7, 11
constexpr float kAddN[6] = {1.0f, 2.0f, 3.0f, 5.0f, 7.0f, 11.0f};

struct Mod {
    uint8_t src, dst;
    float a0, a1;   // amount at block start / end
};

// Slot scratch (per-block caches shared by the slot's voices).
struct SlotState {
    uint32_t serial;
    uint8_t valid;
    uint8_t hasLast;     // lastPitch is set (glide source of a fresh voice)
    uint8_t nmod;
    uint8_t ftype;       // Filter Type 0..14
    uint8_t model, char1, char2, fltOff;
    uint8_t lfoWave[2], lfoRetrig[2];
    uint8_t glideMode, reset1, reset2;
    float envInc[kEnvs], envCd[kEnvs], envCr[kEnvs];   // attack step, decay / release factors (p.end)
    float glideCoef;
    float lfoInc[2];                  // phase per block
    float lfoPh0[2], lfoPh1[2];       // free-running phase at block start / end
    float lfoHold0[2], lfoHold1[2];   // free-running S&H value at block start / end
    uint32_t rng;
    float lastPitch;
    Mod mod[kModSlots];
};
static_assert(sizeof(SlotState) <= kSynthSlotScratchBytes, "PHZ slot scratch");

// Destination values at one instant (block start or end). Controls, not derived coefficients: the map coefficients
// (with their divisions) are derived from these for both ends of the block.
struct Dest {
    float dt1, dt2;        // phase increments
    float x1, y1, x2, y2;  // phase distortion X / Y per oscillator
    float sh1, sh2;        // Shape
    float w1, w2;          // oscillator weights (Mix and the filter input gain included)
    float g, k;            // filter: tan(pi fc / fs), damping of the resonant stage
    float amp;             // amp envelope x Volume destination
};

struct VoiceState {
    float ph1, ph2;        // oscillator phases 0..1
    float pitch, target;   // glided note pitch, its target (semitones)
    float f[4];            // filter states: stage A s1, s2; stage B s1, s2
    float env[kEnvs];
    float lfoPh[2];        // per-voice LFO phase (One Shot / Note)
    float lfoHold[2];      // per-voice S&H value
    uint32_t rng;
    Dest last;             // destinations at the end of the last block (= this block's start)
    uint8_t stage[kEnvs];
    uint8_t started;
    uint8_t lastValid;     // `last` holds the previous block's end (cleared by noteOn)
};
static_assert(sizeof(VoiceState) <= kSynthVoiceStateBytes, "PHZ voice state");

// ---------------------------------------------------------------------------
// LFO waveforms (phase 0..1 -> -1..1).

PHZ_PER_BLOCK __attribute__((noinline)) float lfoShape(int wave, float ph, float hold) {
    switch (wave) {
        case kTri: return ph < 0.25f ? 4.0f * ph : (ph < 0.75f ? 2.0f - 4.0f * ph : 4.0f * ph - 4.0f);
        case kSine: return sinP(ph);
        case kRampUp: return 2.0f * ph - 1.0f;
        case kRampDown: return 1.0f - 2.0f * ph;
        case kSquare: return ph < 0.5f ? 1.0f : -1.0f;
        default: return hold;
    }
}

// ---------------------------------------------------------------------------
// Filter types (SYN2 order, the same 15 as the VAP list).

enum FMode : uint8_t { kLP = 0, kHP, kBP, kNotch };
enum FFlavour : uint8_t { kSVF = 0, kOB, kMG };
struct FType {
    uint8_t mode, flavour, poles24;
};
constexpr FType kFTypes[15] = {
    {kLP, kMG, 1},  {kLP, kOB, 1},  {kLP, kOB, 0},  {kLP, kSVF, 1}, {kLP, kSVF, 0},
    {kHP, kOB, 1},  {kHP, kOB, 0},  {kHP, kSVF, 1}, {kHP, kSVF, 0},
    {kBP, kOB, 1},  {kBP, kOB, 0},  {kBP, kSVF, 1}, {kBP, kSVF, 0},
    {kNotch, kSVF, 1}, {kNotch, kSVF, 0},
};
// Damping of the first stage of a 24 dB type: Butterworth-like for OB / SVF, two coincident real poles for MG.
constexpr float kStage1K = 1.848f;
constexpr float kStage1KMG = 2.0f;

// ---------------------------------------------------------------------------
// Phase distortion maps. A Pd holds the coefficients of one map for one instant, five floats at most.
//
//   OG Saw          knee on the line y = 0.5 at x = (1 - X) / 2, so X 0 is the identity (sine) and X 1 a saw like
//                   cycle through a cosine (the CZ saw). Y skews the input phase (left / right bend), 0.5 = none.
//   OG Square       the first and the second half cycle each run 0 -> 0.5 and 0.5 -> 1 in a window of width
//                   (1 - X) / 2 and then hold: a square like cycle. Y skews as above.
//   Hermite Smooth  linear map with a knee at (X, Y) whose pieces are blended with a cubic Hermite (smoothstep)
//                   term, slope 1 at both ends and at the knee: no corner, the warmest.
//   Squared         knee at (X, Y), each piece a square (ease in, then ease out): flat at both ends.
//   Linear          knee at (X, Y): two straight pieces. X = Y is the identity, (0, 1) and (1, 0) are both a
//                   near constant cycle (the mirror symmetry the manual mentions).
//   Hermite FM      X skews the phase, Y adds a cubic (sine like) Hermite phase deviation of up to 3 cycles, so the
//                   mapped phase wraps the cosine several times per cycle: FM like.

struct Pd {
    float a, b, c, d, e;
};

inline Pd kneePd(float a, float b, float c, float smax) {
    // keep both slopes b / a and (1 - b) / (1 - a) below smax by moving the knee
    a = a < b / smax ? b / smax : a;
    const float hi = 1.0f - (1.0f - b) / smax;
    a = a > hi ? hi : a;
    a = clampf(a, 0.01f, 0.99f);
    Pd p;
    p.a = a;
    p.b = b;
    p.c = c;
    p.d = b / a;
    p.e = (1.0f - b) / (1.0f - a);
    return p;
}

PHZ_PER_BLOCK Pd derivePd(int model, float X, float Y, float dt) {
    const float smax = kMaxSlope / dt;
    const float skew = 0.9f * (2.0f * Y - 1.0f);
    Pd p = {};
    switch (model) {
        case kOgSaw: return kneePd(0.5f * (1.0f - X), 0.5f, skew, smax);
        case kOgSquare: {
            float k = 1.0f / (1.0f - 0.97f * X);   // slope of the edges
            p.a = k > smax ? smax : k;
            p.c = skew;
            return p;
        }
        case kHermiteSmooth: {
            const float b = clampf(Y, 0.02f, 0.98f);
            const Pd k = kneePd(clampf(X, 0.02f, 0.98f), b, 0.0f, smax);
            p.a = k.a;
            p.b = b - k.a;   // knee offset from the identity
            p.c = 1.0f / k.a;
            p.d = 1.0f / (1.0f - k.a);
            return p;
        }
        case kSquared: {
            const float b = clampf(Y, 0.02f, 0.98f);
            const Pd k = kneePd(clampf(X, 0.02f, 0.98f), b, 0.0f, 0.5f * smax);   // the pieces reach 2 b / a
            p.a = k.a;
            p.b = b;
            p.c = 1.0f / k.a;
            p.d = 1.0f / (1.0f - k.a);
            p.e = 1.0f - b;
            return p;
        }
        case kLinear: return kneePd(clampf(X, 0.02f, 0.98f), clampf(Y, 0.02f, 0.98f), 0.0f, smax);
        default: {   // Hermite FM
            // Y 1 is 3 cycles of deviation, scaled down as the pitch rises (the deviation slope is 10.4 g)
            float gmax = (kMaxSlopeFm / dt - 1.0f) * (1.0f / 11.0f);
            gmax = gmax < 0.0f ? 0.0f : (gmax > 3.0f ? 3.0f : gmax);
            p.a = Y * gmax;
            p.c = 0.9f * (2.0f * X - 1.0f);
            return p;
        }
    }
}

template <int kModel>
inline float pdMap(float ph, const Pd& p) {
    if (kModel == kOgSaw || kModel == kLinear) {
        const float x = ph + p.c * ph * (1.0f - ph);
        return x < p.a ? x * p.d : p.b + (x - p.a) * p.e;
    } else if (kModel == kOgSquare) {
        const float x = ph + p.c * ph * (1.0f - ph);
        const bool hi = x >= 0.5f;
        float t = (hi ? x - 0.5f : x) * p.a;
        t = t > 0.5f ? 0.5f : t;
        return hi ? 0.5f + t : t;
    } else if (kModel == kHermiteSmooth) {
        const float u = ph < p.a ? ph * p.c : (1.0f - ph) * p.d;
        return frac64(ph + p.b * u * u * (3.0f - 2.0f * u));
    } else if (kModel == kSquared) {
        if (ph < p.a) {
            const float u = ph * p.c;
            return p.b * u * u;
        }
        const float v = (1.0f - ph) * p.d;
        return 1.0f - p.e * v * v;
    } else {   // Hermite FM
        const float x = ph + p.c * ph * (1.0f - ph);
        return frac64(x + p.a * 10.392f * x * (1.0f - x) * (1.0f - 2.0f * x));
    }
}

// Pass 1: advance the phase, write the mapped phase (always in [0, 1], the two maps that can leave it wrap it). The increment and the coefficients are stepped once per chunk of
// kChunk frames (dt / p are the values at the middle of the first chunk, ddt / dp the step per chunk): the per
// sample cost is the map alone.
constexpr uint32_t kChunk = 16;

template <int kModel>
OT_FASTRUN_FN void pdKernel(float* phi, float& phase, float dt, float ddt, Pd p, Pd dp) {
    float ph = phase;
    for (uint32_t c = 0; c < kBlock; c += kChunk) {
        for (uint32_t i = 0; i < kChunk; ++i) {
            ph += dt;
            ph -= ph >= 1.0f ? 1.0f : 0.0f;
            phi[c + i] = pdMap<kModel>(ph, p);
        }
        dt += ddt;
        p.a += dp.a;
        p.b += dp.b;
        p.c += dp.c;
        p.d += dp.d;
        p.e += dp.e;
    }
    phase = ph;
}

using PdFn = void (*)(float*, float&, float, float, Pd, Pd);
constexpr PdFn kPdFns[kModelCount] = {pdKernel<kOgSaw>,   pdKernel<kOgSquare>, pdKernel<kHermiteSmooth>,
                                      pdKernel<kSquared>, pdKernel<kLinear>,   pdKernel<kHermiteFm>};

// Pass 2: read the cycle at the mapped phase. Character kinds: 0 Smoother (cosine -> triangle), 1 Sharper (cosine
// driven into a hard clip), 2 Wilder (cosine phase modulated by its third harmonic), 3 Add N (cosine plus its
// N-th harmonic). Shape (0..1) moves along the row, w is the oscillator weight. Both are stepped once per chunk
// (values at the middle of the first chunk, step per chunk in dw / ds).
template <int kKind>
OT_FASTRUN_FN void readKernel(float* out, const float* phi, float w, float dw, float s, float ds, float n) {
    for (uint32_t c = 0; c < kBlock; c += kChunk) {
        const float k = 1.0f + 7.0f * s, m = 0.45f * s, sc = 1.0f - 0.5f * s;
        for (uint32_t i = c; i < c + kChunk; ++i) {
            const float v = phi[i];   // in [0, 1]
            float y;
            if (kKind == 0) {
                const float t = triOf(v);
                y = cosTri(t);
                y += s * (t - y);
            } else if (kKind == 1) {
                const float z = cosTri(triOf(v)) * k;
                y = 0.5f * (absf(z + 1.0f) - absf(z - 1.0f));
            } else if (kKind == 2) {
                y = cosP(v + m * sinCheap(3.0f * v));
            } else {
                y = (cosP(v) + s * sinP(n * v)) * sc;
            }
            out[i] += w * y;
        }
        w += dw;
        s += ds;
    }
}

using ReadFn = void (*)(float*, const float*, float, float, float, float, float);
constexpr ReadFn kReadFns[4] = {readKernel<0>, readKernel<1>, readKernel<2>, readKernel<3>};

// ---------------------------------------------------------------------------
// Filter kernels: one TPT SVF stage (Simper's form) over buf.
//   kSat: 0 none, 2 soft clip after (OB).
//   kOut: add y * gain into L / R instead of writing buf.

struct SvfRamp {
    float a1, da1, a2, da2, a3, da3, k, dk;
};

inline SvfRamp svfRamp(float g0, float k0, float g1, float k1) {
    constexpr float n = 1.0f / float(kBlock / kChunk);   // chunk steps per block
    const float a10 = 1.0f / (1.0f + g0 * (g0 + k0)), a11 = 1.0f / (1.0f + g1 * (g1 + k1));
    const float a20 = g0 * a10, a21 = g1 * a11;
    const float a30 = g0 * a20, a31 = g1 * a21;
    const float d1 = (a11 - a10) * n, d2 = (a21 - a20) * n, d3 = (a31 - a30) * n, dk = (k1 - k0) * n;
    return SvfRamp{a10 + 0.5f * d1, d1, a20 + 0.5f * d2, d2, a30 + 0.5f * d3, d3, k0 + 0.5f * dk, dk};
}

template <int kMode, int kSat, bool kOut>
OT_FASTRUN_FN void svfKernel(float* buf, float* s, SvfRamp c, float* L, float* R, float g, float dg) {
    float s1 = s[0], s2 = s[1];
    for (uint32_t b = 0; b < kBlock; b += kChunk) {
        for (uint32_t i = b; i < b + kChunk; ++i) {
            const float x = buf[i];
            const float v3 = x - s2;
            const float v1 = c.a1 * s1 + c.a2 * v3;
            const float v2 = s2 + c.a2 * s1 + c.a3 * v3;
            s1 = 2.0f * v1 - s1;
            s2 = 2.0f * v2 - s2;
            float y;
            if (kMode == kLP) y = v2;
            else if (kMode == kHP) y = x - c.k * v1 - v2;
            else if (kMode == kBP) y = c.k * v1;
            else y = x - c.k * v1;
            if (kSat == 2) y = softClip2(y);
            if (kOut) {
                const float o = y * g;
                L[i] += o;
                R[i] += o;
                g += dg;
            } else {
                buf[i] = y;
            }
        }
        // the coefficients step once per chunk (svfRamp holds the step per chunk, starting at the middle of chunk 0)
        c.a1 += c.da1;
        c.a2 += c.da2;
        c.a3 += c.da3;
        if (kMode != kLP) c.k += c.dk;
    }
    s[0] = s1;
    s[1] = s2;
}

using SvfFn = void (*)(float*, float*, SvfRamp, float*, float*, float, float);
// First stage of a 24 dB type [mode]
constexpr SvfFn kSvfFirst[4] = {svfKernel<kLP, 0, false>, svfKernel<kHP, 0, false>, svfKernel<kBP, 0, false>,
                                svfKernel<kNotch, 0, false>};
// Last stage, into L / R [OB soft clip][mode] (there is no OB notch)
constexpr SvfFn kSvfLast[2][4] = {
    {svfKernel<kLP, 0, true>, svfKernel<kHP, 0, true>, svfKernel<kBP, 0, true>, svfKernel<kNotch, 0, true>},
    {svfKernel<kLP, 2, true>, svfKernel<kHP, 2, true>, svfKernel<kBP, 2, true>, svfKernel<kBP, 2, true>},
};

// Cutoff "Off": no filter, the buffer goes straight to the output (the cheap path)
OT_FASTRUN_FN void outKernel(const float* buf, float* L, float* R, float g, float dg) {
    for (uint32_t i = 0; i < kBlock; ++i) {
        const float o = buf[i] * g;
        L[i] += o;
        R[i] += o;
        g += dg;
    }
}

// ---------------------------------------------------------------------------
// Destinations.

inline float pitchToDt(float pitch) {
    return clampf(440.0f * kInvFs * fastExp2((pitch - 69.0f) * (1.0f / 12.0f)), 1.0e-6f, 0.45f);
}

// src[]: Amp Env, Filter Env, Aux Env, LFO 1, LFO 2. atEnd selects the mod amount.
PHZ_PER_BLOCK void evalDest(const float* P, const SlotState& S, const float* src, bool atEnd, float pitch, Dest& d) {
    float m[kDstCount];
    for (int j = 0; j < kDstCount; ++j) {
        OT_NO_LIBCALL();
        m[j] = 0.0f;
    }
    for (int j = 0; j < S.nmod; ++j) {
        const Mod& md = S.mod[j];
        m[md.dst] += (atEnd ? md.a1 : md.a0) * src[md.src];
    }
    // Oscillators
    d.x1 = clampf(P[kX1] + m[kDstX1], 0.0f, 1.0f);
    d.x2 = clampf(P[kX2] + m[kDstX2], 0.0f, 1.0f);
    d.y1 = clampf(P[kY1] + m[kDstY1], 0.0f, 1.0f);
    d.y2 = clampf(P[kY2] + m[kDstY2], 0.0f, 1.0f);
    d.sh1 = clampf(P[kShape1] + m[kDstShape1], 0.0f, 1.0f);
    d.sh2 = clampf(P[kShape2] + m[kDstShape2], 0.0f, 1.0f);
    const float mix = clampf(P[kMix] + m[kDstMix], 0.0f, 1.0f);
    const float fine = 0.01f * P[kFinetune], det = 0.005f * P[kDetune];
    d.dt1 = pitchToDt(pitch + P[kTune1] + fine - det + 12.0f * m[kDstPitch1]);
    d.dt2 = pitchToDt(pitch + P[kTune2] + fine + det + 12.0f * m[kDstPitch2]);
    // Filter
    const float oct = kCutoffOct * (P[kFilterEnvAmt] * src[kSrcFltEnv] + m[kDstCutoff]) + P[kNoteTrack] * (pitch - 60.0f) * (1.0f / 12.0f);
    const float fc = clampf(P[kCutoff] * fastExp2(oct), 16.0f, 0.45f * kSampleRate);
    d.g = fastTan(kPi * fc * kInvFs);
    const float r = clampf(P[kResonance] + m[kDstResonance], 0.0f, 1.0f);
    d.k = 2.0f * (1.0f - r) * (1.0f - r) + 0.02f;   // Q 0.5 .. 50
    // Filter input gain (MG: drive, both: resonance bass loss), folded into the oscillator weights
    const FType ft = kFTypes[S.ftype];
    const float in = ft.flavour == kMG ? 1.2f - 0.6f * r : (ft.flavour == kSVF ? 1.0f - 0.4f * r : 1.0f);
    d.w1 = (1.0f - mix) * in;
    d.w2 = mix * in;
    // Amplifier
    const float vol = 1.0f + m[kDstVolume];
    d.amp = src[kSrcAmpEnv] * (vol > 0.0f ? vol * kOutTrim : 0.0f);
}

// ---------------------------------------------------------------------------

// One oscillator over the block: pass 1 + pass 2 into buf. A silent oscillator only advances its phase.
PHZ_PER_BLOCK void oscPass(float* buf, float* phi, float& phase, int model, int chr, float dt0, float dt1, float x0, float y0,
             float x1, float y1, float w0, float w1, float s0, float s1) {
    constexpr float n = 1.0f / float(kBlock / kChunk);   // chunk steps per block
    if (w0 == 0.0f && w1 == 0.0f) {
        phase += float(kBlock) * 0.5f * (dt0 + dt1);
        phase -= float(int(phase));
        return;
    }
    const Pd p0 = derivePd(model, x0, y0, dt0), p1 = derivePd(model, x1, y1, dt1);
    const Pd dp = {(p1.a - p0.a) * n, (p1.b - p0.b) * n, (p1.c - p0.c) * n, (p1.d - p0.d) * n, (p1.e - p0.e) * n};
    const Pd pm = {p0.a + 0.5f * dp.a, p0.b + 0.5f * dp.b, p0.c + 0.5f * dp.c, p0.d + 0.5f * dp.d, p0.e + 0.5f * dp.e};
    const float ddt = (dt1 - dt0) * n, dw = (w1 - w0) * n, ds = (s1 - s0) * n;
    kPdFns[model](phi, phase, dt0 + 0.5f * ddt, ddt, pm, dp);
    kReadFns[chr > 3 ? 3 : chr](buf, phi, w0 + 0.5f * dw, dw, s0 + 0.5f * ds, ds, chr >= 3 ? kAddN[chr - 3] : 0.0f);
}

class PhzEngine final : public ISynthEngine {
public:
    int32_t id() const override { return kEngineId; }
    const char* name() const override { return "PHZ"; }

    void setTempo(float bpm) override { phz::setTempo(bpm); }

    void slotReset(SynthSlotScratch& sc) override {
        std::memset(sc.bytes, 0, sizeof(sc.bytes));
        SlotState& S = sc.as<SlotState>();
        S.rng = 0x2545F491u;
    }

    PHZ_PER_BLOCK void prepare(SynthSlotScratch& sc, const SynthParams& p) override {
        if (p.count < kParamCount) return;
        SlotState& S = sc.as<SlotState>();
        const float* E = p.end;
        if (!S.valid || S.serial != p.serial) {
            for (int e = 0; e < kEnvs; ++e) {
                const int base = kAmpA + 4 * e;
                const float a = E[base];
                S.envInc[e] = a <= kBlockT ? 2.0f : kBlockT / a;
                S.envCd[e] = blockCoef(E[base + 1]);
                S.envCr[e] = blockCoef(E[base + 3]);
            }
            S.glideCoef = blockCoef(E[kGlideTime]);
            S.ftype = uint8_t(enumOf(E[kFilterType], 14));
            S.model = uint8_t(enumOf(E[kModel], kModelCount - 1));
            S.char1 = uint8_t(enumOf(E[kChar1], kCharCount - 1));
            S.char2 = uint8_t(enumOf(E[kChar2], kCharCount - 1));
            S.fltOff = E[kCutoff] >= kCutoffOff;
            for (int l = 0; l < 2; ++l) {
                const int base = l == 0 ? kLfo1Sync : kLfo2Sync;
                S.lfoWave[l] = uint8_t(enumOf(E[base + 2], 5));
                S.lfoRetrig[l] = uint8_t(enumOf(E[base + 3], 2));
            }
            S.glideMode = uint8_t(enumOf(E[kGlideMode], 3));
            S.reset1 = E[kReset1] >= 0.5f;
            S.reset2 = E[kReset2] >= 0.5f;
            S.serial = p.serial;
            S.valid = 1;
        }
        // LFO rates (Sync: tempo divisions, the tempo may change any block)
        for (int l = 0; l < 2; ++l) {
            const int base = l == 0 ? kLfo1Sync : kLfo2Sync;
            const float hz = E[base] >= 0.5f ? gTempo * (1.0f / 60.0f) / kSyncBeats[enumOf(E[base + 1], 15)]
                                             : clampf(E[base + 1], 0.0f, 100.0f);
            S.lfoInc[l] = hz * kBlockT;
            S.lfoPh0[l] = S.lfoPh1[l];
            S.lfoHold0[l] = S.lfoHold1[l];
            float ph = S.lfoPh1[l] + S.lfoInc[l];
            if (ph >= 1.0f) {
                ph -= float(int(ph));
                S.lfoHold1[l] = rnd(S.rng);
            }
            S.lfoPh1[l] = ph;
        }
        // Mod matrix (amounts ramp start -> end)
        int n = 0;
        for (int k = 0; k < kModSlots; ++k) {
            const int b = kModBase + 3 * k;
            const float a0 = p.start[b + 2], a1 = E[b + 2];
            if (a0 == 0.0f && a1 == 0.0f) continue;
            Mod& md = S.mod[n++];
            md.src = uint8_t(enumOf(E[b], kSrcCount - 1));
            md.dst = uint8_t(enumOf(E[b + 1], kDstCount - 1));
            md.a0 = a0;
            md.a1 = a1;
        }
        S.nmod = uint8_t(n);
    }

    PHZ_PER_BLOCK void noteOn(SynthVoice& v, const SynthParams& p, const SynthNote& n) override {
        SlotState& S = v.slotScratch->as<SlotState>();
        VoiceState& V = v.as<VoiceState>();
        const float* E = p.end;
        if (!V.started) {
            V.rng = (lcg(S.rng) ^ 0x9E3779B9u) | 1u;
            V.started = 1;
        }
        V.lastValid = 0;
        const int gm = enumOf(E[kGlideMode], 3);
        const bool legato = n.overlap && (gm == 2 || gm == 3);   // Legato modes: no retrigger
        const bool glide = !n.fxGlide && E[kGlideTime] > 0.0005f && ((gm == 0 || gm == 2) ? (n.reused || S.hasLast) : n.overlap);
        const float from = n.reused ? V.pitch : S.lastPitch;
        V.target = n.pitch;
        V.pitch = glide ? from : n.pitch;
        S.lastPitch = n.pitch;
        S.hasLast = 1;
        if (!legato) {
            for (int e = 0; e < kEnvs; ++e) V.stage[e] = kAttack;
            for (int l = 0; l < 2; ++l) {
                const int base = l == 0 ? kLfo1Sync : kLfo2Sync;
                if (enumOf(E[base + 3], 2) != kFree) {
                    V.lfoPh[l] = 0.0f;
                    V.lfoHold[l] = rnd(V.rng);
                }
            }
            // Reset On: the cycle restarts at phase 0 on note on. Off: free running, the phase continues from where
            // the voice left it (0 on a fresh voice).
            if (E[kReset1] >= 0.5f) V.ph1 = 0.0f;
            if (E[kReset2] >= 0.5f) V.ph2 = 0.0f;
        }
        v.level = V.env[0];
    }

    void noteOff(SynthVoice& v, const SynthParams& p) override {
        (void)p;
        VoiceState& V = v.as<VoiceState>();
        for (int e = 0; e < kEnvs; ++e)
            if (V.stage[e] != kIdle) V.stage[e] = kRelease;
    }

    PHZ_PER_BLOCK bool render(SynthVoice& v, const SynthParams& p, float g0, float gInc, float* L, float* R) override {
        VoiceState& V = v.as<VoiceState>();
        if (!V.started || V.stage[0] == kIdle || p.count < kParamCount) {
            v.level = 0.0f;
            return false;
        }
        const SlotState& S = v.slotScratch->as<SlotState>();
        const float* P1 = p.end;

        // Sources at the block start
        float src0[kSrcCount], src1[kSrcCount];
        src0[kSrcAmpEnv] = V.env[0];
        src0[kSrcFltEnv] = V.env[1];
        src0[kSrcAuxEnv] = V.env[2];
        for (int l = 0; l < 2; ++l) {
            const int w = S.lfoWave[l];
            src0[kSrcLfo1 + l] = S.lfoRetrig[l] == kFree ? lfoShape(w, S.lfoPh0[l], S.lfoHold0[l])
                                                         : lfoShape(w, V.lfoPh[l], V.lfoHold[l]);
        }
        const float pitch0 = V.pitch;

        // Step glide, envelopes and LFOs by one block
        if (V.pitch != V.target) {
            const float d = (V.pitch - V.target) * S.glideCoef;
            V.pitch = absf(d) < 0.001f ? V.target : V.target + d;
        }
        for (int e = 0; e < kEnvs; ++e) {
            float lv = V.env[e];
            const float sus = clampf(P1[kAmpS + 4 * e], 0.0f, 1.0f);
            switch (V.stage[e]) {
                case kAttack:
                    lv += S.envInc[e];
                    if (lv >= 1.0f) {
                        lv = 1.0f;
                        V.stage[e] = kDecay;
                    }
                    break;
                case kDecay:
                    lv = sus + (lv - sus) * S.envCd[e];
                    if (sus <= 0.0f && lv < 1.0e-4f) {
                        lv = 0.0f;
                        V.stage[e] = kIdle;
                    }
                    break;
                case kRelease:
                    lv *= S.envCr[e];
                    if (lv < 1.0e-4f) {
                        lv = 0.0f;
                        V.stage[e] = kIdle;
                    }
                    break;
                default: break;
            }
            V.env[e] = lv;
        }
        src1[kSrcAmpEnv] = V.env[0];
        src1[kSrcFltEnv] = V.env[1];
        src1[kSrcAuxEnv] = V.env[2];
        for (int l = 0; l < 2; ++l) {
            const int w = S.lfoWave[l];
            if (S.lfoRetrig[l] == kFree) {
                src1[kSrcLfo1 + l] = lfoShape(w, S.lfoPh1[l], S.lfoHold1[l]);
                continue;
            }
            float ph = V.lfoPh[l] + S.lfoInc[l];
            if (ph >= 1.0f) {
                if (S.lfoRetrig[l] == kOneShot) {
                    ph = kOneShotEnd;
                } else {
                    ph -= float(int(ph));
                    V.lfoHold[l] = rnd(V.rng);
                }
            }
            V.lfoPh[l] = ph;
            src1[kSrcLfo1 + l] = lfoShape(w, ph, V.lfoHold[l]);
        }

        // Destinations: the block start is the last block's end
        Dest d0, d1;
        if (V.lastValid) d0 = V.last;
        else evalDest(p.start, S, src0, false, pitch0 + v.bendStart, d0);
        evalDest(P1, S, src1, true, V.pitch + v.bend, d1);
        V.last = d1;
        V.lastValid = 1;

        // Both oscillators into buf
        float buf[kBlock], phi[kBlock];
        zeroWords(buf, sizeof(buf));
        oscPass(buf, phi, V.ph1, S.model, S.char1, d0.dt1, d1.dt1, d0.x1, d0.y1, d1.x1, d1.y1, d0.w1, d1.w1, d0.sh1, d1.sh1);
        oscPass(buf, phi, V.ph2, S.model, S.char2, d0.dt2, d1.dt2, d0.x2, d0.y2, d1.x2, d1.y2, d0.w2, d1.w2, d0.sh2, d1.sh2);

        // Filter and output
        constexpr float n = 1.0f / float(kBlock);
        const FType ft = kFTypes[S.ftype];
        const float gEnd = g0 + gInc * float(kBlock);
        const float ga = d0.amp * g0, gb = d1.amp * gEnd;
        const float dg = (gb - ga) * n;
        if (S.fltOff) {
            outKernel(buf, L, R, ga, dg);
        } else {
            if (ft.poles24) {
                const float k1 = ft.flavour == kMG ? kStage1KMG : kStage1K;
                kSvfFirst[ft.mode](buf, V.f, svfRamp(d0.g, k1, d1.g, k1), nullptr, nullptr, 0.0f, 0.0f);
            }
            kSvfLast[ft.flavour == kOB ? 1 : 0][ft.mode](buf, V.f + 2, svfRamp(d0.g, d0.k, d1.g, d1.k), L, R, ga, dg);
        }

        v.level = V.env[0];
        if (V.stage[0] == kIdle) {
            V.env[0] = 0.0f;
            return false;
        }
        return true;
    }
};

PhzEngine gPhz;

}  // namespace

ISynthEngine* engine() { return &gPhz; }

void setTempo(float bpm) {
    if (bpm > 0.0f) gTempo = bpm;
}

#ifdef PHZ_BENCH
void benchPd(int model, float* out, float& phase, float dt, float x, float y) {
    const Pd p = derivePd(model, x, y, dt);
    kPdFns[model](out, phase, dt, 0.0f, p, Pd{});
}
void benchRead(int character, float* out, const float* phi, float w, float shape) {
    kReadFns[character > 3 ? 3 : character](out, phi, w, 0.0f, shape, 0.0f, character >= 3 ? kAddN[character - 3] : 0.0f);
}
#endif

}  // namespace phz
}  // namespace audio
}  // namespace ot
