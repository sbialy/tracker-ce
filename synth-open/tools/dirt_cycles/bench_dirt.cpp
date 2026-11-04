// Dirt cost bench (NOT firmware): the real AudioEffectDirt (synth-open/ff_dirt.cpp, ff_dirt_ctl.cpp) and the real AudioStream core
// between a stand-in source (copies a prepared stereo test block into two fresh blocks, like a mixer would hand them on) and a
// sink (receives and releases, like the limiter's input would). Scenario sc: 0 = Dirt off (pass through), 1..5 = preset 1..5 of
// ffdirt::kPresets (Soft Hard Fold Asym Crush), 6 = no Dirt object in the graph (the baseline), 7 = Soft with a silent bus.
#include "AudioStream.h"
#include "ff_dirt.h"
#include <math.h>
#include <stdint.h>

extern "C" int16_t bench_pat[2][AUDIO_BLOCK_SAMPLES];   // the test block the source repeats
extern "C" int16_t bench_out[2][AUDIO_BLOCK_SAMPLES];   // the last block the sink received
int16_t bench_pat[2][AUDIO_BLOCK_SAMPLES];
int16_t bench_out[2][AUDIO_BLOCK_SAMPLES];

namespace
{
alignas(8) audio_block_t gPool[16];

class Src : public AudioStream
{
  public:
    Src() : AudioStream(0, NULL) {}
    bool silent = false;
    void update() override
    {
        if (silent) return;
        for (unsigned ch = 0; ch < 2; ch++)
        {
            audio_block_t* b = allocate();
            if (!b) return;
            memcpy(b->data, bench_pat[ch], sizeof(b->data));
            transmit(b, ch);
            release(b);
        }
    }
};
volatile int gSink;
class Sink : public AudioStream
{
  public:
    Sink() : AudioStream(2, q_) {}
    void update() override
    {
        for (unsigned ch = 0; ch < 2; ch++)
        {
            audio_block_t* b = receiveReadOnly(ch);
            if (b)
            {
                gSink = b->data[7];
                memcpy(bench_out[ch], b->data, sizeof(b->data));
                release(b);
            }
        }
    }
    audio_block_t* q_[2];
};
Src gSrc;
AudioEffectDirt gDirt;
Sink gSink_;
AudioConnection* gC[8];
int gN;
void con(AudioStream* s, int si, AudioStream* d, int di) { gC[gN++] = new AudioConnection(s, si, d, di); }
} // namespace

extern "C" void (*__init_array_start[])(void);
extern "C" void (*__init_array_end[])(void);
extern "C" void bench_setup(int sc)
{
    static bool once;
    if (!once) { once = true; for (auto f = __init_array_start; f < __init_array_end; f++) (*f)(); }
    AudioStream::initialize_memory(gPool, 16);
    // a loud mixed down stand-in: a saw and a sine per side (a master bus is rarely quiet), about -3 dBFS peak
    for (int i = 0; i < AUDIO_BLOCK_SAMPLES; i++)
    {
        const float saw = (float)((i * 3) % AUDIO_BLOCK_SAMPLES) / 64.0f - 1.0f;
        bench_pat[0][i] = (int16_t)(16000.0f * saw + 6000.0f * sinf(0.35f * (float)i));
        bench_pat[1][i] = (int16_t)(14000.0f * saw * (i & 1 ? 1.0f : 0.8f) + 8000.0f * sinf(0.21f * (float)i));
    }
    if (sc == 6)
    {
        con(&gSrc, 0, &gSink_, 0);
        con(&gSrc, 1, &gSink_, 1);
    }
    else
    {
        con(&gSrc, 0, &gDirt, 0);
        con(&gSrc, 1, &gDirt, 1);
        con(&gDirt, 0, &gSink_, 0);
        con(&gDirt, 1, &gSink_, 1);
    }
    gDirt.setMode(sc >= 1 && sc <= 5 ? sc : (sc == 7 ? 1 : 0));
    gSrc.silent = sc == 7;
}

extern "C" void bench_run(int blocks)
{
    for (int b = 0; b < blocks; b++) AudioStream::update_all();
}

// ---- bare metal support
namespace { alignas(8) unsigned char gHeap[16 * 1024]; unsigned gHeapUsed; }
void* operator new(unsigned n) { void* p = gHeap + gHeapUsed; gHeapUsed += (n + 7u) & ~7u; return p; }
void operator delete(void*) {}
void operator delete(void*, unsigned) {}
// the word wise memcpy / memset of newlib and of fastmem.S (same shape, same cost)
extern "C" void* memcpy(void* d, const void* s, unsigned n)
{
    auto* dw = (unsigned*)d; auto* sw = (const unsigned*)s;
    for (; n >= 4; n -= 4) *dw++ = *sw++;
    return d;
}
extern "C" void* memset(void* d, int c, unsigned n)
{
    auto* dw = (unsigned*)d;
    for (; n >= 4; n -= 4) *dw++ = (unsigned)c;
    return d;
}
extern "C" void __cxa_pure_virtual() { for (;;) {} }
extern "C" void _exit(int) { for (;;) {} }
extern "C" int _kill(int, int) { return -1; }
extern "C" int _getpid() { return 1; }
