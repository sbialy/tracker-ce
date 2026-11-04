// PHZ cycle bench, target side: bare-metal Cortex-M4F program that renders PHZ blocks. NOT firmware. run_bench.sh builds
// it with the firmware's compiler and flags (-Os, -mcpu=cortex-m4 -mfpu=fpv4-sp-d16 -mfloat-abi=hard) and
// run_bench.py executes it in an emulator, counting the executed instructions of bench_run() and turning them into an
// estimated cycle count with a Cortex-M4 cost table.
#include "open_engine.h"
#include "phz/phz.h"

using namespace ot::audio;

namespace polysynth::open
{
extern const EngineDef kPhzDef;
}

namespace
{
SynthSlotScratch gScratch;
SynthVoice gVoice;
SynthParams gP;
float gL[kBlock], gR[kBlock];
ISynthEngine* gEng;

void setP(int id, float v)
{
    gP.end[id] = v;
    gP.start[id] = v;
}
void mod(int slot, int src, int dst, float amt)
{
    setP(phz::kModBase + 3 * slot, float(src));
    setP(phz::kModBase + 3 * slot + 1, float(dst));
    setP(phz::kModBase + 3 * slot + 2, amt);
}
} // namespace

// scenario ids: 0 default, 1 cutoff Off, 2 worst, 3 + model*4 + charKind: model x character matrix (Cutoff Off, both osc)
extern "C" int bench_scenarios() { return 3 + 6 * 4; }

extern "C" void bench_setup(int sc)
{
    const auto& def = polysynth::open::kPhzDef;
    for (int i = 0; i < def.paramCount; i++)
    {
        gP.start[i] = gP.end[i] = def.params[i].def;
    }
    gP.count = def.paramCount;
    gP.serial = 1;
    gP.moving = false;
    gEng = phz::engine();
    gEng->slotReset(gScratch);
    gVoice = SynthVoice();
    gVoice.slotScratch = &gScratch;
    if (sc == 1)
    {
        setP(phz::kCutoff, 20000.0f);
    }
    else if (sc == 2)
    {
        setP(phz::kModel, 5);
        setP(phz::kChar1, 2);
        setP(phz::kChar2, 2);
        setP(phz::kShape1, 0.7f);
        setP(phz::kShape2, 0.4f);
        setP(phz::kX1, 0.8f);
        setP(phz::kY1, 0.9f);
        setP(phz::kX2, 0.3f);
        setP(phz::kY2, 0.7f);
        setP(phz::kFilterType, 1);
        setP(phz::kResonance, 0.9f);
        setP(phz::kGlideTime, 0.5f);
        setP(phz::kLfo1Wave, 5);
        setP(phz::kLfo1Retrig, 1);
        setP(phz::kLfo2Retrig, 2);
        mod(0, phz::kSrcLfo1, phz::kDstX1, 0.5f);
        mod(1, phz::kSrcLfo2, phz::kDstX2, 0.5f);
        mod(2, phz::kSrcAuxEnv, phz::kDstY1, 0.5f);
        mod(3, phz::kSrcAmpEnv, phz::kDstShape1, 0.5f);
        mod(4, phz::kSrcFltEnv, phz::kDstCutoff, 0.2f);
        mod(5, phz::kSrcLfo1, phz::kDstPitch1, 0.02f);
    }
    else if (sc >= 3)
    {
        const int model = (sc - 3) / 4, kind = (sc - 3) % 4;
        setP(phz::kCutoff, 20000.0f);
        setP(phz::kModel, float(model));
        setP(phz::kChar1, float(kind == 3 ? 6 : kind));
        setP(phz::kChar2, float(kind == 3 ? 6 : kind));
        setP(phz::kShape1, 0.5f);
        setP(phz::kShape2, 0.5f);
        setP(phz::kY1, 0.4f);
        setP(phz::kY2, 0.4f);
    }
    SynthNote n;
    n.pitch = 57.0f;
    gEng->prepare(gScratch, gP);
    gEng->noteOn(gVoice, gP, n);
    for (int b = 0; b < 6; b++)
    {
        for (uint32_t i = 0; i < kBlock; i++) gL[i] = gR[i] = 0.0f;
        gEng->prepare(gScratch, gP);
        gEng->render(gVoice, gP, 1.0f, 0.0f, gL, gR);
    }
}

extern "C" void bench_run(int blocks)
{
    for (int b = 0; b < blocks; b++)
    {
        for (uint32_t i = 0; i < kBlock; i++) gL[i] = gR[i] = 0.0f;
        gEng->prepare(gScratch, gP);
        gEng->render(gVoice, gP, 1.0f, 0.0f, gL, gR);
    }
}

extern "C" float bench_peak()
{
    float p = 0;
    for (uint32_t i = 0; i < kBlock; i++) p = gL[i] > p ? gL[i] : (-gL[i] > p ? -gL[i] : p);
    return p;
}

// Bare metal: no C++ runtime, no heap, no exit
extern "C" void* memcpy(void* d, const void* s, unsigned n)
{
    auto* dd = static_cast<unsigned char*>(d);
    auto* ss = static_cast<const unsigned char*>(s);
    while (n--) *dd++ = *ss++;
    return d;
}
extern "C" void* memset(void* d, int c, unsigned n)
{
    auto* dd = static_cast<unsigned char*>(d);
    while (n--) *dd++ = static_cast<unsigned char>(c);
    return d;
}
extern "C" void __cxa_pure_virtual() { for (;;) {} }
void operator delete(void*, unsigned) {}
void operator delete(void*) {}
