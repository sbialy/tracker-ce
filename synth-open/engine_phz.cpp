// PHZ parameter metadata for the open PolySynth shim on MK66. PHZ is our clean-room phase distortion engine
// (synth-open/phz/phz.cpp). The parameter list, the page order, the value ranges and the enum names follow the public
// Polyend Synth manual (SYN2 p.76-80, summarised in ref/engine-specs.md section 5). The manual states no defaults and no
// mod destinations: every default, the 12 mod destinations, the edit curves, the section split and the default macros
// are this shim's own choice.
#include "open_engine.h"

#include "PolySynth/config.h"
#include "phz/phz.h"

namespace polysynth::open
{
namespace
{
using ot::audio::phz::Param;

const char* const kModel[] = { "OG Saw", "OG Square", "Hermite Smooth", "Squared", "Linear", "Hermite FM" };
const char* const kChar[] = { "Smoother", "Sharper", "Wilder", "Add 1", "Add 2", "Add 3", "Add 5", "Add 7", "Add 11" };
const char* const kGlide[] = { "Always", "Overlap", "Legato", "Legato Overlap" };
const char* const kFilter[] = { "LP MG 24dB",   "LP OB 24dB",   "LP OB 12dB",   "LP SVF 24dB",   "LP SVF 12dB",
                                "HP OB 24dB",   "HP OB 12dB",   "HP SVF 24dB",  "HP SVF 12dB",   "BP OB 24dB",
                                "BP OB 12dB",   "BP SVF 24dB",  "BP SVF 12dB",  "Notch SVF 24dB", "Notch SVF 12dB" };
const char* const kWave[] = { "Triangle", "Sine", "Ramp Up", "Ramp Down", "Square", "Random S&H" };
const char* const kRetrig[] = { "Free Running", "One Shot", "Note" };
const char* const kModSrc[] = { "Amp Env", "Filter Env", "Aux Env", "LFO 1", "LFO 2" };
const char* const kModDest[] = { "Osc1 XMod", "Osc2 XMod", "Osc1 YMod", "Osc2 YMod", "Osc1 Shape", "Osc2 Shape",
                                 "Osc Mix",   "Cutoff",    "Resonance", "Volume",    "Osc1 Pitch", "Osc2 Pitch" };

using F = Fmt;
using C = CurveKind;

#define MODROW(k) \
    { "Src " #k, 0, 0, 4, F::kEnum, C::kLinear, kModSrc }, { "Dest " #k, 0, 0, 11, F::kEnum, C::kLinear, kModDest }, \
      { "Amt " #k, 0, -4, 4, F::kPctMax, C::kLinear, nullptr }

#define ENVROW(a, d, s, r) \
    { "Attack", a, 0, 10, F::kSec, C::kSteep, nullptr }, { "Decay", d, 0, 10, F::kSec, C::kSteep, nullptr }, \
      { "Sustain", s, 0, 1, F::kPct, C::kLinear, nullptr }, { "Release", r, 0, 10, F::kSec, C::kSteep, nullptr }

#define LFOROW() \
    { "Sync", 0, 0, 1, F::kOnOff, C::kLinear, nullptr }, { "Ratio", 4, 0, 100, F::kLfoHz, C::kExp, nullptr }, \
      { "Waveform", 1, 0, 5, F::kEnum, C::kLinear, kWave }, { "Retrigger", 2, 0, 2, F::kEnum, C::kLinear, kRetrig }

const ParamDef kParams[Param::kParamCount] = {
    // Oscillator page 1
    { "Shape 1", 0, 0, 1, F::kVal100, C::kLinear, nullptr },
    { "Shape 2", 0, 0, 1, F::kVal100, C::kLinear, nullptr },
    { "Mix", 0.5f, 0, 1, F::kPct, C::kLinear, nullptr },
    { "Osc1 XMod", 0.6f, 0, 1, F::kUnit, C::kLinear, nullptr },
    { "Osc2 XMod", 0.6f, 0, 1, F::kUnit, C::kLinear, nullptr },
    { "Osc1 YMod", 0.5f, 0, 1, F::kUnit, C::kLinear, nullptr },
    { "Osc2 YMod", 0.5f, 0, 1, F::kUnit, C::kLinear, nullptr },
    { "Model", 0, 0, 5, F::kEnum, C::kLinear, kModel },
    // Oscillator page 2
    { "Reset 1", 1, 0, 1, F::kOnOff, C::kLinear, nullptr },
    { "Reset 2", 1, 0, 1, F::kOnOff, C::kLinear, nullptr },
    { "Detune", 8, 0, 100, F::kCents, C::kLinear, nullptr },
    { "Tune 1", 0, -36, 36, F::kSemi, C::kLinear, nullptr },
    { "Tune 2", 0, -36, 36, F::kSemi, C::kLinear, nullptr },
    { "Finetune", 0, -100, 100, F::kCents, C::kLinear, nullptr },
    { "Character 1", 0, 0, 8, F::kEnum, C::kLinear, kChar },
    { "Character 2", 0, 0, 8, F::kEnum, C::kLinear, kChar },
    // Oscillator page 3
    { "Glide Mode", 0, 0, 3, F::kEnum, C::kLinear, kGlide },
    { "Glide Time", 0, 0, 3, F::kSec, C::kPower, nullptr },
    { "Volume", 1, 0, 2, F::kPct, C::kLinear, nullptr },
    // Filter page 1
    { "Cutoff", 6000, 20, 20000, F::kHz, C::kExp, nullptr },
    { "Resonance", 0.2f, 0, 1, F::kPct, C::kLinear, nullptr },
    { "Type", 3, 0, 14, F::kEnum, C::kLinear, kFilter },
    { "Env Amount", 0.25f, -1, 1, F::kPct, C::kLinear, nullptr },
    { "Note Track", 1, -2, 2, F::kPct, C::kLinear, nullptr },
    // Envelopes: Amp, Filter, Aux
    ENVROW(0, 0.4f, 0.8f, 0.25f),
    ENVROW(0, 0.5f, 0.2f, 0.3f),
    ENVROW(0, 0.5f, 0, 0.3f),
    // LFO 1, LFO 2
    LFOROW(),
    LFOROW(),
    MODROW(1), MODROW(2), MODROW(3), MODROW(4), MODROW(5), MODROW(6),
};
#undef MODROW
#undef ENVROW
#undef LFOROW

// SYN2 pages: Oscillator 1 to 3, Filter, Envelope Amp / Filter / Aux, LFO 1 / 2, then the mod matrix
const uint8_t kOsc1[] = { Param::kShape1, Param::kShape2, Param::kMix, Param::kX1, Param::kX2, Param::kY1, Param::kY2, Param::kModel };
const uint8_t kOsc2[] = { Param::kReset1, Param::kReset2, Param::kDetune, Param::kTune1, Param::kTune2, Param::kFinetune,
                          Param::kChar1, Param::kChar2 };
const uint8_t kVoice[] = { Param::kGlideMode, Param::kGlideTime, Param::kVolume };
const uint8_t kFlt[] = { Param::kCutoff, Param::kResonance, Param::kFilterType, Param::kFilterEnvAmt, Param::kNoteTrack };
const uint8_t kAmp[] = { Param::kAmpA, Param::kAmpD, Param::kAmpS, Param::kAmpR };
const uint8_t kEnvF[] = { Param::kFltA, Param::kFltD, Param::kFltS, Param::kFltR };
const uint8_t kEnvA[] = { Param::kAuxA, Param::kAuxD, Param::kAuxS, Param::kAuxR };
const uint8_t kLfo1[] = { Param::kLfo1Sync, Param::kLfo1Ratio, Param::kLfo1Wave, Param::kLfo1Retrig };
const uint8_t kLfo2[] = { Param::kLfo2Sync, Param::kLfo2Ratio, Param::kLfo2Wave, Param::kLfo2Retrig };
const uint8_t kMod[] = { 44, 45, 46, 47, 48, 49, 50, 51, 52, 53, 54, 55, 56, 57, 58, 59, 60, 61 };

// The editor picks the section icon from the name ("Oscillator", "Amp Envelope", "Envelope", "Filter", "LFO", ...)
const SectionDef kSections[] = {
    { "Oscillator 1", "Osc 1", kOsc1, sizeof(kOsc1) },
    { "Oscillator 2", "Osc 2", kOsc2, sizeof(kOsc2) },
    { "Voice", "Voice", kVoice, sizeof(kVoice) },
    { "Filter", "Filter", kFlt, sizeof(kFlt) },
    { "Amp Envelope", "Amp Env", kAmp, sizeof(kAmp) },
    { "Filter Envelope", "Flt Env", kEnvF, sizeof(kEnvF) },
    { "Aux Envelope", "Aux Env", kEnvA, sizeof(kEnvA) },
    { "LFO 1", "LFO 1", kLfo1, sizeof(kLfo1) },
    { "LFO 2", "LFO 2", kLfo2, sizeof(kLfo2) },
    { "Modulation", "Mod", kMod, sizeof(kMod) },
};

// Default macros (this shim's own choice)
const MacroDef kMacros[] = {
    { "Cutoff", false, 1, { { Param::kCutoff, 0.4f } } },
    { "Resonance", false, 1, { { Param::kResonance, 1.0f } } },
    { "Timbre", true, 2, { { Param::kX1, 0.5f }, { Param::kX2, 0.5f } } },
    { "Bend", true, 2, { { Param::kY1, 0.5f }, { Param::kY2, 0.5f } } },
    { "Mix", true, 1, { { Param::kMix, 0.5f } } },
    { "Detune", false, 1, { { Param::kDetune, 0.2f } } },
};

} // namespace

const EngineDef kPhzDef = {
    "PHZ",
    0x82, // no Polyend id: ours, see engine_list.h
    kParams,
    Param::kParamCount,
    kSections,
    static_cast<int>(sizeof(kSections) / sizeof(kSections[0])),
    kMacros,
    static_cast<int>(sizeof(kMacros) / sizeof(kMacros[0])),
    Param::kVolume,
    kMaxVoicesPerInstrument,
    &ot::audio::phz::engine,
    false,
};

} // namespace polysynth::open
