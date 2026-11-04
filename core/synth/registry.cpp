#include "config.h"
#ifdef FEATURE_SYNTH

#include "PolySynth/sdk/InstrumentTypeRegistry.h"
#ifndef FEATURE_SYNTH_OPEN
#include "engines/ChiptuneSynthVoice.h"
#endif

namespace polysynth
{
// Factory synth engines (provided by precompiled libPolySynth.a + libPolyDSP.a)
extern std::unique_ptr<InstrumentType> createMidiOut();
#ifdef FEATURE_SYNTH_OPEN
// SYNTH_OPEN (MK66): the engines picked at build time (SYNTH_ENGINES, synth-open/engine_list.h)
#define SYNTH_ENGINE(NAME, STORED, TYPE_ID, FACTORY, ...) extern std::unique_ptr<InstrumentType> FACTORY();
#include "engine_list.h"
#undef SYNTH_ENGINE
#else
extern std::unique_ptr<InstrumentType> createSimpleAnalogSynth();
extern std::unique_ptr<InstrumentType> createFatAnalogSynth();
extern std::unique_ptr<InstrumentType> createVASynth();
extern std::unique_ptr<InstrumentType> createFMSynth();
extern std::unique_ptr<InstrumentType> createDrumSynth();
#endif
extern std::unique_ptr<InstrumentType> createShapingSynth();

void InstrumentTypeRegistry::registerInstrumentTypes(
  InstrumentTypeRegistry& registry,
  [[maybe_unused]] const Synth::Allocator& largeBuffersAlloc)
{
    registry.addInstrumentType(static_cast<int>(InstrumentTypeId::kMidiOut), createMidiOut());
#ifdef FEATURE_SYNTH_OPEN
#define SYNTH_ENGINE(NAME, STORED, TYPE_ID, FACTORY, ...) \
    registry.addInstrumentType(static_cast<int>(TYPE_ID), FACTORY());
#include "engine_list.h"
#undef SYNTH_ENGINE
#else
    registry.addInstrumentType(static_cast<int>(InstrumentTypeId::kSimpleAnalogSynth),
                               createSimpleAnalogSynth());
    registry.addInstrumentType(static_cast<int>(InstrumentTypeId::kFatAnalogSynth),
                               createFatAnalogSynth());
    registry.addInstrumentType(static_cast<int>(InstrumentTypeId::kVASynth), createVASynth());
    registry.addInstrumentType(static_cast<int>(InstrumentTypeId::kFMSynth), createFMSynth());
    registry.addInstrumentType(static_cast<int>(InstrumentTypeId::kDrumSynth), createDrumSynth());
    registry.addInstrumentType(static_cast<int>(InstrumentTypeId::kShapingSynth),
                               createShapingSynth());

    // Open-Source Custom Synth Engines
    registry.addInstrumentType(static_cast<int>(InstrumentTypeId::kChiptuneSynth),
                               createChiptuneSynth());
#endif
}

} // namespace polysynth

#endif
