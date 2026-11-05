// Cold half of the open PolySynth shim, built at -Os (SYNTH_OPT_COLD): value text, edit curves,
// the registry factories and metadata, OpenPatch's GUI API and the patch store. Nothing here runs
// per audio block (OpenPatch::effective and the instrument are in open_synth.cpp at -O2).
//
// Written only from the open headers in synth/include/PolySynth and OpenTracker's Apache-2.0
// engine interface (synth-open/ot). Nothing here is derived from libPolySynth/libPolyDSP.
#include "open_patch.h"
#include "open_load.h"

#include "PolySynth/PatchStore.h"
#include "PolySynth/sdk/InstrumentType.h"
#include "PolySynth/sdk/InstrumentTypeRegistry.h"
#include "PolySynth/sdk/InstrumentVoice.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <new>
#include <vector>

// The patch is applied while the render path cannot read it (macros are vectors). The host test
// build has no audio interrupt.
#ifdef OPEN_HOST_TEST
#define OPEN_LOCK_AUDIO()   ((void)0)
#define OPEN_UNLOCK_AUDIO() ((void)0)
#define OPEN_EXTERNAL_RAM
#else
#define OPEN_EXTERNAL_RAM __attribute__((section(".noinit.$EXTERNAL_RAM"))) // the SDRAM, as __NOINIT(EXTERNAL_RAM)
#include "interruptsControl.h"
#define OPEN_LOCK_AUDIO()   DISABLE_AUDIO_INTERRUPTS()
#define OPEN_UNLOCK_AUDIO() ENABLE_AUDIO_INTERRUPTS()
#endif

namespace polysynth
{
using open::EngineDef;
using open::ParamDef;
using open::kVelocityMacro;

namespace
{

std::string formatValue(const ParamDef& d, float v)
{
    char buf[24];
    switch (d.fmt)
    {
        case open::Fmt::kPct: snprintf(buf, sizeof(buf), "%d%%", int(std::lround(v * 100.0f))); break;
        case open::Fmt::kPctMax:
        {
            const float m = std::max(std::fabs(d.min), std::fabs(d.max));
            snprintf(buf, sizeof(buf), "%d%%", int(std::lround(m > 0 ? v / m * 100.0f : 0.0f)));
            break;
        }
        case open::Fmt::kHz:
            if (v >= 1000.0f)
            {
                snprintf(buf, sizeof(buf), "%.2f kHz", v / 1000.0f);
            }
            else
            {
                snprintf(buf, sizeof(buf), "%d Hz", int(std::lround(v)));
            }
            break;
        case open::Fmt::kSec:
            if (v < 1.0f)
            {
                snprintf(buf, sizeof(buf), "%d ms", int(std::lround(v * 1000.0f)));
            }
            else
            {
                snprintf(buf, sizeof(buf), "%.2f s", v);
            }
            break;
        case open::Fmt::kLfoHz: snprintf(buf, sizeof(buf), "%.2f Hz", v); break;
        case open::Fmt::kEnum:
        {
            const int idx = std::clamp(int(std::lround(v)), 0, int(d.max));
            return d.enumNames[idx];
        }
        case open::Fmt::kVal100: snprintf(buf, sizeof(buf), "%d", int(std::lround(v * 100.0f))); break;
        case open::Fmt::kOnOff: return v >= 0.5f ? "On" : "Off";
        case open::Fmt::kSemi: snprintf(buf, sizeof(buf), "%d st", int(std::lround(v))); break;
        case open::Fmt::kCents: snprintf(buf, sizeof(buf), "%d c", int(std::lround(v))); break;
        case open::Fmt::kInt: snprintf(buf, sizeof(buf), "%d", int(std::lround(v))); break;
        case open::Fmt::kRatio:
            snprintf(buf, sizeof(buf), "%.2f", 0.25f * float(std::clamp(int(std::lround(v)), 0, 47) + 1));
            break;
        case open::Fmt::kMs10: snprintf(buf, sizeof(buf), "%.2f ms", v * 0.1f); break;
        case open::Fmt::kDb: snprintf(buf, sizeof(buf), "%.1f dB", v); break;
#ifdef SYNTH_ENGINE_PERC
        case open::Fmt::kNote:
        {
            static const char* const kName[12] = { "C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B" };
            const int n = std::clamp(int(std::lround(v)), 0, 127);
            snprintf(buf, sizeof(buf), "%s%d", kName[n % 12], n / 12 - 1);
            break;
        }
        case open::Fmt::kBodyWave:
        {
            const int i = std::clamp(int(std::lround(v)), 0, 175);
            snprintf(buf, sizeof(buf), "%s %d%%", d.enumNames[i / 11], (i % 11) * 10);
            break;
        }
#endif
#ifdef SYNTH_ENGINE_PHZ
        case open::Fmt::kUnit: snprintf(buf, sizeof(buf), "%.2f", v); break;
#endif
        case open::Fmt::kPan:
        {
            const int p = int(std::lround(v * 100.0f));
            if (p == 0)
            {
                return "0";
            }
            snprintf(buf, sizeof(buf), "%c%d", p < 0 ? 'L' : 'R', p < 0 ? -p : p);
            break;
        }
    }
    return buf;
}

} // namespace

// Knob position [0,1] <-> value
float open::curveToValue(const ParamDef& d, float n)
{
    n = std::clamp(n, 0.0f, 1.0f);
    switch (d.curve)
    {
        case open::CurveKind::kLinear: break;
        case open::CurveKind::kExp:
            if (d.min > 0.0f)
            {
                return d.min * std::pow(d.max / d.min, n);
            }
            n = n * n;
            break;
        case open::CurveKind::kSteep: n = n * n * n; break;
        case open::CurveKind::kPower: n = n * n; break;
    }
    return d.min + (d.max - d.min) * n;
}

float open::valueToCurve(const ParamDef& d, float v)
{
    v = std::clamp(v, d.min, d.max);
    if (d.curve == open::CurveKind::kExp && d.min > 0.0f)
    {
        return std::log(v / d.min) / std::log(d.max / d.min);
    }
    const float n = d.max > d.min ? (v - d.min) / (d.max - d.min) : 0.0f;
    switch (d.curve)
    {
        case open::CurveKind::kExp:
        case open::CurveKind::kPower: return std::sqrt(n);
        case open::CurveKind::kSteep: return std::cbrt(n);
        default: return n;
    }
}

namespace
{

// ----------------------------------------------------------------- registry

std::unique_ptr<InstrumentType> createFromDef(const EngineDef& def)
{
    auto type = std::make_unique<InstrumentType>();
    type->name = def.name;
    type->createMetadata = [&def](Patch&, Patch::Metadata& metadata)
    {
        using Meta = Patch::Metadata;
        metadata.instrumentTypeId = def.typeId;
        metadata.outputChannels = Meta::OutputChannels::kMono;
        metadata.maxVoices = def.maxVoices;
        metadata.params.resize(def.paramCount);
        for (int i = 0; i < def.paramCount; i++)
        {
            const ParamDef& d = def.params[i];
            auto& p = metadata.params[i];
            p.displayName = d.name;
            p.defaultValue = d.def;
            p.minValue = d.min;
            p.maxValue = d.max;
            p.isEnum = d.fmt == open::Fmt::kEnum;
            p.editCurve = static_cast<Meta::Param::Curve>(d.curve);
            p.valueFormatter = [&d](const Patch&, float v) { return formatValue(d, v); };
        }
        for (int s = 0; s < def.sectionCount; s++)
        {
            const auto& sd = def.sections[s];
            Meta::EditorLayout::EditorSection section;
            section.name = sd.name;
            section.shortName = sd.shortName;
            for (int e = 0; e < sd.count; e++)
            {
                section.elements.push_back(
                  { Meta::EditorLayout::EditorElement::Type::kParam, sd.ids[e] });
            }
            metadata.layoutFlat.sections.push_back(std::move(section));
        }
        metadata.initDefaultMacros = [&def](std::vector<Meta::Macro>& macros)
        {
            for (int m = 0; m < def.macroCount && m < int(kUserMacroCount); m++)
            {
                const auto& md = def.macros[m];
                macros[m].displayName = md.name;
                macros[m].bipolar = md.bipolar;
                macros[m].modifiers.clear();
                for (int t = 0; t < md.count; t++)
                {
                    macros[m].modifiers.push_back({ md.targets[t].param, md.targets[t].amount });
                }
            }
            auto& vel = macros[kVelocityMacro];
            vel.displayName = "Velocity";
            vel.system = true;
            vel.perVoice = true;
            if (def.volumeParam >= 0)
            {
                vel.modifiers = { { def.volumeParam, 1.0f } };
            }
            auto& pr = macros[kVelocityMacro + 1];
            pr.displayName = "Pressure";
            pr.system = true;
            pr.perVoice = true;
        };
    };
    type->createVoice = [](int) -> std::unique_ptr<InstrumentVoice> { return nullptr; };
    return type;
}

// Engines that live in the closed libs and have no open replacement here: name only.
std::unique_ptr<InstrumentType> createUnavailable(const char* name)
{
    auto type = std::make_unique<InstrumentType>();
    type->name = name;
    type->createMetadata = [](Patch&, Patch::Metadata& metadata)
    { metadata.outputChannels = Patch::Metadata::OutputChannels::kStereo; };
    type->createVoice = [](int) -> std::unique_ptr<InstrumentVoice> { return nullptr; };
    return type;
}

} // namespace

const EngineDef* open::engineDefForType(int typeId)
{
    static const EngineDef* const kDefs[] = {
#define SYNTH_ENGINE(NAME, STORED, TYPE_ID, FACTORY, DEF, ...) &DEF,
#include "engine_list.h"
#undef SYNTH_ENGINE
    };
    for (const EngineDef* def : kDefs)
    {
        if (def->typeId == typeId)
        {
            return def;
        }
    }
    return nullptr;
}

int open::firstEngineTypeId()
{
    static const int kFirst[] = {
#define SYNTH_ENGINE(NAME, STORED, TYPE_ID, ...) static_cast<int>(TYPE_ID),
#include "engine_list.h"
#undef SYNTH_ENGINE
    };
    return kFirst[0];
}

std::shared_ptr<const InstrumentType> InstrumentTypeRegistry::getInstrumentType(int typeId) const
{
    auto it = instrumentTypes.find(typeId);
    return it == instrumentTypes.end() ? nullptr : it->second;
}

void InstrumentTypeRegistry::addInstrumentType(int typeId,
                                               std::unique_ptr<InstrumentType> instrumentType)
{
    instrumentTypes[typeId] = std::move(instrumentType);
}

std::unique_ptr<InstrumentType> createMidiOut() { return createUnavailable("MIDI"); }
// One registry factory per engine picked at build time (engine_list.h)
#define SYNTH_ENGINE(NAME, STORED, TYPE_ID, FACTORY, DEF, ...) \
    std::unique_ptr<InstrumentType> FACTORY() { return createFromDef(open::DEF); }
#include "engine_list.h"
#undef SYNTH_ENGINE

namespace open
{

// ------------------------------------------------------------------- patch

OpenPatch::OpenPatch(int typeId, const InstrumentType* type)
  : def(open::engineDefForType(typeId))
{
    metadata.instrumentTypeId = typeId;
    if (type && type->createMetadata)
    {
        type->createMetadata(*this, metadata);
    }
    metadata.initDefaultMacros(metadata.macros);
    count = std::min<int>(metadata.params.size(), kMaxParams);
    for (int i = 0; i < count; i++)
    {
        values[i] = metadata.params[i].defaultValue.value_or(0.0f);
    }
    for (int m = 0; m < int(kMacroCount); m++)
    {
        macroCC[m] = metadata.macros[m].bipolar ? 63 : 0;
    }
}

Patch::optional_paramvalue_t OpenPatch::getParamValue(int id) const
{
    return inRange(id) ? optional_paramvalue_t{ values[id] } : optional_paramvalue_t{ std::nullopt };
}

tiny_optional<float> OpenPatch::getParamValueEditMapped(int id) const
{
    if (!inRange(id) || !def)
    {
        return std::nullopt;
    }
    return valueToCurve(def->params[id], values[id]);
}

void OpenPatch::setParamValueEditMapped(int id, tiny_optional<float> v, ParamChangeSource src)
{
    if (inRange(id) && def && v.has_value())
    {
        setParamValue(id, curveToValue(def->params[id], *v), src);
    }
}

Patch::optional_paramvalue_t OpenPatch::getParamFinalValue(int id) const
{
    if (!inRange(id))
    {
        return std::nullopt;
    }
    float out[kMaxParams];
    effective(out);
    return out[id];
}

void OpenPatch::setParamValue(int id, optional_paramvalue_t value, ParamChangeSource)
{
    if (!inRange(id))
    {
        return;
    }
    const auto& p = metadata.params[id];
    const float v = value.has_value() ? *value : p.defaultValue.value_or(0.0f);
    values[id] = std::clamp(v, float(p.minValue), float(p.maxValue));
}

Patch::optional_paramvalue_t OpenPatch::getMacroValue(int id) const
{
    return validMacro(id) ? optional_paramvalue_t{ macroCC[id] * kMaxMacroValue / 127.0f }
                          : optional_paramvalue_t{ std::nullopt };
}

void OpenPatch::setMacroValue(int id, optional_paramvalue_t value, tiny_optional<int>)
{
    if (validMacro(id) && value.has_value())
    {
        macroCC[id] = uint8_t(std::clamp(std::lround(*value * 127.0f / kMaxMacroValue), 0L, 127L));
    }
}

void OpenPatch::setMacroModifier(int macroId, int idx, tiny_optional<int> paramId)
{
    if (!validMacro(macroId) || idx < 0)
    {
        return;
    }
    auto& mods = metadata.macros[macroId].modifiers;
    if (!paramId.has_value())
    {
        if (idx < int(mods.size()))
        {
            mods.erase(mods.begin() + idx);
        }
    }
    else if (idx < int(mods.size()))
    {
        mods[idx].paramId = *paramId;
    }
    else if (idx == int(mods.size()))
    {
        mods.push_back({ *paramId, 0.0f });
    }
}

void OpenPatch::setMacroModifierAmount(int macroId, int idx, float amount)
{
    if (auto* mod = modifier(macroId, idx))
    {
        mod->amount = std::clamp(amount,
                                 -Metadata::Macro::ParamModifier::kMaxAmount,
                                 Metadata::Macro::ParamModifier::kMaxAmount);
    }
}

tiny_optional<float> OpenPatch::getMacroModifierAmount(int macroId, int idx)
{
    auto* mod = modifier(macroId, idx);
    return mod ? tiny_optional<float>{ mod->amount } : tiny_optional<float>{ std::nullopt };
}

void OpenPatch::clearMacroModifiers()
{
    for (auto& macro : metadata.macros)
    {
        macro.modifiers.clear();
    }
}

void OpenPatch::changeMacroModifierAmount(int macroId, int idx, paramvalue_t delta)
{
    if (auto* mod = modifier(macroId, idx))
    {
        setMacroModifierAmount(macroId, idx, mod->amount + delta);
    }
}

void OpenPatch::setMacroBipolar(int macroId, bool bipolar)
{
    if (validMacro(macroId))
    {
        metadata.macros[macroId].bipolar = bipolar;
    }
}

void OpenPatch::setMacroDisplayName(int macroId, std::string name)
{
    if (validMacro(macroId))
    {
        name.resize(std::min<size_t>(name.size(), Metadata::Macro::kMaxDisplayNameLen));
        metadata.macros[macroId].displayName = std::move(name);
    }
}

Patch::Metadata::Macro::ParamModifier* OpenPatch::modifier(int macroId, int idx)
{
    if (!validMacro(macroId) || idx < 0 || idx >= int(metadata.macros[macroId].modifiers.size()))
    {
        return nullptr;
    }
    return &metadata.macros[macroId].modifiers[idx];
}

// ---------------------------------------------------------------- .patch transfer

void OpenPatch::exportData(PatchData& d) const
{
    d.engine = engineIdForType(metadata.instrumentTypeId);
    d.nParams = count;
    for (int i = 0; i < count; i++)
    {
        const auto& p = metadata.params[i];
        d.values[i] = float(p.minValue) == float(p.maxValue) ? NAN : values[i]; // unused slot: absent
    }
    d.nMacros = kPatchMacros;
    for (int m = 0; m < kPatchMacros; m++)
    {
        const auto& src = metadata.macros[m];
        PatchMacroData& dst = d.macros[m];
        std::strncpy(dst.name, src.displayName.c_str(), kMacroNameMax);
        dst.bipolar = src.bipolar;
        int n = 0;
        for (const auto& mod : src.modifiers)
        {
            if (mod.paramId.has_value() && n < kMacroTargets)
            {
                dst.param[n] = int16_t(*mod.paramId);
                dst.amount[n] = mod.amount;
                n++;
            }
        }
        dst.count = int8_t(n);
    }
    d.extra = extra;
}

void OpenPatch::valuesFromData(const PatchData& d, float* out) const
{
    for (int i = 0; i < count; i++)
    {
        const auto& p = metadata.params[i];
        const float v = i < d.nParams ? d.values[i] : NAN;
        const bool ok = std::isfinite(v) && (float(p.minValue) == float(p.maxValue) ||
                                             (v >= float(p.minValue) && v <= float(p.maxValue)));
        out[i] = ok ? v : p.defaultValue.value_or(0.0f);
    }
}

void OpenPatch::importData(const PatchData& d)
{
    valuesFromData(d, values);
    for (int m = 0; m < d.nMacros && m < kPatchMacros; m++)
    {
        const PatchMacroData& src = d.macros[m];
        auto& dst = metadata.macros[m];
        if (src.name[0])
        {
            dst.displayName = src.name;
        }
        else if (m < int(kUserMacroCount))
        {
            dst.displayName = std::string("Macro ") + char('1' + m);
        }
        if (dst.bipolar != src.bipolar)
        {
            macroCC[m] = src.bipolar ? 63 : 0;
        }
        dst.bipolar = src.bipolar;
        dst.modifiers.clear();
        for (int t = 0; t < src.count; t++)
        {
            if (src.param[t] >= 0 && src.param[t] < count)
            {
                dst.modifiers.push_back({ src.param[t], std::clamp(src.amount[t], -2.0f, 2.0f) });
            }
        }
    }
    extra = d.extra;
}

// ------------------------------------------------------------- patch store
// Patches/<ENGINE>/<name>.patch (typeFolders) or Workspace/<name>.patch, stock-compatible files.

namespace
{
// Heap buffer of the slot names file (1 KB). On the MK66 operator new is a plain malloc (teensy_core/new.cpp) and returns null when the
// heap is short, so std::make_unique would zero-fill from address 0 and fault. This returns null instead and the callers give up cleanly.
inline std::unique_ptr<uint8_t[]> tryBytes(size_t n)
{
    return std::unique_ptr<uint8_t[]>(new (std::nothrow) uint8_t[n]);
}

// The buffers of every .patch read and write (store and bank): the file bytes and the decoded data, 9.8 KB. They live in the SDRAM,
// not on the heap: on the device the heap had no 8 KB block left after a while and every slot select failed ("Error loading slot").
// Main thread only and never nested. A second lease while one is held gets nothing and its caller fails cleanly.
struct PatchScratch
{
    uint8_t file[kPatchFileMax + 1];
    PatchData data;
};
OPEN_EXTERNAL_RAM PatchScratch gPatchScratch;
bool gPatchScratchBusy = false;

class ScratchLease
{
  public:
    ScratchLease()
    {
        if (!gPatchScratchBusy)
        {
            gPatchScratchBusy = true;
            s = &gPatchScratch;
            new (&s->data) PatchData(); // fresh: the SDRAM is not initialised
        }
    }
    ~ScratchLease()
    {
        if (s)
        {
            gPatchScratchBusy = false;
        }
    }
    ScratchLease(const ScratchLease&) = delete;
    ScratchLease& operator=(const ScratchLease&) = delete;
    explicit operator bool() const { return s != nullptr; }
    uint8_t* buf() const { return s->file; }
    PatchData& data() const { return s->data; }

  private:
    PatchScratch* s = nullptr;
};

class OpenPatchStore final : public PatchStore
{
    static constexpr size_t kExtLen = 6; // ".patch"

  public:
    OpenPatchStore(const InstrumentTypeRegistry& registry, FileSystem* fs, bool typeFolders)
      : registry(registry)
      , fs(fs)
      , typeFolders(typeFolders)
    {
    }

    std::unique_ptr<Patch> createPatch(int typeId) override
    {
        // A type this build does not have (PERC, Shaping, an engine left out of SYNTH_ENGINES): the Init patch
        // of the first engine of this build
        if (!engineDefForType(typeId))
        {
            typeId = firstEngineTypeId();
        }
        auto type = registry.getInstrumentType(typeId);
        return std::make_unique<OpenPatch>(typeId, type.get());
    }
    std::vector<std::pair<std::string_view, int>> listPatchesFilter(
      std::function<bool(InstrumentTypeId)>) override
    {
        return {};
    }
    // The views point into the file system's own list buffer (valid until its next listFiles), cut
    // before ".patch". The editor copies exactly size() characters, so no terminator is needed.
    std::vector<std::string_view> listPatchesByType(int typeId) override
    {
        const EngineDef* def = engineDefForType(typeId);
        if (!fs || !def)
        {
            return {};
        }
        auto files = fs->listFiles(typeFolders ? def->name : "",
                                   [](const std::string& n)
                                   {
                                       return n.size() > kExtLen &&
                                              std::strcmp(n.c_str() + n.size() - kExtLen, ".patch") == 0;
                                   },
                                   std::nullopt);
        for (auto& f : files)
        {
            f = f.substr(0, f.size() - kExtLen);
        }
        for (size_t i = 1; i < files.size(); i++) // alphabetical, a few hundred names at most
        {
            for (size_t j = i; j > 0 && files[j] < files[j - 1]; j--)
            {
                std::swap(files[j], files[j - 1]);
            }
        }
        return files;
    }
    bool loadPatch(const std::string& name, Patch& patch, bool synchronizeAudio) override
    {
        OpenPatch& op = static_cast<OpenPatch&>(patch);
        const EngineDef* def = op.getDef();
        if (!fs || !def)
        {
            return false;
        }
        ScratchLease sc;
        if (!sc || readPatchFile(def, op.typeId(), name, sc.buf(), sc.data()) != FileRead::kOk)
        {
            return false;
        }
        if (synchronizeAudio)
        {
            OPEN_LOCK_AUDIO();
            op.importData(sc.data());
            OPEN_UNLOCK_AUDIO();
        }
        else
        {
            op.importData(sc.data());
        }
        return true;
    }
    bool savePatch(const std::string& name, const Patch& patch) override
    {
        const OpenPatch& op = static_cast<const OpenPatch&>(patch);
        const EngineDef* def = op.getDef();
        if (!fs || !def)
        {
            return false;
        }
        constexpr size_t kCap = 4096; // every file this writes is at most 4 KB
        static_assert(kCap <= sizeof(PatchScratch::file));
        ScratchLease sc;
        if (!sc)
        {
            return false;
        }
        op.exportData(sc.data());
        const size_t n = encodePatch(sc.data(), sc.buf(), kCap);
        if (n == 0)
        {
            return false;
        }
        if (typeFolders)
        {
            fs->createFolder(def->name);
        }
        auto h = fs->open(pathFor(def, name), FileSystem::OpenOptions::kWrite);
        if (!h)
        {
            return false;
        }
        const bool ok = fs->write(*h, std::span<const uint8_t>(sc.buf(), n));
        fs->close(*h);
        return ok;
    }
    std::optional<InstrumentTypeId> loadPatchInstrumentType(const std::string&) override
    {
        return std::nullopt;
    }
    // Reads and decodes one file of the engine `typeId` (buf holds kPatchFileMax + 1 bytes, out is fresh).
    enum class FileRead
    {
        kOk,
        kMissing, // no such file
        kOther,   // there, but damaged or of another engine
    };
    FileRead readPatchFile(const EngineDef* def, int typeId, const std::string& name, uint8_t* buf, PatchData& out)
    {
        if (!fs)
        {
            return FileRead::kMissing;
        }
        auto h = fs->open(pathFor(def, name), FileSystem::OpenOptions::kNone);
        if (!h)
        {
            return FileRead::kMissing;
        }
        size_t n = 0;
        int32_t got; // a read error is -1: it ends the loop like the end of the file
        while (n <= kPatchFileMax && (got = fs->read(*h, std::span<uint8_t>(buf + n, kPatchFileMax + 1 - n))) > 0)
        {
            n += size_t(got);
        }
        fs->close(*h);
        return decodePatch(buf, n, out) && typeForEngineId(out.engine) == typeId ? FileRead::kOk : FileRead::kOther;
    }
    // A whole small file by its full name (with extension) from the store's folder, at most `cap` bytes. 0 when missing or empty.
    size_t readRaw(const std::string& file, uint8_t* buf, size_t cap)
    {
        auto h = fs ? fs->open(file, FileSystem::OpenOptions::kNone) : std::nullopt;
        if (!h)
        {
            return 0;
        }
        size_t n = 0;
        int32_t got;
        while (n < cap && (got = fs->read(*h, std::span<uint8_t>(buf + n, cap - n))) > 0)
        {
            n += size_t(got);
        }
        fs->close(*h);
        return n;
    }
    bool writeRaw(const std::string& file, const uint8_t* data, size_t n)
    {
        auto h = fs ? fs->open(file, FileSystem::OpenOptions::kWrite) : std::nullopt;
        if (!h)
        {
            return false;
        }
        const bool ok = fs->write(*h, std::span<const uint8_t>(data, n));
        fs->close(*h);
        return ok;
    }

  private:
    std::string pathFor(const EngineDef* def, const std::string& name) const
    {
        return (typeFolders ? std::string(def->name) + "/" : std::string()) + name + ".patch";
    }

    const InstrumentTypeRegistry& registry;
    FileSystem* fs;
    bool typeFolders;
};

} // namespace

std::unique_ptr<PatchStore> makePatchStore(const InstrumentTypeRegistry& registry,
                                           FileSystem* fs,
                                           bool typeFolders)
{
    return std::make_unique<OpenPatchStore>(registry, fs, typeFolders);
}

// ------------------------------------------------------------- patch slots and bank (Patch step FX)
// Per part: the values (floats only, no macros) of every slot file of the part's engine. Built in the main thread, applied by
// a copy in the audio callback. The audio callback reads gLive (a plain pointer, one aligned word) and gBank, and the main
// thread changes gBank only with the audio interrupt disabled.

namespace
{
struct PartBank
{
    int typeId = -1;
    int nParams = 0;
    uint16_t valid = 0;    // bit n: slot n has a file of this engine (kSlot... rows of vals)
    uint8_t cur = 0;       // the slot open in the editor: the live patch holds its values
    int8_t from = -1;      // cur has no file yet: it began as a copy of this slot (-1: it has a file)
    bool applied = false;  // the FX replaced the live values, the edited ones wait in the last row of vals
    float* vals = nullptr; // (kPatchBankSlots + 1) * nParams: slot rows, then the shadow row
};
PartBank gBank[kPatchBankParts];
OpenPatch* volatile gLive[kPatchBankParts];

bool partOk(int part)
{
    return part >= 0 && part < kPatchBankParts;
}
} // namespace

void patchSlotName(int part, int slot, char (&out)[8])
{
    if (slot > 0)
    {
        snprintf(out, sizeof(out), "%d-%d", part, slot);
    }
    else
    {
        snprintf(out, sizeof(out), "%d", part);
    }
}

// ------------------------------------------------------------- slot names side file (<P>.names, see open_slotnames.h)

bool patchNamesRead(int part, PatchStore& workspace, SlotNames& out)
{
    out = SlotNames();
    if (!partOk(part))
    {
        return false;
    }
    char base[8];
    patchSlotName(part, 0, base);
    auto buf = tryBytes(kSlotNamesFileMax);
    if (!buf)
    {
        return false;
    }
    const size_t n = static_cast<OpenPatchStore&>(workspace).readRaw(std::string(base) + kSlotNamesExt, buf.get(), kSlotNamesFileMax);
    return n > 0 && decodeSlotNames(buf.get(), n, out);
}

bool patchNamesWrite(int part, PatchStore& workspace, const SlotNames& in)
{
    if (!partOk(part))
    {
        return false;
    }
    OpenPatchStore& st = static_cast<OpenPatchStore&>(workspace);
    char base[8];
    patchSlotName(part, 0, base);
    const std::string file = std::string(base) + kSlotNamesExt;
    uint8_t probe[1];
    if (!anySlotName(in) && st.readRaw(file, probe, 1) == 0)
    {
        return true; // nothing to say and no file to correct: a project that never named a slot gets no side file
    }
    auto buf = tryBytes(kSlotNamesFileMax); // not on the stack: the audio interrupt runs on the same one
    if (!buf)
    {
        return false;
    }
    const size_t n = encodeSlotNames(in, buf.get(), kSlotNamesFileMax);
    return n > 0 && st.writeRaw(file, buf.get(), n);
}

void patchBankSetLive(int part, Patch* patch)
{
    if (partOk(part))
    {
        gLive[part] = static_cast<OpenPatch*>(patch);
    }
}

void patchBankPreload(int part, Patch& patch, PatchStore& workspace)
{
    if (!partOk(part))
    {
        return;
    }
    OpenPatch& op = static_cast<OpenPatch&>(patch);
    OpenPatchStore& st = static_cast<OpenPatchStore&>(workspace);
    const EngineDef* def = op.getDef();
    const int n = op.paramCount();
    PartBank nb;
    nb.typeId = op.typeId();
    nb.nParams = n;
    if (def && n > 0)
    {
        nb.vals = new (std::nothrow) float[size_t(kPatchBankSlots + 1) * size_t(n)];
    }
    if (nb.vals)
    {
        ScratchLease sc;
        for (int i = 0; sc && i < kPatchBankSlots; i++) // no buffers: an empty bank, the FX does nothing
        {
            char name[8];
            patchSlotName(part, i, name);
            new (&sc.data()) PatchData(); // fresh in place: `data = PatchData()` put a 1.6 KB temporary on the stack
            if (st.readPatchFile(def, nb.typeId, name, sc.buf(), sc.data()) == OpenPatchStore::FileRead::kOk)
            {
                op.valuesFromData(sc.data(), nb.vals + size_t(i) * size_t(n));
                nb.valid |= uint16_t(1u << i);
            }
        }
    }
    OPEN_LOCK_AUDIO();
    std::swap(gBank[part], nb);
    OPEN_UNLOCK_AUDIO();
    delete[] nb.vals; // the previous bank
}

void patchBankToSlotZero(int part)
{
    if (!partOk(part))
    {
        return;
    }
    OPEN_LOCK_AUDIO();
    gBank[part].cur = 0;
    gBank[part].from = -1;
    gBank[part].applied = false; // the project's own values are loaded next, the shadow row is stale
    OPEN_UNLOCK_AUDIO();
}

int patchBankCurrent(int part)
{
    return partOk(part) ? gBank[part].cur : 0;
}

namespace
{
// Bank usable with the live patch of the part (same engine and size). Main thread or audio callback.
inline bool bankMatches(const PartBank& b, const OpenPatch* p)
{
    return p && b.vals && b.typeId == p->typeId() && b.nParams == p->paramCount();
}
} // namespace

void patchBankRestore(int part)
{
    if (!partOk(part))
    {
        return;
    }
    OPEN_LOCK_AUDIO();
    PartBank& b = gBank[part];
    OpenPatch* p = gLive[part];
    if (b.applied && bankMatches(b, p))
    {
        p->setValuesRaw(b.vals + size_t(kPatchBankSlots) * size_t(b.nParams));
    }
    b.applied = false;
    OPEN_UNLOCK_AUDIO();
}

void patchBankCommit(int part)
{
    if (!partOk(part))
    {
        return;
    }
    OPEN_LOCK_AUDIO();
    PartBank& b = gBank[part];
    OpenPatch* p = gLive[part];
    if (bankMatches(b, p) && !b.applied)
    {
        p->getValuesRaw(b.vals + size_t(b.cur) * size_t(b.nParams));
        b.valid |= uint16_t(1u << b.cur);
        b.from = -1;
    }
    OPEN_UNLOCK_AUDIO();
}

SlotSelect patchBankSelect(int part, int slot, Patch& patch, PatchStore& workspace)
{
    if (!partOk(part) || slot < 0 || slot >= kPatchBankSlots)
    {
        return SlotSelect::kError;
    }
    PartBank& b = gBank[part];
    if (slot == b.cur)
    {
        return SlotSelect::kSame;
    }
    patchBankRestore(part);
    OpenPatch& op = static_cast<OpenPatch&>(patch);
    OpenPatchStore& st = static_cast<OpenPatchStore&>(workspace);
    const EngineDef* def = op.getDef();
    if (!def)
    {
        return SlotSelect::kError;
    }
    char name[8];
    patchSlotName(part, slot, name);
    ScratchLease sc;
    if (!sc)
    {
        return SlotSelect::kError;
    }
    const auto state = st.readPatchFile(def, op.typeId(), name, sc.buf(), sc.data());
    if (state == OpenPatchStore::FileRead::kOther)
    {
        return SlotSelect::kOtherEngine;
    }
    const int8_t origin = b.from >= 0 ? b.from : int8_t(b.cur); // the file a pending copy began from
    if (state == OpenPatchStore::FileRead::kOk)
    {
        OPEN_LOCK_AUDIO();
        op.importData(sc.data());
        OPEN_UNLOCK_AUDIO();
    }
    OPEN_LOCK_AUDIO();
    b.cur = uint8_t(slot);
    b.from = state == OpenPatchStore::FileRead::kOk ? int8_t(-1) : origin;
    OPEN_UNLOCK_AUDIO();
    return state == OpenPatchStore::FileRead::kOk ? SlotSelect::kLoaded : SlotSelect::kCopied;
}

bool patchBankRevert(int part, Patch& patch, PatchStore& workspace)
{
    if (!partOk(part))
    {
        return false;
    }
    PartBank& b = gBank[part];
    patchBankRestore(part);
    OpenPatch& op = static_cast<OpenPatch&>(patch);
    OpenPatchStore& st = static_cast<OpenPatchStore&>(workspace);
    const EngineDef* def = op.getDef();
    if (!def)
    {
        return false;
    }
    char name[8];
    patchSlotName(part, b.from >= 0 ? b.from : b.cur, name);
    ScratchLease sc;
    if (!sc || st.readPatchFile(def, op.typeId(), name, sc.buf(), sc.data()) != OpenPatchStore::FileRead::kOk)
    {
        return false;
    }
    OPEN_LOCK_AUDIO();
    op.importData(sc.data());
    OPEN_UNLOCK_AUDIO();
    return true;
}

bool patchBankApply(int part, int slot)
{
    if (!partOk(part) || slot < 0 || slot >= kPatchBankSlots)
    {
        return false;
    }
    PartBank& b = gBank[part];
    OpenPatch* p = gLive[part];
    if (!bankMatches(b, p))
    {
        return false;
    }
    const size_t n = size_t(b.nParams);
    float* shadow = b.vals + size_t(kPatchBankSlots) * n;
    if (slot == b.cur)
    {
        if (b.applied)
        {
            p->setValuesRaw(shadow); // back to the edited values of the open slot
            b.applied = false;
        }
        return true;
    }
    if (!(b.valid & (1u << slot)))
    {
        return false;
    }
    if (!b.applied)
    {
        p->getValuesRaw(shadow); // keep the edited values of the open slot
        b.applied = true;
    }
    p->setValuesRaw(b.vals + size_t(slot) * n);
    return true;
}

} // namespace open

} // namespace polysynth
