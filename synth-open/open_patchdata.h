// .patch file model and codec interface of the open PolySynth shim (codec in open_patchio.cpp).
// Plain data, no PolySynth includes, so the codec object stays small.
#pragma once

#include <cstddef>
#include <cstdint>

namespace polysynth::open
{

constexpr int kMaxParams = 84; // PERC has 84


// ---- .patch file model (codec in open_patchio.cpp, see there)

constexpr int kPatchMacros = 8;          // 6 user + Velocity + Pressure
constexpr int kMacroTargets = 6;         // targets per macro in the file
constexpr int kMacroNameMax = 20;        // Patch::Metadata::Macro::kMaxDisplayNameLen
constexpr size_t kPatchFileMax = 8192;   // larger files are refused (as OpenTracker)
constexpr size_t kRawMax = 768;          // names, unknown and extension fields kept verbatim

struct PatchMacroData
{
    char name[kMacroNameMax + 1] = {};
    bool bipolar = false;
    int8_t count = 0;                    // live targets
    int16_t param[kMacroTargets] = {};
    float amount[kMacroTargets] = {};
};

struct PatchExtra
{
    uint16_t len = 0;
    bool hasNames = false;               // the raw bytes hold the four name fields
    uint8_t bytes[kRawMax] = {};
};

struct PatchData
{
    int32_t engine = 0;                  // OpenTracker / Polyend engine id (ACD 1, FAT 2, VAP 4, WTFM 5, PERC 6)
    int nParams = 0;
    float values[kMaxParams] = {};       // NaN = absent (the parameter default)
    int nMacros = 0;
    PatchMacroData macros[kPatchMacros];
    PatchExtra extra;
};

int engineIdForType(int typeId);         // 0 when unknown
int typeForEngineId(int engine);         // -1 when unknown
bool decodePatch(const uint8_t* data, size_t len, PatchData& out);   // out must be fresh (default constructed)
size_t encodePatch(const PatchData& p, uint8_t* out, size_t cap);    // bytes written, 0 on failure

} // namespace polysynth::open
