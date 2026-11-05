#pragma once
#include "config.h"
#ifdef FEATURE_SYNTH
#include "AudioStream.h"
#include "Platform.h"
#include "PolySynth/Patch.h"
#include "PolySynth/config.h"
#include "PolyUtil/misc.h"

#include "mtStructs.h"
#ifdef FEATURE_SYNTH_OPEN
#include "open_load.h"
#endif
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
    // Patch slots (SYNTH_OPEN): a part has kPatchSlots patches in the project, slot 0 is <P>.patch (stock opens it), slot n is
    // <P>-n.patch. The live patch is the slot that is open in the editor, the Patch step FX copies another slot's values over it.
    // Everything here is main thread only except applyPatchBankSlot (audio callback, copies floats, no shared_ptr).
    static constexpr int kPatchSlots = polysynth::open::kPatchBankSlots;
    int getPatchSlot(int part);
    // Saves the open slot when it is dirty, then opens `slot` (a slot without a file starts as a copy of the open one)
    polysynth::open::SlotSelect selectPatchSlot(int part, int slot);
    // Cancel of the library browser: back to what the open slot was before the browsing
    void revertCurrentPatch(int part, const std::string& previousName);
    // If the Patch FX left another slot's values in the part, put the open slot's values back (call before showing or editing)
    void restorePatchFx(int part) { polysynth::open::patchBankRestore(part); }
    // Writes the open slot to its Workspace file when it was edited (before the engine of the part changes)
    void flushPatchSlot(int part);
    void reloadPatchBank(int part);
    bool applyPatchBankSlot(int part, int slot);
    // Names of slots 1..15 live in the side file <P>.names next to the slot files (see synth-open/open_slotnames.h). Written whenever
    // a name changes and on project save, read on project load.
    void persistSlotNames(int part);
    void patchNameChanged(int part) // the open slot got another name (library load, save as, Init)
    {
        if (getPatchSlot(part) > 0)
        {
            persistSlotNames(part);
        }
    }
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
#ifdef FEATURE_SYNTH_OPEN
        std::array<std::string, polysynth::open::kPatchBankSlots> slotNames = {}; // name of each slot while it is not the open one
#endif
    };
    std::array<Part, kSynthParts> parts;
    std::shared_ptr<polysynth::Synth> synth;
    std::unique_ptr<polysynth::PatchPool> patchPool;
    std::shared_ptr<polysynth::PatchStore> patchStore;
    std::shared_ptr<polysynth::PatchStore> patchStoreWorkspace;
    std::unique_ptr<MidiAdapter> midiAdapter;
    FileSystem* filesystem;

    void setPatch(int part, std::shared_ptr<polysynth::Patch> patch, std::string name);
#ifdef FEATURE_SYNTH_OPEN
    bool loadWorkspacePatch(int part, std::optional<std::string> overrideName);
    void loadSlotNames(int part);
#endif

    friend class PolySynth;
};
#endif
