// Host smoke test of the open shim (NOT firmware, not built by make). Build and run on the Mac (x86_64 via Rosetta):
//   clang++ -arch x86_64 -std=c++20 -O1 "-D_GLIBCXX_THROW_OR_ABORT(x)=__builtin_abort()" -I synth/include -I util/include \
//     -I util/3rdparty -I synth-open -I synth-open/ot -DFEATURE_SYNTH_OPEN -DOPEN_HOST_TEST synth-open/tools/host_smoke_test.cpp \
//     synth-open/open_synth.cpp synth-open/open_cold.cpp synth-open/open_patchio.cpp synth-open/engine_{acd,fat,vap,wtfm}.cpp synth-open/ot/audio/engines/{acd,fat,vap,wtfm}.cpp \
//     synth-open/ot/audio/{governor,cpu_meter}.cpp \
//     -o /tmp/smoke && /tmp/smoke
// Optional OpenTracker oracle (ref/opentracker, not in the repository): add -DOT_ORACLE -I ref/opentracker/src/core
// ref/opentracker/src/core/formats/patch_pb.cpp to the line above.
// Checks: ACD metadata (29 params, 6 sections), note on -> pitch/amplitude, 2 voices, steal, release to silence, macro CC,
// then one note per engine of the picked set (engine_list.h, e.g. ACD FAT VAP WTFM PERC CHIP JU6 PHZ): metadata counts, output finite and non-silent, fails with exit 1 otherwise.
// PERC also gets one hit per drum (kick, tom, snare, hats, cymbal, perc) and the checks of its one voice kit.
// Then the voice pool and the governor: 6 notes over 3 slots sound at most 4 voices (the oldest are stolen), budget 8 and the
// per-instrument cap, a simulated overload (block cycles) makes the governor cut with hold-off, calm blocks restore one voice
// at a time, Stop and offline render reset it (they use the first engine of the set and 2 voices per instrument, so a set that starts with PERC, 1 voice, fails them: the canonical order puts PERC after WTFM, only PERC alone starts with it). Then .patch save/load on an in-memory file system (all picked engines, then references to an engine this build lacks fall back safely).
#include "open_load.h"
#include "open_patchdata.h"
#include "open_patch.h"
#include "PolySynth/FileSystem.h"
#include <map>
#ifdef OT_ORACLE
#include "formats/patch_pb.h"
#endif
#include "PolySynth/Instrument.h"
#include "PolySynth/MidiSource.h"
#include "PolySynth/PatchStore.h"
#include "PolySynth/Synth.h"
#include "PolySynth/sdk/InstrumentTypeRegistry.h"
#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>
#ifdef SYNTH_ENGINE_PHZ
#include "phz/phz.h"
#endif
namespace polysynth {
std::unique_ptr<InstrumentType> createMidiOut();
// One factory per engine picked with -DSYNTH_ENGINE_<NAME> (synth-open/engine_list.h), as core/synth/registry.cpp
#define SYNTH_ENGINE(NAME, STORED, TYPE_ID, FACTORY, ...) std::unique_ptr<InstrumentType> FACTORY();
#include "engine_list.h"
#undef SYNTH_ENGINE
void InstrumentTypeRegistry::registerInstrumentTypes(InstrumentTypeRegistry& r, const Synth::Allocator&) {
    r.addInstrumentType(int(InstrumentTypeId::kMidiOut), createMidiOut());
#define SYNTH_ENGINE(NAME, STORED, TYPE_ID, FACTORY, ...) r.addInstrumentType(int(TYPE_ID), FACTORY());
#include "engine_list.h"
#undef SYNTH_ENGINE
}
}
using namespace polysynth;
struct Src : MidiSource { Src():MidiSource(Type::kInternalKeyboard){} std::weak_ptr<MidiEventCallback> cb;
  void subscribeMidiEvents(int, std::weak_ptr<MidiEventCallback> c) override { cb = c; }
  void send(MidiEvent::Type t, int ch, int a, int b) { MidiEvent e{}; e.type=t; e.channel=ch; e.data.rawData.value1=a; e.data.rawData.value2=b; (*cb.lock())(e,0);} };
static float peakOf(std::array<float,128>& l){ float p=0; for(float v:l){ if(!std::isfinite(v)) return INFINITY; p=std::max(p,std::fabs(v)); } return p; }
// In-memory FileSystem for the patch store tests: "dir/name" keys, one level of folders.
struct MemFS : FileSystem {
  struct H { std::string path; size_t pos; bool w; };
  std::map<std::string, std::vector<uint8_t>> files; std::vector<std::string> listed; std::vector<H*> hs;
  std::vector<std::string_view> listFiles(const std::string& dir, std::function<bool(const std::string&)> m, tiny_optional<int>) override {
    listed.clear(); const std::string pre = dir.empty() ? "" : dir + "/";
    for (auto& [k, v] : files) if (k.compare(0, pre.size(), pre) == 0 && k.find('/', pre.size()) == std::string::npos && m(k.substr(pre.size()))) listed.push_back(k.substr(pre.size()));
    return std::vector<std::string_view>(listed.begin(), listed.end()); }
  std::optional<FileHandle> open(const std::string& path, OpenOptions o) override {
    const bool w = o == OpenOptions::kWrite; if (!w && !files.count(path)) return std::nullopt; if (w) files[path].clear();
    hs.push_back(new H{path, 0, w}); return hs.back(); }
  void close(FileHandle) override {}
  bool write(FileHandle h, std::span<const uint8_t> d) override { auto* x = (H*)h; files[x->path].insert(files[x->path].end(), d.begin(), d.end()); return true; }
  int32_t read(FileHandle h, std::span<uint8_t> b) override { auto* x = (H*)h; auto& f = files[x->path]; size_t n = std::min(b.size(), f.size() - x->pos); std::copy_n(f.begin() + x->pos, n, b.begin()); x->pos += n; return (int32_t)n; }
  bool seek(FileHandle, int) override { return true; }
  void createFolder(const std::string&) override {}
};
int main() {
  auto synth = Synth::createSynth([](size_t){return nullptr;});
  auto store = synth->createPatchStore(Synth::FileSystemId::kPatches, true);
  const int kLead = open::firstEngineTypeId();   // the first engine of this build (ACD unless left out)
  auto patch = std::shared_ptr<Patch>(store->createPatch(kLead));
  const auto& md = patch->getMetadata();
  printf("params=%zu sections=%zu macros=%zu maxVoices=%d\n", md.params.size(), md.layoutFlat.sections.size(), md.macros.size(), md.maxVoices);
#ifdef SYNTH_ENGINE_ACD
  for (int i : {5, 9, 15, 23, 28}) printf("  [%d] %s = %s (editmapped %.3f)\n", i, md.params[i].displayName.c_str(), md.params[i].valueFormatter(*patch, *patch->getParamValue(i)).c_str(), *patch->getParamValueEditMapped(i));
  patch->setParamValueEditMapped(9, 0.5f, Patch::ParamChangeSource::kKnob);
  printf("  cutoff at knob 0.5 = %s\n", md.params[9].valueFormatter(*patch,*patch->getParamValue(9)).c_str());
  patch->setParamValueEditMapped(9, 0.7f, Patch::ParamChangeSource::kKnob);
#endif
  Src src;
  auto inst = synth->createInstrument(0);
  inst->assignPatch(patch); inst->connect(src); inst->setVoiceCount(2); inst->setMidiChannel(0);
  std::shared_ptr<Instrument> arr[1] = { inst };
  synth->setup(std::span<std::shared_ptr<Instrument>>(arr,1), {}, nullptr, []{ return tiny::optional<float>(120.0f); });
  std::array<float,128> L, R, A1, A2; std::array<AudioBufferRef,2> aux{AudioBufferRef(A1),AudioBufferRef(A2)};
  auto blk=[&](){ synth->renderAudioBuffer({AudioBufferRef(L),AudioBufferRef(R)}, std::span<AudioBufferRef>(aux), 0); return peakOf(L); };
  printf("silent peak=%.4f\n", blk());
  src.send(MidiEvent::Type::kNoteOn, 0, 57, 100);   // A3
  float pk=0; int zc=0; float prev=0; double e=0;
  for (int b=0;b<40;b++){ float p=blk(); pk=std::max(pk,p); for(float v:L){ if((v>0)!=(prev>0)) zc++; prev=v; e+=v*v; } }
  printf("noteOn A3: peak=%.4f rms=%.4f zero-cross/5120 smp=%d (A3=220Hz -> ~%d)\n", pk, std::sqrt(e/5120), zc, int(2*220*5120/44117.6));
  src.send(MidiEvent::Type::kNoteOn, 0, 64, 127);   // second voice
  pk=0; for (int b=0;b<20;b++) pk=std::max(pk,blk()); printf("2 voices peak=%.4f\n", pk);
  src.send(MidiEvent::Type::kNoteOn, 0, 72, 127);   // third: steal
  pk=0; for (int b=0;b<20;b++) pk=std::max(pk,blk()); printf("3rd note (steal) peak=%.4f finite=%d\n", pk, std::isfinite(pk));
  src.send(MidiEvent::Type::kNoteOff, 0, 64, 0); src.send(MidiEvent::Type::kNoteOff, 0, 72, 0);
  float last=0; for (int b=0;b<600;b++) last=blk(); printf("after release+1.7s peak=%.6f (expect ~0)\n", last);
  // macro CC 102 (cutoff)
  src.send(MidiEvent::Type::kNoteOn, 0, 57, 100); src.send(MidiEvent::Type::kControlChange, 0, 102, 127);
#ifdef SYNTH_ENGINE_ACD
  printf("final cutoff incl macro=%s\n", md.params[9].valueFormatter(*patch,*patch->getParamFinalValue(9)).c_str());
#endif
  pk=0; for (int b=0;b<20;b++) pk=std::max(pk,blk()); printf("with macro peak=%.4f\n", pk);
  // One note per engine: default patch, 40 blocks, output must be finite and non-silent
  struct E { const char* name; InstrumentTypeId id; size_t params; int note; } engines[] = {
#define SYNTH_ENGINE(NAME, STORED, TYPE_ID, FACTORY, DEF, ...) {#NAME, TYPE_ID, size_t(open::DEF.paramCount), TYPE_ID == InstrumentTypeId::kDrumSynth || int(TYPE_ID) == 0x85 ? 60 : 57},
#include "engine_list.h"
#undef SYNTH_ENGINE
  };
  int bad = 0;
  for (const auto& e : engines) {
    auto ep = std::shared_ptr<Patch>(store->createPatch(int(e.id)));
    if (ep->getMetadata().instrumentTypeId != int(e.id)) { printf("%s: patch has the wrong type id\n", e.name); bad++; }
    const auto& emd = ep->getMetadata();
    auto ei = synth->createInstrument(0);
    Src es; ei->assignPatch(ep); ei->connect(es); ei->setVoiceCount(2); ei->setMidiChannel(0);
    std::shared_ptr<Instrument> earr[1] = { ei };
    synth->setup(std::span<std::shared_ptr<Instrument>>(earr,1), {}, nullptr, []{ return tiny::optional<float>(120.0f); });
    es.send(MidiEvent::Type::kNoteOn, 0, e.note, 100);
    float epk = 0; double en = 0;
    for (int b = 0; b < 40; b++) { float p = blk(); epk = std::max(epk, p); for (float v : L) en += v * v; }
    bool ok = std::isfinite(epk) && epk > 1e-4f && emd.params.size() == e.params && !emd.layoutFlat.sections.empty();
    printf("%-5s params=%zu sections=%zu peak=%.4f rms=%.4f %s\n", e.name, emd.params.size(), emd.layoutFlat.sections.size(), epk, std::sqrt(en / 5120), ok ? "OK" : "FAIL");
    bad += !ok;
  }
  printf(bad ? "ENGINE CHECK FAILED\n" : "all engines OK\n");

  // ---- global voice pool and governor: 3 ACD slots, 2 voices each per instrument, pool default 4
  auto check = [&](bool ok, const char* what) { printf("  %-62s %s\n", what, ok ? "OK" : "FAIL"); bad += !ok; };
  auto s3 = Synth::createSynth([](size_t){return nullptr;});
  auto st3 = s3->createPatchStore(Synth::FileSystemId::kPatches, true);
  std::shared_ptr<Patch> pat3[3]; std::shared_ptr<Instrument> ins3[3]; Src src3[3];
  for (int k = 0; k < 3; k++) {
    pat3[k] = std::shared_ptr<Patch>(st3->createPatch(kLead));
    ins3[k] = s3->createInstrument(k); ins3[k]->assignPatch(pat3[k]); ins3[k]->connect(src3[k]);
    ins3[k]->setVoiceCount(2); ins3[k]->setMidiChannel(0);
  }
  s3->setup(std::span<std::shared_ptr<Instrument>>(ins3,3), {}, nullptr, []{ return tiny::optional<float>(120.0f); });
  float slotPk[3];
  auto renderAll = [&]() { for (int k = 0; k < 3; k++) { s3->renderAudioBuffer({AudioBufferRef(L),AudioBufferRef(R)}, std::span<AudioBufferRef>(aux), k); slotPk[k] = peakOf(L); } };
  printf("voice pool: budget=%d cap=%d (default 4, max 8)\n", open::voiceBudget(), open::voiceCap());
  check(open::voiceBudget() == 4 && open::voiceCap() == 4, "default budget 4");
  int maxSounding = 0;
  const int notes[6][2] = {{0,60},{0,64},{1,67},{1,71},{2,72},{2,76}};   // {slot, note}: 6 notes over 3 slots
  for (const auto& n : notes) {
    src3[n[0]].send(MidiEvent::Type::kNoteOn, 0, n[1], 100);
    maxSounding = std::max(maxSounding, open::voicesSounding());
    renderAll(); renderAll();   // age the voices so "oldest" is defined
    maxSounding = std::max(maxSounding, open::voicesSounding());
  }
  for (int b = 0; b < 4; b++) renderAll();
  printf("6 notes over 3 slots: max voices sounding=%d final=%d slot peaks %.3f %.3f %.3f\n", maxSounding, open::voicesSounding(), slotPk[0], slotPk[1], slotPk[2]);
  check(maxSounding <= 4 && open::voicesSounding() == 4, "6 notes sound at most 4 voices (pool full)");
  check(slotPk[0] < 1e-4f && slotPk[1] > 1e-3f && slotPk[2] > 1e-3f, "the two oldest notes (slot 0) were stolen and went silent");
  open::setVoiceBudget(99);
  check(open::voiceBudget() == 8, "budget clamps to 8");
  src3[0].send(MidiEvent::Type::kNoteOn, 0, 60, 100); src3[0].send(MidiEvent::Type::kNoteOn, 0, 64, 100); renderAll();
  check(open::voicesSounding() == 6, "budget 8: 3 slots x 2 voices = 6 (per-instrument cap 2 kept)");
  src3[0].send(MidiEvent::Type::kNoteOn, 0, 62, 100);
  check(open::voicesSounding() == 6, "third note on one slot steals inside the slot (cap 2)");
  open::setVoiceBudget(4);
  check(open::voicesSounding() == 4 && open::voiceCap() == 4, "budget back to 4 steals down to 4");
  for (int b = 0; b < 4; b++) renderAll();

  // simulated overload: 1015 permille blocks (the block budget is 522,240 cycles)
  const uint32_t kOver = 530000, kCalm = 156672;   // 1015 and 300 permille
  open::reportBlockCycles(kOver);
  printf("overload block: cap=%d sounding=%d cuts=%u\n", open::voiceCap(), open::voicesSounding(), open::governorCuts());
  check(open::voiceCap() == 3 && open::voicesSounding() == 3 && open::governorCuts() == 1, "overrun block cuts one voice (4 -> 3)");
  for (int b = 0; b < 80; b++) { open::reportBlockCycles(kOver); renderAll(); }
  check(open::governorCuts() == 1 && open::voiceCap() == 3, "no second cut within the 250 ms hold-off");
  src3[0].send(MidiEvent::Type::kNoteOn, 0, 50, 100); renderAll();
  check(open::voicesSounding() <= 3, "new notes respect the lowered cap");
  for (int b = 0; b < 40; b++) { open::reportBlockCycles(kOver); renderAll(); }
  printf("after 120 overload blocks: cap=%d sounding=%d cuts=%u\n", open::voiceCap(), open::voicesSounding(), open::governorCuts());
  check(open::governorCuts() >= 2 && open::voiceCap() <= 2, "sustained overload cuts again after the hold-off");
  int cap = open::voiceCap(), steps = 0, blocks = 0; bool stepOk = true;
  for (; blocks < 6000 && open::voiceCap() < 4; blocks++) {
    open::reportBlockCycles(kCalm); renderAll();
    if (open::voiceCap() != cap) { stepOk = stepOk && open::voiceCap() == cap + 1; cap = open::voiceCap(); steps++; printf("  restore at block %d: cap=%d\n", blocks, cap); }
  }
  check(open::voiceCap() == 4 && stepOk && steps >= 2, "calm blocks restore one voice at a time up to the budget (>= 2 s each)");
  check(blocks >= 690 * steps, "each restore needs 690 calm blocks (2 s)");
  for (const auto& n : notes) src3[n[0]].send(MidiEvent::Type::kNoteOn, 0, n[1] + 1, 100);
  check(open::voicesSounding() == 4, "after the restore the pool sounds 4 voices again");
  // Stop resets, offline render switches the governor off
  open::reportBlockCycles(kOver);
  check(open::voiceCap() == 3, "cut again");
  src3[0].send(MidiEvent::Type::kStop, 0, 0, 0);
  check(open::voiceCap() == 4, "transport Stop resets the cap to the budget");
  open::reportBlockCycles(kOver);
  open::setOffline(true);
  const uint32_t cutsOffline = open::governorCuts();
  for (int b = 0; b < 200; b++) open::reportBlockCycles(kOver);
  check(open::voiceCap() == 4 && open::governorCuts() == cutsOffline, "offline render: governor off, cap at the budget");
  open::setOffline(false);
  // project load: cap back at the budget, hold-off cleared (a cut right after), no voice left
  src3[0].send(MidiEvent::Type::kNoteOn, 0, 60, 100); renderAll();
  open::reportBlockCycles(kOver);
  check(open::voiceCap() < 4 && open::voicesSounding() > 0, "overload again: cap cut below the budget, voices sounding");
  open::resetOnProjectLoad();
  check(open::voiceCap() == 4 && open::voicesSounding() == 0, "project load: cap back at the budget, voices cut");
  src3[0].send(MidiEvent::Type::kNoteOn, 0, 60, 100); renderAll();   // inside the old 250 ms hold-off
  open::reportBlockCycles(kOver);
  check(open::voiceCap() < 4, "project load clears the hold-off: next overrun cuts at once");
  open::resetOnProjectLoad();
  // ---- 4 voices per instrument: the cap clamps at 4, one slot can fill the default pool, a fifth note steals inside the slot
  {
    ins3[0]->setVoiceCount(9);
    check(ins3[0]->getVoiceCount() == 4, "Voices above 4 clamp to 4 (was 2)");
    ins3[0]->setVoiceCount(4); ins3[1]->setVoiceCount(4); ins3[2]->setVoiceCount(4);
    const int chord[5] = {48, 55, 60, 64, 67};
    for (int k = 0; k < 4; k++) { src3[0].send(MidiEvent::Type::kNoteOn, 0, chord[k], 100); renderAll(); renderAll(); }
    check(open::voicesSounding() == 4, "4 notes on one slot: 4 voices sound (pool default 4)");
    src3[0].send(MidiEvent::Type::kNoteOn, 0, chord[4], 100); renderAll(); renderAll();
    check(open::voicesSounding() == 4, "a fifth note steals inside the slot, still 4");
    src3[1].send(MidiEvent::Type::kNoteOn, 0, 72, 100); renderAll(); renderAll(); renderAll();
    check(open::voicesSounding() == 4 && slotPk[1] > 1e-3f, "a note on slot 2 takes a voice from the full pool (global victim), still 4");
    open::setVoiceBudget(8);
    for (int k = 0; k < 4; k++) { src3[2].send(MidiEvent::Type::kNoteOn, 0, 50 + 4 * k, 100); renderAll(); }
    src3[1].send(MidiEvent::Type::kNoteOn, 0, 76, 100); src3[1].send(MidiEvent::Type::kNoteOn, 0, 79, 100); src3[1].send(MidiEvent::Type::kNoteOn, 0, 83, 100); renderAll();
    src3[0].send(MidiEvent::Type::kNoteOn, 0, 40, 100); src3[0].send(MidiEvent::Type::kNoteOn, 0, 43, 100); renderAll(); renderAll();
    check(open::voicesSounding() == 8, "budget 8: pool full at 8 with 3 slots asking for 12");
    open::setVoiceBudget(4);
    for (int b = 0; b < 4; b++) renderAll();
    check(open::voicesSounding() == 4, "budget 4 again: down to 4");
    for (int k = 0; k < 3; k++) ins3[k]->setVoiceCount(2);
    open::resetOnProjectLoad();
  }
  // ---- .patch save / load (open_patchio.cpp + OpenPatchStore) on an in-memory file system
  {
    auto s4 = Synth::createSynth([](size_t){return nullptr;});
    auto fsP = std::make_unique<MemFS>(); MemFS* mp = fsP.get();
    auto fsW = std::make_unique<MemFS>(); MemFS* mw = fsW.get();
    s4->addFileSystem(Synth::FileSystemId::kPatches, std::move(fsP)); s4->addFileSystem(Synth::FileSystemId::kWorkspace, std::move(fsW));
    auto ps = s4->createPatchStore(Synth::FileSystemId::kPatches, true);
    auto ws = s4->createPatchStore(Synth::FileSystemId::kWorkspace, false);
    const struct { const char* name; InstrumentTypeId id; } kinds[] = {
#define SYNTH_ENGINE(NAME, STORED, TYPE_ID, ...) {#NAME, TYPE_ID},
#include "engine_list.h"
#undef SYNTH_ENGINE
    };
    const int nKinds = int(sizeof(kinds) / sizeof(kinds[0]));
    bool allSame = true, allIdem = true, allList = true;
    for (const auto& k : kinds) {
      auto a = ps->createPatch(int(k.id)); const auto& am = a->getMetadata();
      for (size_t i = 0; i < am.params.size(); i++) { const float lo = float(am.params[i].minValue), hi = float(am.params[i].maxValue); a->setParamValue(int(i), lo + (hi - lo) * (0.1f + 0.8f * float((i * 37) % 11) / 10.0f), Patch::ParamChangeSource::kKnob); }
      a->setMacroDisplayName(0, "My Macro"); a->setMacroBipolar(1, true); a->setMacroModifier(2, 0, 3); a->setMacroModifierAmount(2, 0, -0.75f);
      a->setMacroModifier(2, 1, 1); a->setMacroModifierAmount(2, 1, 0.5f);
      if (!ps->savePatch("t", *a)) { allSame = false; continue; }
      const auto& bytes = mp->files[std::string(k.name) + "/t.patch"];
      auto b = ps->createPatch(int(k.id)); const bool loaded = ps->loadPatch("t", *b, true);
      bool same = loaded;
      for (size_t i = 0; i < am.params.size() && same; i++) { same = *a->getParamValue(int(i)) == *b->getParamValue(int(i)); if (!same) printf("  %s param %zu (%s) saved %.9g loaded %.9g range %g..%g\n", k.name, i, am.params[i].displayName.c_str(), *a->getParamValue(int(i)), *b->getParamValue(int(i)), double(am.params[i].minValue), double(am.params[i].maxValue)); }
      const auto& bm = b->getMetadata();
      for (int m = 0; m < int(kMacroCount) && same; m++) {
        same = am.macros[m].displayName == bm.macros[m].displayName && am.macros[m].bipolar == bm.macros[m].bipolar && am.macros[m].modifiers.size() == bm.macros[m].modifiers.size();
        for (size_t t = 0; same && t < am.macros[m].modifiers.size(); t++) same = am.macros[m].modifiers[t].paramId == bm.macros[m].modifiers[t].paramId && am.macros[m].modifiers[t].amount == bm.macros[m].modifiers[t].amount;
      }
      printf("  %-5s .patch %zu bytes, round trip %s\n", k.name, bytes.size(), same ? "equal" : "DIFFERENT");
      allSame = allSame && same && bytes.size() > 1300 && bytes.size() < open::kPatchFileMax;
      ps->savePatch("t2", *b); allIdem = allIdem && mp->files[std::string(k.name) + "/t2.patch"] == bytes;
      auto names = ps->listPatchesByType(int(k.id)); allList = allList && names.size() == 2 && names[0] == "t" && names[1] == "t2";
    }
    check(allSame, "save -> load gives equal params and macros (all engines of this build)");
    check(allIdem, "load -> save writes the identical bytes (all engines of this build)");
    check(allList, "patch browser lists the saved patches, sorted, without the extension");
    // wrong engine, damaged file, missing file: load fails and leaves the patch alone
    const auto& kLast = kinds[nKinds - 1]; const std::string lastName = kLast.name, firstName = kinds[0].name;
    auto fat = ps->createPatch(int(kLast.id)); const float before = *fat->getParamValue(0);
    mp->files[lastName + "/short.patch"] = mp->files[lastName + "/t.patch"]; { // cut inside a field: the first length at which the file no longer decodes
      auto& f = mp->files[lastName + "/short.patch"]; const auto whole = f;
      for (size_t n = 60; n < whole.size(); n++) { open::PatchData tmp; if (!open::decodePatch(whole.data(), n, tmp)) { f.resize(n); break; } } }
    auto& trunc = mp->files[lastName + "/short.patch"];
    if (nKinds > 1) { mp->files[lastName + "/wrong.patch"] = mp->files[firstName + "/t.patch"];
      check(!ps->loadPatch("wrong", *fat, true), "a file of the first engine does not load into a patch of the last engine"); }
    check(!ps->loadPatch("short", *fat, true), "a file cut in the middle of a field is refused");
    check(!ps->loadPatch("missing", *fat, true) && *fat->getParamValue(0) == before, "a missing file fails and the patch is untouched");
    // An engine this build lacks: a .patch of it, its type id from a stock project and its folder must all be harmless
    // (no crash, no load, nothing listed, an Init patch of the first engine instead). PERC (.patch engine 6, Polyend
    // kDrumSynth) is that engine unless it is picked, else it is Shaping (kShapingSynth, never built, .patch engine 0x7E)
    {
#ifdef SYNTH_ENGINE_PERC
      const InstrumentTypeId kAbsent = InstrumentTypeId::kShapingSynth; const int kAbsentEngine = 0x7E; const char* const kAbsentName = "absent";
#else
      const InstrumentTypeId kAbsent = InstrumentTypeId::kDrumSynth; const int kAbsentEngine = 6 /* ot::kSynthEnginePerc */; const char* const kAbsentName = "PERC";
#endif
      auto src = ps->createPatch(kLead); open::PatchData pd; static_cast<open::OpenPatch&>(*src).exportData(pd);
      pd.engine = kAbsentEngine; uint8_t pbuf[4096]; const size_t pn = open::encodePatch(pd, pbuf, sizeof(pbuf));
      bool refused = pn > 0, untouched = true, decoded = pn > 0;
      for (const auto& kd : kinds) {
        const InstrumentTypeId k = kd.id;
        auto t = ps->createPatch(int(k)); const float b0 = *t->getParamValue(0);
        const char* nm = open::engineDefForType(int(k))->name;
        mp->files[std::string(nm) + "/absent.patch"].assign(pbuf, pbuf + pn);
        refused = refused && !ps->loadPatch("absent", *t, true); untouched = untouched && *t->getParamValue(0) == b0;
      }
      open::PatchData back; decoded = decoded && open::decodePatch(pbuf, pn, back) && open::typeForEngineId(back.engine) == -1;
      printf("  absent engine: %s (.patch engine %d)\n", kAbsentName, kAbsentEngine);
      check(decoded && refused && untouched, "a .patch of an engine this build lacks is refused by every engine and leaves the patch untouched");
      auto pp = ps->createPatch(int(kAbsent));   // stale type id from a stock project
      check(pp && pp->getMetadata().instrumentTypeId == kLead && int(pp->getMetadata().params.size()) == open::engineDefForType(kLead)->paramCount,
            "createPatch(absent type) falls back to an Init patch of the first engine");
      check(ps->listPatchesByType(int(kAbsent)).empty() && s4->getInstrumentName(int(kAbsent)).empty(),
            "an absent type lists no patches and has no instrument name");
      check(!ps->loadPatch("absent", *pp, true) && !ws->loadPatch("0", *pp, true), "loading into the fallback patch from an absent engine or missing files fails cleanly");
    }
    // workspace store: flat N.patch, as Synth::savePatchToWorkspace / loadPatchFromWorkspace use it
    auto w0 = ws->createPatch(kLead); const float w0v = float(w0->getMetadata().params[0].maxValue); w0->setParamValue(0, w0v, Patch::ParamChangeSource::kKnob);
    check(ws->savePatch("0", *w0) && mw->files.count("0.patch") == 1, "workspace store writes Workspace/0.patch (no type folder)");
    auto w1 = ws->createPatch(kLead);
    check(ws->loadPatch("0", *w1, true) && *w1->getParamValue(0) == w0v, "project reload: the edited value comes back from Workspace/0.patch");
    // ---- patch change FX: a bank of the first patches of the engine, applied as a plain copy of the values
    {
      auto sB = Synth::createSynth([](size_t){return nullptr;});
      auto fsB = std::make_unique<MemFS>(); MemFS* mb = fsB.get();
      sB->addFileSystem(Synth::FileSystemId::kPatches, std::move(fsB));
      auto pb = sB->createPatchStore(Synth::FileSystemId::kPatches, true);
      auto sE = Synth::createSynth([](size_t){return nullptr;});   // a library with no patch at all
      sE->addFileSystem(Synth::FileSystemId::kPatches, std::make_unique<MemFS>());
      auto pe = sE->createPatchStore(Synth::FileSystemId::kPatches, true);
      auto sameValues = [](Patch& a, Patch& b) { const size_t n = a.getMetadata().params.size(); bool same = n == b.getMetadata().params.size();
        for (size_t i = 0; i < n && same; i++) same = *a.getParamValue(int(i)) == *b.getParamValue(int(i)); return same; };
      bool switches = true, keepsMacros = true, ignoresBad = true, ignoresForeign = true, emptyIgnored = true, changed = true;
      int part = 0;
      for (const auto& k : kinds) {
        auto lo = pb->createPatch(int(k.id)), hi = pb->createPatch(int(k.id)); const auto& m = lo->getMetadata();
        for (size_t i = 0; i < m.params.size(); i++) { const float a = float(m.params[i].minValue), b = float(m.params[i].maxValue);
          lo->setParamValue(int(i), a + (b - a) * 0.1f, Patch::ParamChangeSource::kKnob); hi->setParamValue(int(i), a + (b - a) * 0.9f, Patch::ParamChangeSource::kKnob); }
        pb->savePatch("b_low", *lo); pb->savePatch("b_high", *hi);   // alphabetical: slot 0 = b_high, slot 1 = b_low
        auto rHi = pb->createPatch(int(k.id)), rLo = pb->createPatch(int(k.id));   // what the patch browser loads
        pb->loadPatch("b_high", *rHi, true); pb->loadPatch("b_low", *rLo, true);
        auto part0 = pb->createPatch(int(k.id)); part0->setMacroDisplayName(0, "Keep");
        const bool differs = !sameValues(*part0, *rHi) && !sameValues(*rHi, *rLo);
        changed = changed && differs;
        const int myPart = part++ % open::kPatchBankParts;
        open::patchBankPreload(myPart, *part0, *pb);
        switches = switches && open::patchBankApply(myPart, 0, part0.get()) && sameValues(*part0, *rHi);
        switches = switches && open::patchBankApply(myPart, 1, part0.get()) && sameValues(*part0, *rLo);
        keepsMacros = keepsMacros && part0->getMetadata().macros[0].displayName == "Keep";
        for (int bad : {2, 15, 16, 99, -1}) ignoresBad = ignoresBad && !open::patchBankApply(myPart, bad, part0.get()) && sameValues(*part0, *rLo);
        if (nKinds > 1) {
          const auto& other = kinds[(&k == &kinds[0]) ? 1 : 0];
          auto foreign = pb->createPatch(int(other.id));
          ignoresForeign = ignoresForeign && !open::patchBankApply(myPart, 0, foreign.get()) && !open::patchBankApply(myPart, 1, foreign.get());
        }
        auto bare = pe->createPatch(int(k.id)); open::patchBankPreload(myPart, *bare, *pe);   // the part now has no library
        emptyIgnored = emptyIgnored && !open::patchBankApply(myPart, 0, bare.get());
        mb->files.erase(std::string(k.name) + "/b_low.patch"); mb->files.erase(std::string(k.name) + "/b_high.patch");
      }
      check(changed, "bank test patches differ from each other and from the Init patch (all engines of this build)");
      check(switches, "patch change FX: slot 0 and slot 1 switch the values to the 2nd and 1st alphabetical patch (all engines)");
      check(keepsMacros, "patch change FX leaves the macros of the part alone");
      check(ignoresBad, "patch change FX: empty or out of range slots (2, 15, 16, 99, -1) are ignored, values stay");
      check(ignoresForeign, "patch change FX: a part of another engine than the bank is ignored");
      check(emptyIgnored, "patch change FX: a part with no patches in its library ignores every slot");
    }
#ifdef OT_ORACLE
    // OpenTracker's own codec as oracle: its Init patches load here and our files decode and re-encode there byte for byte
    bool oraOk = true; int engIds[4] = { ot::kSynthEngineAcd, ot::kSynthEngineFat, ot::kSynthEngineVap, ot::kSynthEngineWtfm };
    for (int e = 0; e < 4; e++) {
      ot::SynthPatch sp; ot::patchInit(engIds[e], &sp); std::vector<uint8_t> ref(8192); size_t n = 0; ot::encodePatch(sp, ref.data(), ref.size(), &n); ref.resize(n);
      mp->files[std::string(kinds[e].name) + "/ref.patch"] = ref;
      auto p = ps->createPatch(int(kinds[e].id)); const bool l = ps->loadPatch("ref", *p, true);
      ps->savePatch("ref2", *p); const auto& mine = mp->files[std::string(kinds[e].name) + "/ref2.patch"];
      ot::SynthPatch back; const bool dec = ot::decodePatch(mine.data(), mine.size(), &back) == ot::PatchStatus::kOk;
      std::vector<uint8_t> re(8192); size_t rn = 0; if (dec) ot::encodePatch(back, re.data(), re.size(), &rn); re.resize(rn);
      printf("  oracle %-5s ref %zu bytes, ours %zu bytes: %s, OT re-encodes ours %s\n", kinds[e].name, ref.size(), mine.size(), mine == ref ? "identical" : "DIFFERENT", re == mine ? "identically" : "DIFFERENTLY");
      oraOk = oraOk && l && dec && re == mine && mine == ref;
    }
    check(oraOk, "OpenTracker oracle: its Init patches round-trip through ours byte for byte");
#endif
  }
#ifdef SYNTH_ENGINE_ACD
  // ---- pitch bend (MIDI kPitchBendChange, also what the Micro-tune FX sends): +-1 semitone at +-8191/-8192
  {
    auto s5 = Synth::createSynth([](size_t){return nullptr;});
    auto st5 = s5->createPatchStore(Synth::FileSystemId::kPatches, true);
    auto p5 = std::shared_ptr<Patch>(st5->createPatch(int(InstrumentTypeId::kSimpleAnalogSynth)));
    for (auto [id, v] : { std::pair{17, 1.0f}, {14, 0.0f}, {9, 20000.0f} }) p5->setParamValue(id, v, Patch::ParamChangeSource::kKnob);   // steady: sustain 1, no filter env
    Src src5; auto i5 = s5->createInstrument(0); i5->assignPatch(p5); i5->connect(src5); i5->setVoiceCount(1); i5->setMidiChannel(0);
    std::shared_ptr<Instrument> a5[1] = { i5 };
    s5->setup(std::span<std::shared_ptr<Instrument>>(a5,1), {}, nullptr, []{ return tiny::optional<float>(120.0f); });
    auto bend = [&](int v) { MidiEvent ev{}; ev.type = MidiEvent::Type::kPitchBendChange; ev.channel = 0; ev.data.pitchBendChange.value = int16_t(v); (*src5.cb.lock())(ev, 0); };
    auto crossings = [&](int blocks) { int zc = 0; float prev = 0; for (int b = 0; b < blocks; b++) { s5->renderAudioBuffer({AudioBufferRef(L),AudioBufferRef(R)}, std::span<AudioBufferRef>(aux), 0); for (float v : L) { if (v > 0 && prev <= 0) zc++; prev = v; } } return zc; };
    src5.send(MidiEvent::Type::kNoteOn, 0, 57, 100); crossings(10);
    const int base = crossings(200);
    bend(8191); crossings(2); const int up = crossings(200);
    bend(-8192); crossings(2); const int down = crossings(200);
    bend(0); crossings(2); const int back = crossings(200);
    printf("  pitch bend: rising crossings per 200 blocks base=%d up=%d down=%d back=%d (expect x1.059 and x0.944)\n", base, up, down, back);
    check(std::fabs(float(up) / base - 1.0595f) < 0.02f, "bend +8191 raises a held note by about one semitone");
    check(std::fabs(float(down) / base - 0.9439f) < 0.02f, "bend -8192 lowers it by about one semitone");
    check(std::abs(back - base) <= 2, "bend 0 returns to the note pitch");
    bend(8191); src5.send(MidiEvent::Type::kStop, 0, 0, 0); src5.send(MidiEvent::Type::kNoteOn, 0, 57, 100); crossings(10);
    check(std::abs(crossings(200) - base) <= 2, "transport Stop resets the bend");
  }
#endif
#ifdef SYNTH_ENGINE_CHIP
  // ---- CHIP: pulse model, steady tone, bend and glide. The hosted voice must follow the shim's bend and report silence
  {
    auto s6 = Synth::createSynth([](size_t){return nullptr;});
    auto st6 = s6->createPatchStore(Synth::FileSystemId::kPatches, true);
    auto p6 = std::shared_ptr<Patch>(st6->createPatch(int(InstrumentTypeId::kChiptuneSynth)));
    // Model 2A03 pulse, no PWM, no sub, open filter, no crush, sustain 1
    for (auto [id, v] : { std::pair{0, 0.0f}, {5, 0.0f}, {6, 0.0f}, {17, 18000.0f}, {19, 0.0f}, {20, 16.0f}, {15, 1.0f} })
      p6->setParamValue(id, v, Patch::ParamChangeSource::kKnob);
    Src src6; auto i6 = s6->createInstrument(0); i6->assignPatch(p6); i6->connect(src6); i6->setVoiceCount(1); i6->setMidiChannel(0);
    std::shared_ptr<Instrument> a6[1] = { i6 };
    s6->setup(std::span<std::shared_ptr<Instrument>>(a6,1), {}, nullptr, []{ return tiny::optional<float>(120.0f); });
    auto bend = [&](int v) { MidiEvent ev{}; ev.type = MidiEvent::Type::kPitchBendChange; ev.channel = 0; ev.data.pitchBendChange.value = int16_t(v); (*src6.cb.lock())(ev, 0); };
    auto crossings = [&](int blocks) { int zc = 0; float prev = 0; for (int b = 0; b < blocks; b++) { s6->renderAudioBuffer({AudioBufferRef(L),AudioBufferRef(R)}, std::span<AudioBufferRef>(aux), 0); for (float v : L) { if (v > 0 && prev <= 0) zc++; prev = v; } } return zc; };
    src6.send(MidiEvent::Type::kNoteOn, 0, 57, 100); crossings(10);
    const int base = crossings(200);
    bend(8191); crossings(2); const int up = crossings(200);
    bend(0); crossings(2); const int back = crossings(200);
    printf("  CHIP pulse: rising crossings per 200 blocks base=%d up=%d back=%d (expect x1.059)\n", base, up, back);
    check(base > 100, "CHIP pulse at A3 sounds with the expected cycle count");
    check(std::fabs(float(up) / base - 1.0595f) < 0.03f, "CHIP follows the shim's pitch bend (+1 semitone)");
    check(std::abs(back - base) <= 2, "CHIP bend 0 returns to the note pitch");
    src6.send(MidiEvent::Type::kNoteOff, 0, 57, 0);
    float rel = 1; for (int b = 0; b < 400; b++) { s6->renderAudioBuffer({AudioBufferRef(L),AudioBufferRef(R)}, std::span<AudioBufferRef>(aux), 0); rel = peakOf(L); }
    check(rel < 1e-4f, "CHIP releases to silence after note off");
    // two voices on one slot, then steal: each voice object plays its own note
    i6->setVoiceCount(2);
    src6.send(MidiEvent::Type::kNoteOn, 0, 57, 100); src6.send(MidiEvent::Type::kNoteOn, 0, 64, 100); src6.send(MidiEvent::Type::kNoteOn, 0, 69, 100);
    float pk2 = 0; for (int b = 0; b < 40; b++) { s6->renderAudioBuffer({AudioBufferRef(L),AudioBufferRef(R)}, std::span<AudioBufferRef>(aux), 0); pk2 = std::max(pk2, peakOf(L)); }
    check(std::isfinite(pk2) && pk2 > 1e-3f, "CHIP: three notes on a 2 voice slot (one steal) stay finite and audible");
  }
#endif
#ifdef SYNTH_ENGINE_JU6
  // ---- JU6: the chorus (slotPost) runs on the slot's own bus: stereo when on, mono when off, with a tail after note off
  {
    auto s7 = Synth::createSynth([](size_t){return nullptr;});
    auto st7 = s7->createPatchStore(Synth::FileSystemId::kPatches, true);
    Src src7[2]; std::shared_ptr<Patch> p7[2]; std::shared_ptr<Instrument> i7[2];
    for (int k = 0; k < 2; k++) {   // slot 0 = JU6 with chorus II, slot 1 = JU6 chorus off: no bleed between slots
      p7[k] = std::shared_ptr<Patch>(st7->createPatch(0x81));
      p7[k]->setParamValue(22, k == 0 ? 2.0f : 0.0f, Patch::ParamChangeSource::kKnob);
      i7[k] = s7->createInstrument(k); i7[k]->assignPatch(p7[k]); i7[k]->connect(src7[k]); i7[k]->setVoiceCount(1); i7[k]->setMidiChannel(0);
    }
    std::shared_ptr<Instrument> a7[2] = { i7[0], i7[1] };
    s7->setup(std::span<std::shared_ptr<Instrument>>(a7,2), {}, nullptr, []{ return tiny::optional<float>(120.0f); });
    auto diff = [&](int slot, int blocks, float& pk) { double d = 0; pk = 0; for (int b = 0; b < blocks; b++) { s7->renderAudioBuffer({AudioBufferRef(L),AudioBufferRef(R)}, std::span<AudioBufferRef>(aux), slot); /* mono mix in this call */ } return d; };
    (void)diff;
    // render both slots through the main (summing) path to look at L and R separately
    auto mainPath = [&](int blocks, double& dLR, float& pk) { dLR = 0; pk = 0; for (int b = 0; b < blocks; b++) { s7->renderAudioBuffer({AudioBufferRef(L),AudioBufferRef(R)}); for (int i = 0; i < 128; i++) { dLR += std::fabs(L[i] - R[i]); pk = std::max(pk, std::max(std::fabs(L[i]), std::fabs(R[i]))); } } };
    double dLR; float pk;
    src7[1].send(MidiEvent::Type::kNoteOn, 0, 57, 100);   // chorus off only
    mainPath(120, dLR, pk);
    check(pk > 1e-3f && dLR < 1e-6, "JU6 chorus off: sounds, L == R");
    src7[0].send(MidiEvent::Type::kNoteOn, 0, 57, 100);   // chorus II too
    mainPath(120, dLR, pk);
    check(std::isfinite(pk) && dLR > 1e-3, "JU6 chorus II on slot 0: the mix is stereo");
    src7[1].send(MidiEvent::Type::kNoteOff, 0, 57, 0); src7[0].send(MidiEvent::Type::kNoteOff, 0, 57, 0);
    mainPath(40, dLR, pk);
    float tailPk = 0; for (int b = 0; b < 3; b++) { mainPath(1, dLR, pk); tailPk = std::max(tailPk, pk); }
    mainPath(1200, dLR, pk);   // pk is the maximum over these blocks, so look at a few more afterwards
    mainPath(10, dLR, pk);
    printf("  JU6 after release: pk=%g\n", pk);
    for (int k = 0; k < 2; k++) { s7->renderAudioBuffer({AudioBufferRef(L),AudioBufferRef(R)}, std::span<AudioBufferRef>(aux), k); printf("    slot %d alone: pk=%g\n", k, peakOf(L)); }
    check(std::isfinite(pk) && pk < 1e-4f, "JU6 released: voices and chorus tail end in silence");
  }
#endif
#ifdef SYNTH_ENGINE_PHZ
  // ---- PHZ: every model x character is finite and sounds, X and Y of the phase distortion change the spectrum, the
  // filter Off path and a filtered type both stay finite, Reset restarts the cycle
  {
    using namespace ot::audio::phz;
    auto s8 = Synth::createSynth([](size_t){return nullptr;});
    auto st8 = s8->createPatchStore(Synth::FileSystemId::kPatches, true);
    auto p8 = std::shared_ptr<Patch>(st8->createPatch(0x82));
    Src src8; auto i8 = s8->createInstrument(0); i8->assignPatch(p8); i8->connect(src8); i8->setVoiceCount(1); i8->setMidiChannel(0);
    std::shared_ptr<Instrument> a8[1] = { i8 };
    s8->setup(std::span<std::shared_ptr<Instrument>>(a8,1), {}, nullptr, []{ return tiny::optional<float>(120.0f); });
    auto blk8 = [&]() { s8->renderAudioBuffer({AudioBufferRef(L),AudioBufferRef(R)}, std::span<AudioBufferRef>(aux), 0); return peakOf(L); };
    auto set = [&](int id, float v) { p8->setParamValue(id, v, Patch::ParamChangeSource::kKnob); };
    // steady, dry, one oscillator, filter Off
    set(kMix, 0.0f); set(kDetune, 0.0f); set(kAmpA, 0.0f); set(kAmpS, 1.0f); set(kCutoff, 20000.0f); set(kFilterEnvAmt, 0.0f);
    constexpr int kBlocks = 64;   // 8192 samples
    const double f0 = 220.0, fs = 44117.647;
    // Spectrum of the held note: harmonics 1..24 (Goertzel, Hann window). Returns false when not finite
    auto spectrum = [&](double* mag, float& peak) {
      src8.send(MidiEvent::Type::kNoteOff, 0, 57, 0); for (int b = 0; b < 300; b++) blk8();
      src8.send(MidiEvent::Type::kNoteOn, 0, 57, 100); for (int b = 0; b < 12; b++) blk8();
      static float x[kBlocks * 128]; peak = 0;
      for (int b = 0; b < kBlocks; b++) { blk8(); for (int i = 0; i < 128; i++) { x[b * 128 + i] = L[i]; peak = std::max(peak, std::fabs(L[i])); if (!std::isfinite(L[i])) peak = INFINITY; } }
      const int n = kBlocks * 128;
      for (int h = 1; h <= 24; h++) {
        double re = 0, im = 0; const double w = 2 * M_PI * h * f0 / fs;
        for (int i = 0; i < n; i++) { const double hann = 0.5 - 0.5 * std::cos(2 * M_PI * i / n); re += hann * x[i] * std::cos(w * i); im -= hann * x[i] * std::sin(w * i); }
        mag[h - 1] = std::sqrt(re * re + im * im) / n * 4;
      }
    };
    auto brightness = [](const double* m) { double hi = 0, all = 1e-12; for (int h = 0; h < 24; h++) { all += m[h] * m[h]; if (h >= 1) hi += m[h] * m[h]; } return hi / all; };
    auto dist = [](const double* a, const double* b) { double d = 0; for (int h = 0; h < 24; h++) d += (a[h] - b[h]) * (a[h] - b[h]); return std::sqrt(d); };
    double ref[24], m[24]; float pk;
    bool allOk = true;
    for (int model = 0; model <= 5; model++) {
      for (int ch : { 0, 1, 2, 3, 6, 8 }) {
        set(kModel, float(model)); set(kChar1, float(ch)); set(kX1, 0.7f); set(kY1, 0.4f); set(kShape1, 0.6f);
        spectrum(m, pk);
        const bool ok = std::isfinite(pk) && pk > 0.05f && pk < 1.5f;
        if (!ok) { printf("  PHZ model %d character %d: peak %g FAIL\n", model, ch, pk); allOk = false; }
      }
    }
    check(allOk, "PHZ: all 6 models x 6 characters finite, non-silent, peak below 1.5");
    // X changes the spectrum: OG Saw from sine (X 0) to saw (X 1)
    set(kModel, 0.0f); set(kChar1, 0.0f); set(kShape1, 0.0f); set(kY1, 0.5f);
    set(kX1, 0.0f); spectrum(ref, pk); const double bSine = brightness(ref);
    set(kX1, 0.9f); spectrum(m, pk); const double bSaw = brightness(m);
    printf("  PHZ OG Saw: brightness X=0 %.4f, X=0.9 %.4f, spectrum distance %.3f\n", bSine, bSaw, dist(ref, m));
    check(bSine < 0.01 && bSaw > 0.15 && dist(ref, m) > 0.1, "PHZ: X moves OG Saw from a sine to a bright saw");
    // Y changes the spectrum for the models it is meant for
    for (int model : { 0, 4 }) {
      set(kModel, float(model)); set(kX1, 0.35f);
      set(kY1, 0.5f); spectrum(ref, pk);
      set(kY1, 0.9f); spectrum(m, pk);
      printf("  PHZ model %d: Y 0.5 -> 0.9 spectrum distance %.3f\n", model, dist(ref, m));
      check(dist(ref, m) > 0.03, model == 0 ? "PHZ OG Saw: Y changes the spectrum" : "PHZ Linear: Y changes the spectrum");
    }
    // Shape moves along the Character row
    set(kModel, 0.0f); set(kX1, 0.5f); set(kY1, 0.5f); set(kChar1, 6.0f);
    set(kShape1, 0.0f); spectrum(ref, pk); set(kShape1, 1.0f); spectrum(m, pk);
    check(dist(ref, m) > 0.05, "PHZ: Shape changes the spectrum (Add 5)");
    // Filtered type and Reset: finite, quieter than the unfiltered note, and two notes with Reset On are identical
    set(kShape1, 0.0f); set(kChar1, 0.0f); set(kX1, 0.8f); set(kCutoff, 800.0f); set(kResonance, 0.6f); set(kFilterType, 1.0f);
    spectrum(m, pk); check(std::isfinite(pk) && pk > 0.01f, "PHZ: LP OB 24 dB at 800 Hz sounds and is finite");
    set(kCutoff, 20000.0f); set(kReset1, 1.0f); set(kAmpD, 0.0f);
    spectrum(ref, pk); spectrum(m, pk);
    check(dist(ref, m) < 1e-3, "PHZ: Reset On starts every note on the same phase");
    // Model and Character names reach the editor as text
    const auto& md8 = p8->getMetadata();
    check(md8.params[kModel].valueFormatter(*p8, 5.0f) == "Hermite FM" && md8.params[kChar2].valueFormatter(*p8, 8.0f) == "Add 11" && md8.params[kX1].valueFormatter(*p8, 0.25f) == "0.25",
          "PHZ: value text for Model, Character and XMod");
    src8.send(MidiEvent::Type::kNoteOff, 0, 57, 0);
  }
#endif
  // ---- ids: the canonical stored numbers and the .patch engine numbers of the picked engines are unique and map back
  {
    struct Row { const char* name; int stored; int type; int patchEngine; } rows[] = {
#define SYNTH_ENGINE(NAME, STORED, TYPE_ID, FACTORY, DEF, PATCH_ENGINE, ...) {#NAME, STORED, int(TYPE_ID), PATCH_ENGINE},
#include "engine_list.h"
#undef SYNTH_ENGINE
    };
    bool uniq = true, back = true;
    for (size_t a = 0; a < sizeof(rows) / sizeof(rows[0]); a++) {
      back = back && open::engineIdForType(rows[a].type) == rows[a].patchEngine && open::typeForEngineId(rows[a].patchEngine) == rows[a].type;
      for (size_t b = a + 1; b < sizeof(rows) / sizeof(rows[0]); b++)
        uniq = uniq && rows[a].stored != rows[b].stored && rows[a].type != rows[b].type && rows[a].patchEngine != rows[b].patchEngine;
      printf("  %-5s stored %d, type id %d, .patch engine %d\n", rows[a].name, rows[a].stored, rows[a].type, rows[a].patchEngine);
    }
    check(uniq, "stored numbers, type ids and .patch engine numbers are unique");
    check(back, ".patch engine number <-> type id round trip for every picked engine");
  }
#ifdef SYNTH_ENGINE_PERC
  // ---- PERC: one hit per drum sounds and is finite, the kit overlaps inside one voice, velocity scales the hit, the
  // unmapped notes (61 63 68 70) are silent, everything dies out
  {
    auto s9 = Synth::createSynth([](size_t){return nullptr;});
    auto st9 = s9->createPatchStore(Synth::FileSystemId::kPatches, true);
    auto p9 = std::shared_ptr<Patch>(st9->createPatch(int(InstrumentTypeId::kDrumSynth)));
    Src src9; auto i9 = s9->createInstrument(0); i9->assignPatch(p9); i9->connect(src9); i9->setVoiceCount(2); i9->setMidiChannel(0);
    std::shared_ptr<Instrument> a9[1] = { i9 };
    s9->setup(std::span<std::shared_ptr<Instrument>>(a9,1), {}, nullptr, []{ return tiny::optional<float>(120.0f); });
    auto blk9 = [&]() { s9->renderAudioBuffer({AudioBufferRef(L),AudioBufferRef(R)}, std::span<AudioBufferRef>(aux), 0); return peakOf(L); };
    check(i9->getVoiceCount() == 2 && p9->getMetadata().maxVoices == 1, "PERC: the patch metadata caps it at 1 voice");
    // Hit one note, return peak and energy of the next n blocks; wait until the kit is silent first
    auto hit = [&](int note, int vel, int n, double& rms) {
      src9.send(MidiEvent::Type::kStop, 0, 0, 0); for (int b = 0; b < 1500; b++) blk9();   // the drums of an earlier hit ring on after Stop
      src9.send(MidiEvent::Type::kNoteOn, 0, note, vel);
      float pk = 0; double en = 0; for (int b = 0; b < n; b++) { pk = std::max(pk, blk9()); for (int i = 0; i < 128; i++) en += double(L[i]) * L[i]; }
      rms = std::sqrt(en / (n * 128.0)); return pk;
    };
    const struct { const char* name; int note; } drums[] = { {"kick", 60}, {"snare", 62}, {"closed hat", 64}, {"open hat", 67}, {"cymbal", 69}, {"perc", 71}, {"tom", 48}, {"tom high", 84} };
    bool all = true;
    for (const auto& d : drums) {
      double r; const float pk = hit(d.note, 100, 40, r);
      printf("  PERC %-10s note %3d peak=%.4f rms=%.4f\n", d.name, d.note, pk, r);
      all = all && std::isfinite(pk) && pk > 1e-3f && pk < 4.0f;
    }
    check(all, "PERC: kick, snare, both hats, cymbal, perc and two toms sound, finite and below 4.0");
    bool silent = true; for (int note : {61, 63, 68, 70}) { double r; const float pk = hit(note, 100, 20, r); silent = silent && pk < 1e-4f; }
    check(silent, "PERC: notes 61 63 68 70 play nothing");
    double rHi, rLo; hit(60, 127, 20, rHi); hit(60, 40, 20, rLo);
    printf("  PERC kick rms vel 127 %.4f, vel 40 %.4f\n", rHi, rLo);
    check(rLo < 0.6 * rHi && rLo > 0.1 * rHi, "PERC: velocity scales the hit (vel 40 is clearly quieter than 127)");
    // overlap: kick, then snare 20 blocks later: the kick still rings under the snare (peak above the snare alone)
    double r; const float snareOnly = hit(62, 100, 20, r);
    src9.send(MidiEvent::Type::kStop, 0, 0, 0); for (int b = 0; b < 1500; b++) blk9();
    src9.send(MidiEvent::Type::kNoteOn, 0, 60, 100); for (int b = 0; b < 20; b++) blk9();
    src9.send(MidiEvent::Type::kNoteOn, 0, 62, 100); float both = 0; for (int b = 0; b < 20; b++) both = std::max(both, blk9());
    printf("  PERC snare alone %.4f, kick then snare %.4f\n", snareOnly, both);
    check(std::isfinite(both) && both > snareOnly * 0.9f, "PERC: a second drum does not cut the first (one voice, drums overlap)");
    // all notes at once and then release: finite, and the kit ends in silence
    for (int note : {60, 62, 64, 69, 71, 48}) src9.send(MidiEvent::Type::kNoteOn, 0, note, 127);
    float pkAll = 0; for (int b = 0; b < 100; b++) pkAll = std::max(pkAll, blk9());
    for (int note : {60, 62, 64, 69, 71, 48}) src9.send(MidiEvent::Type::kNoteOff, 0, note, 0);
    float tail = 0; for (int b = 0; b < 1800; b++) blk9(); for (int b = 0; b < 4; b++) tail = std::max(tail, blk9());
    printf("  PERC six drums at once peak=%.4f, after release+5 s peak=%.6f\n", pkAll, tail);
    check(std::isfinite(pkAll) && pkAll > 1e-3f && tail < 1e-4f, "PERC: six drums together stay finite and the kit dies out");
  }
#endif

  // ---- every picked engine: ids, parameter sweep, velocity, polyphony (and the T-Type note map)
  {
    struct Id { const char* name; int stored, typeId, patchId; } ids[] = {
#define SYNTH_ENGINE(NAME, STORED, TYPE_ID, FACTORY, DEF, PATCH_ENGINE, ...) {#NAME, STORED, int(TYPE_ID), PATCH_ENGINE},
#include "engine_list.h"
#undef SYNTH_ENGINE
    };
    const int nIds = int(sizeof(ids) / sizeof(ids[0]));
    bool uniq = true;
    for (int i = 0; i < nIds; i++) for (int j = i + 1; j < nIds; j++)
      if (ids[i].stored == ids[j].stored || ids[i].typeId == ids[j].typeId || ids[i].patchId == ids[j].patchId || !strcmp(ids[i].name, ids[j].name)) {
        printf("  clash: %s / %s\n", ids[i].name, ids[j].name); uniq = false; }
    check(uniq, "engine names, stored numbers, type ids and .patch ids are unique");
    // the numbers the poly-editor stores (patches saved by the editor load on the device): pinned
    static const struct { const char* name; int stored, patchId; } kPin[] = {
      {"ACD",0,1},{"FAT",1,2},{"VAP",2,4},{"WTFM",3,5},{"PERC",4,6},{"CHIP",6,8},{"JU6",7,0x81},{"PHZ",8,0x82},{"FM4",9,0x80},{"DTYPE",10,0xA2},
      {"CSTYPE",11,0x83},{"ATYPE",12,0x84},{"TTYPE",13,0x85},{"8TYPE",14,0x86},{"HANDTYPE",15,0x87},{"BTYPE",16,0x88},{"ARCTYPE",17,0x89} };
    bool pinned = true, roundTrip = true;
    for (const auto& id : ids) {
      bool found = false;
      for (const auto& pin : kPin) if (!strcmp(pin.name, id.name)) { found = true; pinned = pinned && pin.stored == id.stored && pin.patchId == id.patchId && (id.patchId < 0x80 || id.patchId == id.typeId); }
      pinned = pinned && found;
      roundTrip = roundTrip && open::engineIdForType(id.typeId) == id.patchId && open::typeForEngineId(id.patchId) == id.typeId;
    }
    check(pinned, "stored numbers, .patch ids and OpenTracker type ids match the poly-editor table (D-Type 0xA2, PHZ 0x82)");
    check(roundTrip, ".patch engine id <-> type id round trips for every engine");

    // One note on a fresh synth with the default patch, one parameter changed. out = 40 blocks of L then R.
    auto renderCase = [&](int typeId, int note, int vel, int pidx, float pval, std::vector<float>& out) {
      auto sx = Synth::createSynth([](size_t){return nullptr;});
      auto stx = sx->createPatchStore(Synth::FileSystemId::kPatches, true);
      auto px = std::shared_ptr<Patch>(stx->createPatch(typeId));
      if (pidx >= 0) px->setParamValue(pidx, pval, Patch::ParamChangeSource::kKnob);
      Src sr; auto ix = sx->createInstrument(0); ix->assignPatch(px); ix->connect(sr); ix->setVoiceCount(2); ix->setMidiChannel(0);
      std::shared_ptr<Instrument> ax[1] = { ix };
      sx->setup(std::span<std::shared_ptr<Instrument>>(ax,1), {}, nullptr, []{ return tiny::optional<float>(120.0f); });
      std::array<float,128> l, r, a1, a2; std::array<AudioBufferRef,2> au{AudioBufferRef(a1),AudioBufferRef(a2)};
      sr.send(MidiEvent::Type::kNoteOn, 0, note, vel);
      out.clear(); float pk = 0;
      for (int b = 0; b < 40; b++) { sx->renderAudioBuffer({AudioBufferRef(l),AudioBufferRef(r)}, std::span<AudioBufferRef>(au), 0); pk = std::max(pk, peakOf(l)); out.insert(out.end(), l.begin(), l.end()); out.insert(out.end(), r.begin(), r.end()); }
      return pk;
    };
    auto energy = [](const std::vector<float>& v) { double e = 0; for (float x : v) e += double(x) * x; return std::sqrt(e / double(v.size())); };
    for (const auto& id : ids) {
      const open::EngineDef& d = *open::engineDefForType(id.typeId);
      const int note = (id.typeId == int(InstrumentTypeId::kDrumSynth) || id.typeId == 0x85) ? 60 : 57;
      std::vector<float> base, alt;
      const float pkBase = renderCase(id.typeId, note, 100, -1, 0, base);
      // a kit engine (PERC, T-Type) has one parameter group per drum: the sweep plays one note per drum there
      std::vector<int> sweepNotes = { note };
      if (d.maxVoices == 1) sweepNotes = { 60, 61, 62, 63, 64, 66, 68, 69, 70, 48, 52, 56 };
      std::vector<std::vector<float>> bases(sweepNotes.size());
      for (size_t k = 0; k < sweepNotes.size(); k++) renderCase(id.typeId, sweepNotes[k], 100, -1, 0, bases[k]);
      int changed = 0; std::string still;
      for (int i = 0; i < d.paramCount; i++) {
        const open::ParamDef& pd = d.params[i];
        const float v = pd.def < pd.max ? pd.max : pd.min;
        bool diff = false;
        for (size_t k = 0; k < sweepNotes.size() && !diff; k++) {
          const float pk = renderCase(id.typeId, sweepNotes[k], 100, i, v, alt);
          if (!std::isfinite(pk)) { printf("  %s param %d (%s) = %g: NOT FINITE\n", id.name, i, pd.name, v); bad++; diff = true; }
          for (size_t q = 0; q < alt.size() && !diff; q++) diff = std::fabs(bases[k][q] - alt[q]) > 1e-6f;
        }
        changed += diff; if (!diff) still += (still.empty() ? "" : ",") + std::to_string(i);
      }
      std::vector<float> lo, hi; const float pkLo = renderCase(id.typeId, note, 32, -1, 0, lo); const float pkHi = renderCase(id.typeId, note, 127, -1, 0, hi);
      const double eLo = energy(lo), eHi = energy(hi);
      // polyphony: 4 voice instrument: 2 notes -> min(2, maxVoices) voices, 4 notes -> min(4, maxVoices), a fifth steals
      auto sp = Synth::createSynth([](size_t){return nullptr;});
      auto stp = sp->createPatchStore(Synth::FileSystemId::kPatches, true);
      auto pp = std::shared_ptr<Patch>(stp->createPatch(id.typeId));
      Src spr; auto ip = sp->createInstrument(0); ip->assignPatch(pp); ip->connect(spr); ip->setVoiceCount(4); ip->setMidiChannel(0);
      std::shared_ptr<Instrument> ap[1] = { ip };
      sp->setup(std::span<std::shared_ptr<Instrument>>(ap,1), {}, nullptr, []{ return tiny::optional<float>(120.0f); });
      std::array<float,128> l, r, a1, a2; std::array<AudioBufferRef,2> au{AudioBufferRef(a1),AudioBufferRef(a2)};
      auto blkp = [&]() { sp->renderAudioBuffer({AudioBufferRef(l),AudioBufferRef(r)}, std::span<AudioBufferRef>(au), 0); return peakOf(l); };
      const int n1 = note == 60 ? 60 : 57, n2 = note == 60 ? 62 : 64, n3 = note == 60 ? 64 : 72;
      spr.send(MidiEvent::Type::kNoteOn, 0, n1, 100); for (int b = 0; b < 4; b++) blkp();
      spr.send(MidiEvent::Type::kNoteOn, 0, n2, 100); for (int b = 0; b < 4; b++) blkp();
      const int v2 = open::voicesSounding();
      spr.send(MidiEvent::Type::kNoteOn, 0, n3, 100); for (int b = 0; b < 4; b++) blkp();
      spr.send(MidiEvent::Type::kNoteOn, 0, n3 + 3, 100); for (int b = 0; b < 4; b++) blkp();
      const int v4 = open::voicesSounding();
      spr.send(MidiEvent::Type::kNoteOn, 0, n3 + 7, 100); float pkP = 0; for (int b = 0; b < 20; b++) pkP = std::max(pkP, blkp());
      const int v3 = open::voicesSounding();   // after the 5th note
      const int want = d.maxVoices < 4 ? d.maxVoices : 4;
      printf("%-8s sweep: %d of %d params change the output%s%s | velocity 32 -> 127: peak %.3f -> %.3f, rms %.3f -> %.3f | voices after 2 / 4 / 5 notes: %d / %d / %d (max %d)\n",
             id.name, changed, d.paramCount, still.empty() ? "" : ", unchanged: ", still.size() > 150 ? (still.substr(0, 150) + "..").c_str() : still.c_str(), pkLo, pkHi, eLo, eHi, v2, v4, v3, d.maxVoices);
      check(std::isfinite(pkBase) && pkBase > 1e-4f && changed >= 3 && changed * 5 >= d.paramCount, (std::string(id.name) + ": default patch sounds, parameter changes alter the output").c_str());
      check(std::isfinite(pkLo) && std::isfinite(pkHi) && pkLo > 1e-5f && pkHi > 1e-4f, (std::string(id.name) + ": velocity 32 and 127 both sound, finite").c_str());
      check(v2 == (d.maxVoices < 2 ? d.maxVoices : 2) && v4 == want && v3 == want && std::isfinite(pkP) && pkP > 1e-4f, (std::string(id.name) + ": voices follow the cap (4, kit engines 1), a fifth note steals").c_str());
      if (id.typeId == 0x85) {
        // T-Type note map (ttype.cpp drumForNote): 60 kick, 61 rim, 62 snare, 63 and 71 clap, 64..67 hat closed .. open, 68 cowbell,
        // 69 crash, 70 ride, 48..59 the three toms (4 semitones each), below 48 the low tom, 72 and up wraps onto 60..71
        bool all = true; std::vector<float> tmp;
        for (int nn : {36, 47, 48, 52, 56, 59, 60, 61, 62, 63, 64, 65, 66, 67, 68, 69, 70, 71, 72, 84}) {
          const float pk = renderCase(id.typeId, nn, 100, -1, 0, tmp);
          if (!std::isfinite(pk) || pk < 1e-3f) { printf("  T-Type note %d: peak %g FAIL\n", nn, pk); all = false; }
        }
        check(all, "T-Type: every note of the map (36..72, 84) plays a drum, finite");
        renderCase(id.typeId, 62, 100, -1, 0, tmp); const double snareE = energy(tmp);
        std::vector<float> kick; renderCase(id.typeId, 60, 100, -1, 0, kick); const double kickE = energy(kick);
        std::vector<float> hit; renderCase(id.typeId, 60, 40, -1, 0, hit);
        check(kickE > 1e-4 && snareE > 1e-4 && energy(hit) < 0.9 * kickE && energy(hit) > 0.05 * kickE, "T-Type: velocity (Accent) scales a hit, kick and snare differ");
        // kit semantics: kick, then a snare 20 blocks later: the kick still rings under it (one voice, drums overlap)
        auto s9 = Synth::createSynth([](size_t){return nullptr;});
        auto st9 = s9->createPatchStore(Synth::FileSystemId::kPatches, true);
        auto p9 = std::shared_ptr<Patch>(st9->createPatch(id.typeId));
        Src src9; auto i9 = s9->createInstrument(0); i9->assignPatch(p9); i9->connect(src9); i9->setVoiceCount(2); i9->setMidiChannel(0);
        std::shared_ptr<Instrument> a9[1] = { i9 };
        s9->setup(std::span<std::shared_ptr<Instrument>>(a9,1), {}, nullptr, []{ return tiny::optional<float>(120.0f); });
        auto blk9 = [&]() { s9->renderAudioBuffer({AudioBufferRef(L),AudioBufferRef(R)}, std::span<AudioBufferRef>(aux), 0); return peakOf(L); };
        float snareOnly = 0; src9.send(MidiEvent::Type::kNoteOn, 0, 62, 100); for (int b = 0; b < 20; b++) snareOnly = std::max(snareOnly, blk9());
        src9.send(MidiEvent::Type::kStop, 0, 0, 0); for (int b = 0; b < 1500; b++) blk9();
        src9.send(MidiEvent::Type::kNoteOn, 0, 60, 100); for (int b = 0; b < 20; b++) blk9();
        src9.send(MidiEvent::Type::kNoteOn, 0, 62, 100); float both = 0; for (int b = 0; b < 20; b++) both = std::max(both, blk9());
        printf("  T-Type snare alone %.4f, kick then snare %.4f\n", snareOnly, both);
        check(std::isfinite(both) && both > snareOnly * 0.9f, "T-Type: a second drum does not cut the first (one voice, drums overlap)");
        for (int nn : {60, 62, 64, 66, 69, 70, 48, 52}) src9.send(MidiEvent::Type::kNoteOn, 0, nn, 127);
        float pkAll = 0; for (int b = 0; b < 100; b++) pkAll = std::max(pkAll, blk9());
        for (int b = 0; b < 3000; b++) blk9(); float tail = 0; for (int b = 0; b < 4; b++) tail = std::max(tail, blk9());
        printf("  T-Type eight drums at once peak=%.4f, after 8.7 s peak=%.6f\n", pkAll, tail);
        check(std::isfinite(pkAll) && pkAll > 1e-3f && tail < 1e-4f, "T-Type: eight drums together stay finite and the kit dies out");
      }
    }
  }
  // ---- idle part: open::partIdle is what PolySynth::update asks to skip a part that renders nothing (no blocks sent at all)
  {
    auto sI = Synth::createSynth([](size_t){return nullptr;});
    auto stI = sI->createPatchStore(Synth::FileSystemId::kPatches, true);
    auto pI = std::shared_ptr<Patch>(stI->createPatch(kLead));
    Src srcI; auto iI = sI->createInstrument(0); iI->assignPatch(pI); iI->connect(srcI); iI->setVoiceCount(1); iI->setMidiChannel(0);
    std::shared_ptr<Instrument> aI[1] = { iI };
    sI->setup(std::span<std::shared_ptr<Instrument>>(aI,1), {}, nullptr, []{ return tiny::optional<float>(120.0f); });
    auto blkI = [&]() { sI->renderAudioBuffer({AudioBufferRef(L),AudioBufferRef(R)}, std::span<AudioBufferRef>(aux), 0); return peakOf(L); };
    check(open::partIdle(0), "idle part: 1 voice allowed, nothing sounding -> partIdle");
    check(!open::partIdle(1) && !open::partIdle(-1), "a part that does not exist is never idle (old full path)");
    srcI.send(MidiEvent::Type::kNoteOn, 0, 57, 100);
    check(!open::partIdle(0), "note on: the part is not idle (checked before the first block renders)");
    float pkI = 0; for (int b = 0; b < 40; b++) pkI = std::max(pkI, blkI());
    check(!open::partIdle(0) && pkI > 1e-3f, "note held: not idle, sounds");
    srcI.send(MidiEvent::Type::kNoteOff, 0, 57, 0);
    check(!open::partIdle(0), "note off: the release still sounds, not idle");
    for (int b = 0; b < 3000 && !open::partIdle(0); b++) blkI();
    float tailI = 0; for (int b = 0; b < 4; b++) tailI = std::max(tailI, blkI());
    check(open::partIdle(0) && tailI < 1e-4f, "voice ended: idle again and the block it would have sent is silent");
    srcI.send(MidiEvent::Type::kNoteOn, 0, 57, 100);
    iI->setVoiceCount(0);
    check(open::partIdle(0), "Voices set to 0: idle");
  }
  printf(bad ? "SMOKE TEST FAILED\n" : "voice pool, governor, patch files and pitch bend OK\n");
  return bad ? 1 : 0;
}
