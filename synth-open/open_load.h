// Open PolySynth shim for MK66 - synth voice budget and governor interface.
// Plain C++ with no PolySynth or audio library includes, so core/source/Profiler.cpp and the
// exporter can call it. Implemented in open_synth.cpp (OpenSynth owns the pool and the governor).
#pragma once

#include <cstdint>

namespace polysynth
{
class Patch;
class PatchStore;
} // namespace polysynth

namespace polysynth::open
{

// Audio interrupt, once per live block, after the whole block rendered: `cycles` is the CPU
// cycle count of the block (DWT CYCCNT delta, 180 MHz on MK66). Feeds the load meter and the
// governor (ot::audio::Governor). No-op without a Synth.
void reportBlockCycles(uint32_t cycles);

// Offline render (export): the governor is off and the cap is back at the budget.
void setOffline(bool offline);

// Project load: every synth voice is cut and the governor and load meter start fresh (cap back at
// the budget, no hold-off). Call with audio interrupts disabled. No-op without a Synth.
void resetOnProjectLoad();

// Audio interrupt, once per part and block (PolySynth::update): true when the part would render nothing this block - no
// voice sounding and no slot effect tail (the same test as the early return of the instrument's render). The caller then
// transmits nothing (a missing block is silence for the amplifiers, analysers and mixers downstream) instead of
// rendering, soft clipping and sending blocks of zeros. False for a part that does not exist.
bool partIdle(int part);

// Patch change FX. Per part, a bank of the first 16 patches of the part's engine (Patches/<ENGINE>/, alphabetical, as the
// patch browser lists them), held as plain values (no macros, no OpenPatch). Slots of another engine, a missing slot or a file
// that does not decode are ignored.
constexpr int kPatchBankSlots = 16;
constexpr int kPatchBankParts = 3;
// Main thread (after a patch is loaded, the engine changes or a patch is saved): reads the files and replaces the bank of `part`.
void patchBankPreload(int part, Patch& patch, PatchStore& store);
// Audio callback (Sequencer::tick, before the synth renders): copies the values of `slot` into `patch`. False (nothing
// changed) when the slot is out of range, empty or from another engine. No allocation, no file access, patchDirty untouched.
bool patchBankApply(int part, int slot, Patch* patch);

// Global synth voice pool shared by all slots: default 4 (OpenTracker 2.2.4), max 8.
constexpr int kDefaultVoiceBudget = 4;
constexpr int kMaxVoiceBudget = 8;
void setVoiceBudget(int voices);
int voiceBudget();
int voiceCap();       // budget lowered by the governor
int voicesSounding(); // synth voices counted by the pool (stolen ones excluded)
uint32_t governorCuts();

} // namespace polysynth::open
