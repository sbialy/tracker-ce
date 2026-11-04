#pragma once
#include "config.h"
#ifdef FEATURE_SYNTH
#include "AudioStream.h"
#include "Platform.h"
#include "PolySynth/Patch.h"
#include "PolySynth/config.h"
#include "PolyUtil/misc.h"

#include "mtStructs.h"
#include <array>
#include <cassert>
#include <memory>
#include <span>

namespace polysynth
{
class Synth;
class PatchStore;
class Instrument;
class Mixer;
class PatchPool;
};

class MidiAdapter;

class Synth
{
  public:
    static constexpr float kGainSilence = -96;
    static constexpr float kGainMax = 12;
    static constexpr float kGainDefault = -9;
    static constexpr int kPanningMax = 100;
    static constexpr int kPanningDefault = 0;
    static constexpr int kSendMaxValue = 100;
    static constexpr float kSendDefaultValue = 0.0f;
#ifdef FEATURE_SYNTH_OPEN
    // SYNTH_OPEN (MK66): only the engines picked at build time (SYNTH_ENGINES, synth-open/engine_list.h, PERC only when picked)
    // and no Shaping (no open engine). The model list on screen is this array.
    static constexpr std::array kSynthTypes = {
#define SYNTH_ENGINE(NAME, STORED, TYPE_ID, ...) TYPE_ID,
#include "engine_list.h"
#undef SYNTH_ENGINE
    };
    // Projects store an engine as its canonical number, not as its position in kSynthTypes, so a project keeps
    // its engines when it is opened by a build with another engine set (engine_list.h).
    static constexpr std::array kSynthStoredIdx = {
#define SYNTH_ENGINE(NAME, STORED, ...) STORED,
#include "engine_list.h"
#undef SYNTH_ENGINE
    };
    // Position in kSynthTypes of a stored engine number, -1 when this build does not have the engine
    static constexpr int storedToUi(int stored)
    {
        for (size_t i = 0; i < kSynthStoredIdx.size(); i++)
        {
            if (kSynthStoredIdx[i] == stored)
            {
                return static_cast<int>(i);
            }
        }
        return -1;
    }
    static constexpr int uiToStored(int ui) { return kSynthStoredIdx.at(ui); }
#else
    static constexpr std::array kSynthTypes = { polysynth::InstrumentTypeId::kSimpleAnalogSynth,
                                                polysynth::InstrumentTypeId::kFatAnalogSynth,
                                                polysynth::InstrumentTypeId::kVASynth,
                                                polysynth::InstrumentTypeId::kFMSynth,
                                                polysynth::InstrumentTypeId::kDrumSynth,
                                                polysynth::InstrumentTypeId::kShapingSynth,
                                                polysynth::InstrumentTypeId::kChiptuneSynth
                                              };
    static constexpr int storedToUi(int stored)
    {
        return (stored >= 0 && stored < static_cast<int>(kSynthTypes.size())) ? stored : -1;
    }
    static constexpr int uiToStored(int ui) { return ui; }
#endif
    static constexpr bool isStoredTypeValid(int stored) { return storedToUi(stored) >= 0; }
    static constexpr bool storedTypeIs(int stored, polysynth::InstrumentTypeId id)
    {
        const int ui = storedToUi(stored);
        return ui >= 0 && kSynthTypes[ui] == id;
    }
    static constexpr auto kInitPatchName = "Init";

    Synth();
    ~Synth();

    void init(Platform* platform, std::function<tiny::optional<float>()> getTempo);

    ::Midi* getVirtualMidiPort();

    std::vector<std::string_view> getPatchList(int part);
    [[nodiscard]] bool loadPatch(int part, const char* name);
    bool savePatch(int part, const char* name);
    bool savePatch(int part);
    void initPatch(int part);
    bool loadPatchFromWorkspace(int part, std::optional<std::string> overrideName = std::nullopt);
    void savePatchToWorkspace(int part);
    void saveDirtyWorkspacePatches();
    void copyWorkspacePatches(const char* sourceProjectPath, const char* destinationProjectPath);
    void setPatchDirty(int part) { parts[part].patchDirty = true; };
    bool isAnyPatchDirty();

    void setGain(int part, float gainDb);
    void setPanning(int part, float panning);
    void setVoiceCount(int part, int voiceCount);
    int getMaxVoiceCount(int part);
    void setInstrumentType(int part, polysynth::InstrumentTypeId typeId);
#ifdef FEATURE_SYNTH_OPEN
    // Patch change FX: the bank holds the values of the first patches of the part's engine (Patches/<ENGINE>/).
    // reloadPatchBank is main thread only, applyPatchBankSlot runs in the audio callback and only copies floats.
    void reloadPatchBank(int part);
    bool applyPatchBankSlot(int part, int slot);
#endif

    std::string getInstrumentTypeName(int typeIdx);
    int getInstrumentTypeIdx(int part);
    std::string getPatchName(int part);
    std::string getSynthName(int part);
    std::string getParamFullName(int part, int paramId);
    const char* getMacroKnobName(int macroId);
    const char* getDefault(int macroId);

    int getSend(int part, int sendIdx); // in percent
    float getPanning(int part);         // +- percent

    // use AudioCS when modyfing the patch object
    polysynth::Patch& getPatch(int part);

    void triggerTestNote(int part, int note, bool noteOn);

    void updateToSettings(const strMtValues& settings, bool resetAllVoices = false);

    void initSynthPartSettings(int part, SynthPartSettings& settings);
    void initSynthPartMacrosValues(int part, SynthPartSettings& settings);
    void initSynthPartMacroValue(int part, int macroIdx, SynthPartSettings& settings);
    void validateSynthPartSettings(int part, SynthPartSettings& settings);

  private:
    struct Part
    {
        polysynth::InstrumentTypeId instrumentTypeId = {};
        std::shared_ptr<polysynth::Instrument> instrument = nullptr;
        std::shared_ptr<polysynth::Patch> patch = nullptr;
        std::string patchName = {};
        bool patchDirty = false;
    };
    std::array<Part, kSynthParts> parts;
    std::shared_ptr<polysynth::Synth> synth;
    std::unique_ptr<polysynth::PatchPool> patchPool;
    std::shared_ptr<polysynth::PatchStore> patchStore;
    std::shared_ptr<polysynth::PatchStore> patchStoreWorkspace;
    std::unique_ptr<MidiAdapter> midiAdapter;
    FileSystem* filesystem;

    void setPatch(int part, std::shared_ptr<polysynth::Patch> patch, std::string name);

    friend class PolySynth;
};
#endif
