// PHZ: clean-room phase distortion engine for the open PolySynth shim (MK66, Cortex-M4F).
//
// Written from the public Polyend Synth manual only (SYN2 p.76-80, as summarised in ref/engine-specs.md section 5).
// Nothing of Polyend's closed libPolySynth.a / libPolyDSP.a was read. The DSP is our own: two phase distortion
// oscillators (a distorted phase ramp reads a cosine based cycle), a Mix, 15 filter types, three ADSRs, two LFOs and a
// 6 slot mod matrix. The voice skeleton (block rate envelopes / LFOs / glide / mod matrix, the TPT filter stages) follows
// OpenTracker's VAP engine in this directory tree (synth-open/ot/audio/engines/vap.cpp, Apache-2.0, see NOTICE.md).
// No Mutable Instruments Plaits code is used here, so no MIT notice of theirs is carried (see phz.cpp, "Provenance").
//
// Engine contract: audio/synth_engine.h. Volume (index 18) is never applied here, the output is mono (L = R).
#pragma once

#include "audio/synth_engine.h"

namespace ot {
namespace audio {
namespace phz {

constexpr int32_t kEngineId = 0x82;   // no Polyend id: ours, after OpenTracker's JU6 (0x81)

// Parameter indices, in the page order of the Polyend Synth manual (SYN2 p.76-79).
enum Param : int {
    // Oscillator page 1
    kShape1 = 0, kShape2, kMix, kX1, kX2, kY1, kY2, kModel,
    // Oscillator page 2
    kReset1 = 8, kReset2, kDetune, kTune1, kTune2, kFinetune, kChar1, kChar2,
    // Oscillator page 3
    kGlideMode = 16, kGlideTime, kVolume,
    // Filter page 1
    kCutoff = 19, kResonance, kFilterType, kFilterEnvAmt, kNoteTrack,
    // Envelopes: Amp (page 1), Filter (page 2), Aux (page 3)
    kAmpA = 24, kAmpD, kAmpS, kAmpR,
    kFltA = 28, kFltD, kFltS, kFltR,
    kAuxA = 32, kAuxD, kAuxS, kAuxR,
    // LFO 1 (page 1), LFO 2 (page 2)
    kLfo1Sync = 36, kLfo1Ratio, kLfo1Wave, kLfo1Retrig,
    kLfo2Sync = 40, kLfo2Ratio, kLfo2Wave, kLfo2Retrig,
    kModBase = 44,   // slot k: Src 44+3k, Dest 45+3k, Amt 46+3k
    kParamCount = 62,
};

enum Model : int { kOgSaw = 0, kOgSquare, kHermiteSmooth, kSquared, kLinear, kHermiteFm, kModelCount };
enum Character : int { kSmoother = 0, kSharper, kWilder, kAdd1, kAdd2, kAdd3, kAdd5, kAdd7, kAdd11, kCharCount };

enum ModSrc : int { kSrcAmpEnv = 0, kSrcFltEnv, kSrcAuxEnv, kSrcLfo1, kSrcLfo2, kSrcCount };
enum ModDest : int {
    kDstX1 = 0, kDstX2, kDstY1, kDstY2, kDstShape1, kDstShape2, kDstMix, kDstCutoff, kDstResonance, kDstVolume,
    kDstPitch1, kDstPitch2, kDstCount
};

ISynthEngine* engine();

// Tempo for LFO Sync (Ratio = tempo division index 0..15 when Sync is On). The framework sets it every block through
// ISynthEngine::setTempo; 120 BPM until the first block.
void setTempo(float bpm);

#ifdef PHZ_BENCH
// Host and cycle benches only: the two stages of one oscillator, to be measured on their own.
void benchPd(int model, float* out, float& phase, float dt, float x, float y);
void benchRead(int character, float* out, const float* phi, float w, float shape);
#endif

}  // namespace phz
}  // namespace audio
}  // namespace ot
