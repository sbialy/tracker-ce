// Renders PHZ to WAV files on the host so it can be listened to. NOT firmware. Drives the engine directly through
// ISynthEngine (block by block, 128 frames, the parameters ramp start -> end like the shim does), no shim, no hardware.
//   synth-open/tools/phz_render.sh [outdir]      (default build/phz-wav)
// Files (mono, 16 bit, 44118 Hz, filter Off unless a demo sets it):
//   10_x_<model>.wav     X sweeps 0 -> 1 over one held note, Y fixed
//   20_y_<model>.wav     Y sweeps 0 -> 1, X fixed
//   30_char_<name>.wav   Shape sweeps 0 -> 1 on each Character row (OG Saw, X 0.55)
//   40_lead / 41_pad / 42_fm_bell / 43_bass .wav    four small patches with envelopes, LFOs and the mod matrix
//   00_all.wav           everything in this order, half a second apart
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <functional>
#include <string>
#include <vector>

#include "open_engine.h"
#include "phz/phz.h"

using namespace ot::audio;
namespace polysynth::open { extern const EngineDef kPhzDef; }

namespace
{
constexpr int kRate = 44118;
const char* const kModels[] = { "og_saw", "og_square", "hermite_smooth", "squared", "linear", "hermite_fm" };
const char* const kChars[] = { "smoother", "sharper", "wilder", "add1", "add2", "add3", "add5", "add7", "add11" };

struct Patch { float v[phz::kParamCount]; };

Patch initPatch()
{
    Patch p;
    const auto& d = polysynth::open::kPhzDef;
    for (int i = 0; i < d.paramCount; i++) p.v[i] = d.params[i].def;
    p.v[phz::kCutoff] = 20000.0f;   // filter Off
    p.v[phz::kFilterEnvAmt] = 0.0f;
    p.v[phz::kAmpS] = 1.0f;
    return p;
}

void mod(Patch& p, int slot, int src, int dst, float amt)
{
    p.v[phz::kModBase + 3 * slot] = float(src);
    p.v[phz::kModBase + 3 * slot + 1] = float(dst);
    p.v[phz::kModBase + 3 * slot + 2] = amt;
}

struct Note { double t, len; int note; };

// Renders notes of a patch (one engine voice per note, summed). automate(t, patch) may change the patch every block.
std::vector<float> render(Patch patch, const std::vector<Note>& notes, double seconds, const std::function<void(double, Patch&)>& automate = nullptr)
{
    ISynthEngine* eng = phz::engine();
    SynthSlotScratch sc;
    eng->slotReset(sc);
    struct V { SynthVoice v; double off; bool on = true, alive = true; };
    std::vector<V> voices(notes.size());
    for (size_t i = 0; i < notes.size(); i++) { voices[i].v = SynthVoice(); voices[i].v.slotScratch = &sc; voices[i].off = notes[i].t + notes[i].len; }
    SynthParams P;
    P.count = phz::kParamCount;
    std::memcpy(P.start, patch.v, sizeof(patch.v));
    std::memcpy(P.end, patch.v, sizeof(patch.v));
    std::vector<float> out;
    const int blocks = int(seconds * kRate / kBlock) + 1;
    std::vector<bool> started(notes.size(), false);
    for (int b = 0; b < blocks; b++)
    {
        const double t = double(b) * kBlock / kRate;
        std::memcpy(P.start, P.end, sizeof(P.start));
        if (automate) { automate(t, patch); }
        std::memcpy(P.end, patch.v, sizeof(patch.v));
        P.moving = std::memcmp(P.start, P.end, sizeof(P.start)) != 0;
        P.serial++;
        float L[kBlock] = {}, R[kBlock] = {};
        eng->prepare(sc, P);
        for (size_t i = 0; i < notes.size(); i++)
        {
            V& vv = voices[i];
            if (!started[i] && t >= notes[i].t)
            {
                SynthNote n; n.pitch = float(notes[i].note); n.note = uint8_t(notes[i].note);
                eng->noteOn(vv.v, P, n); started[i] = true;
            }
            if (started[i] && vv.on && t >= vv.off) { eng->noteOff(vv.v, P); vv.on = false; }
            if (started[i] && vv.alive) vv.alive = eng->render(vv.v, P, 1.0f, 0.0f, L, R);
        }
        out.insert(out.end(), L, L + kBlock);
    }
    return out;
}

void writeWav(const std::string& path, const std::vector<float>& x)
{
    FILE* f = fopen(path.c_str(), "wb");
    if (!f) { perror(path.c_str()); return; }
    const uint32_t n = uint32_t(x.size()), bytes = n * 2, rate = kRate, br = rate * 2;
    const uint16_t fmt = 1, ch = 1, align = 2, bits = 16;
    const uint32_t sixteen = 16, riff = 36 + bytes;
    fwrite("RIFF", 1, 4, f); fwrite(&riff, 4, 1, f); fwrite("WAVEfmt ", 1, 8, f); fwrite(&sixteen, 4, 1, f);
    fwrite(&fmt, 2, 1, f); fwrite(&ch, 2, 1, f); fwrite(&rate, 4, 1, f); fwrite(&br, 4, 1, f); fwrite(&align, 2, 1, f); fwrite(&bits, 2, 1, f);
    fwrite("data", 1, 4, f); fwrite(&bytes, 4, 1, f);
    for (float v : x) { const int16_t s = int16_t(std::lround(std::clamp(v, -1.0f, 1.0f) * 32767.0f)); fwrite(&s, 2, 1, f); }
    fclose(f);
}

std::vector<float> gAll;
std::string gDir;

void emit(const std::string& name, std::vector<float> x, float gain = 0.8f)
{
    float pk = 0; double e = 0;
    for (float v : x) { pk = std::max(pk, std::fabs(v)); e += v * v; }
    for (float& v : x) v *= gain;
    writeWav(gDir + "/" + name + ".wav", x);
    printf("%-26s %5.1f s  peak %.2f  rms %.3f\n", (name + ".wav").c_str(), double(x.size()) / kRate, pk * gain, std::sqrt(e / std::max<size_t>(x.size(), 1)) * gain);
    gAll.insert(gAll.end(), x.begin(), x.end());
    gAll.insert(gAll.end(), kRate / 2, 0.0f);
}
} // namespace

int main(int argc, char** argv)
{
    gDir = argc > 1 ? argv[1] : "build/phz-wav";
    // X and Y sweeps: one oscillator (Mix 0), A2, 4 s, gate 3.6 s
    const float yFixed[] = { 0.5f, 0.5f, 0.85f, 0.85f, 0.85f, 0.6f }, xFixed[] = { 0.5f, 0.5f, 0.35f, 0.35f, 0.35f, 0.5f };
    for (int m = 0; m < 6; m++)
    {
        Patch p = initPatch();
        p.v[phz::kMix] = 0; p.v[phz::kDetune] = 0; p.v[phz::kModel] = float(m); p.v[phz::kAmpR] = 0.2f;
        p.v[phz::kY1] = yFixed[m];
        emit(std::string("10_x_") + kModels[m], render(p, { { 0.1, 3.6, 45 } }, 4.0, [&](double t, Patch& q) { q.v[phz::kX1] = float(std::clamp((t - 0.1) / 3.5, 0.0, 1.0)); }));
    }
    for (int m = 0; m < 6; m++)
    {
        Patch p = initPatch();
        p.v[phz::kMix] = 0; p.v[phz::kDetune] = 0; p.v[phz::kModel] = float(m); p.v[phz::kAmpR] = 0.2f;
        p.v[phz::kX1] = xFixed[m];
        emit(std::string("20_y_") + kModels[m], render(p, { { 0.1, 3.6, 45 } }, 4.0, [&](double t, Patch& q) { q.v[phz::kY1] = float(std::clamp((t - 0.1) / 3.5, 0.0, 1.0)); }));
    }
    for (int c = 0; c < 9; c++)
    {
        Patch p = initPatch();
        p.v[phz::kMix] = 0; p.v[phz::kDetune] = 0; p.v[phz::kModel] = 0; p.v[phz::kX1] = 0.55f; p.v[phz::kChar1] = float(c); p.v[phz::kAmpR] = 0.2f;
        emit(std::string("30_char_") + kChars[c], render(p, { { 0.1, 3.6, 45 } }, 4.0, [&](double t, Patch& q) { q.v[phz::kShape1] = float(std::clamp((t - 0.1) / 3.5, 0.0, 1.0)); }));
    }
    {   // 40 lead: Linear, two detuned oscillators, glide, filter envelope, vibrato from LFO 1
        Patch p = initPatch();
        p.v[phz::kModel] = 4; p.v[phz::kX1] = 0.3f; p.v[phz::kY1] = 0.75f; p.v[phz::kX2] = 0.35f; p.v[phz::kY2] = 0.7f; p.v[phz::kDetune] = 14;
        p.v[phz::kTune2] = 12; p.v[phz::kMix] = 0.35f; p.v[phz::kGlideMode] = 0; p.v[phz::kGlideTime] = 0.06f;
        p.v[phz::kCutoff] = 1800; p.v[phz::kResonance] = 0.35f; p.v[phz::kFilterType] = 3; p.v[phz::kFilterEnvAmt] = 0.35f;
        p.v[phz::kAmpA] = 0.01f; p.v[phz::kAmpD] = 0.3f; p.v[phz::kAmpS] = 0.7f; p.v[phz::kAmpR] = 0.25f;
        p.v[phz::kFltD] = 0.35f; p.v[phz::kFltS] = 0.3f;
        p.v[phz::kLfo1Ratio] = 5.5f; p.v[phz::kLfo1Wave] = 1; p.v[phz::kLfo1Retrig] = 2;
        mod(p, 0, phz::kSrcLfo1, phz::kDstPitch1, 0.012f); mod(p, 1, phz::kSrcLfo1, phz::kDstPitch2, 0.012f);
        mod(p, 2, phz::kSrcFltEnv, phz::kDstY1, 0.15f);
        const int seq[] = { 62, 65, 69, 67, 65, 62, 64, 60 };
        std::vector<Note> n; double t = 0.1;
        for (int i = 0; i < 8; i++) { n.push_back({ t, 0.42, seq[i] + 12 }); t += 0.5; }
        emit("40_lead", render(p, n, t + 0.8));
    }
    {   // 41 pad: Hermite Smooth, slow envelope, LFO 2 moving X, a chord
        Patch p = initPatch();
        p.v[phz::kModel] = 2; p.v[phz::kX1] = 0.4f; p.v[phz::kY1] = 0.7f; p.v[phz::kX2] = 0.45f; p.v[phz::kY2] = 0.65f; p.v[phz::kDetune] = 22;
        p.v[phz::kMix] = 0.5f; p.v[phz::kChar2] = 3; p.v[phz::kShape2] = 0.3f;
        p.v[phz::kCutoff] = 3000; p.v[phz::kResonance] = 0.15f; p.v[phz::kFilterType] = 4; p.v[phz::kFilterEnvAmt] = 0.2f;
        p.v[phz::kAmpA] = 0.8f; p.v[phz::kAmpD] = 1.0f; p.v[phz::kAmpS] = 0.8f; p.v[phz::kAmpR] = 1.2f; p.v[phz::kFltA] = 1.0f; p.v[phz::kFltS] = 0.6f;
        p.v[phz::kLfo2Ratio] = 0.35f; p.v[phz::kLfo2Wave] = 0; p.v[phz::kLfo2Retrig] = 0;
        mod(p, 0, phz::kSrcLfo2, phz::kDstX1, 0.35f); mod(p, 1, phz::kSrcLfo2, phz::kDstY2, -0.3f);
        std::vector<Note> n = { { 0.1, 4.0, 50 }, { 0.1, 4.0, 57 }, { 0.1, 4.0, 62 }, { 0.1, 4.0, 65 } };
        emit("41_pad", render(p, n, 6.5), 0.5f);
    }
    {   // 42 FM bell: Hermite FM, Aux envelope drives Y (the fold) down, no filter
        Patch p = initPatch();
        p.v[phz::kModel] = 5; p.v[phz::kX1] = 0.5f; p.v[phz::kY1] = 0.0f; p.v[phz::kX2] = 0.5f; p.v[phz::kY2] = 0.0f; p.v[phz::kMix] = 0.5f;
        p.v[phz::kTune2] = 7; p.v[phz::kDetune] = 3;
        p.v[phz::kAmpA] = 0.0f; p.v[phz::kAmpD] = 2.2f; p.v[phz::kAmpS] = 0.0f; p.v[phz::kAmpR] = 0.5f;
        p.v[phz::kAuxD] = 0.9f; p.v[phz::kAuxS] = 0.0f;
        mod(p, 0, phz::kSrcAuxEnv, phz::kDstY1, 0.9f); mod(p, 1, phz::kSrcAuxEnv, phz::kDstY2, 0.5f);
        const int seq[] = { 72, 76, 79, 84 };
        std::vector<Note> n; double t = 0.1;
        for (int i = 0; i < 4; i++) { n.push_back({ t, 1.2, seq[i] }); t += 1.0; }
        emit("42_fm_bell", render(p, n, t + 2.0));
    }
    {   // 43 bass: OG Square, LP MG 24, filter envelope
        Patch p = initPatch();
        p.v[phz::kModel] = 1; p.v[phz::kX1] = 0.55f; p.v[phz::kY1] = 0.5f; p.v[phz::kX2] = 0.45f; p.v[phz::kY2] = 0.5f; p.v[phz::kMix] = 0.5f; p.v[phz::kTune2] = -12;
        p.v[phz::kCutoff] = 260; p.v[phz::kResonance] = 0.5f; p.v[phz::kFilterType] = 0; p.v[phz::kFilterEnvAmt] = 0.55f; p.v[phz::kNoteTrack] = 0.5f;
        p.v[phz::kAmpA] = 0.003f; p.v[phz::kAmpD] = 0.25f; p.v[phz::kAmpS] = 0.6f; p.v[phz::kAmpR] = 0.12f; p.v[phz::kFltD] = 0.2f; p.v[phz::kFltS] = 0.1f;
        const int seq[] = { 33, 33, 45, 33, 36, 33, 40, 38 };
        std::vector<Note> n; double t = 0.1;
        for (int i = 0; i < 8; i++) { n.push_back({ t, 0.22, seq[i] }); t += 0.25; }
        emit("43_bass", render(p, n, t + 0.5));
    }
    writeWav(gDir + "/00_all.wav", gAll);
    printf("00_all.wav                 %5.1f s\n", double(gAll.size()) / kRate);
    return 0;
}
