// Idle synth part cost bench (NOT firmware). Real AudioStream core (copy), AudioAmplifier, AudioMixer<11>, AudioAnalyzeRMS, applyGain.
// PolySynth::update is a literal copy of core/libs/Audio/synth_PolySynth.cpp with the Synth stubbed (renderAudioBuffer clears
// like OpenSynth::renderAudioBuffer, OpenInstrument::render returns at once: no voice sounds). -DIDLE_SKIP adds the fix.
#include "AudioStream.h"
#include "amplifier.h"
#include "analyze_rms.h"
#include "mixer.h"
#include <algorithm>
#include <array>
#include <cmath>
#include <span>

namespace polysynth
{
using sample_t = float;
constexpr int kAudioBufferSize = 128;
using AudioBufferRef = std::span<sample_t>;
namespace sdk
{
inline constexpr float dbToAmplitude(float db) { return db > -90.0f ? std::pow(10.0f, db * 0.05f) : 0.0f; }
inline void saturateSoftClip(std::span<sample_t> buf, float gainDb)
{
    const float gainRaw = dbToAmplitude(gainDb);
    for (auto& element : buf)
    {
        float sample = std::clamp(element * gainRaw, -1.5f, 1.5f);
        sample = sample - (sample * sample * sample) / 6.72f;
        element = std::clamp(sample, -1.0f, 1.0f);
    }
}
} // namespace sdk
} // namespace polysynth

#ifndef BENCH_GAIN_DB
#define BENCH_GAIN_DB -9.0f // Synth::kGainDefault, a new project
#endif

namespace
{
bool gHasVoices = false;
}

// ---- PolySynth::update, literal copy of the non-sends mono path (the only one an open engine takes), stubs marked STUB
extern "C" bool bench_part_idle(int) { return !gHasVoices; }

class BenchPolySynth : public AudioStream
{
  public:
    BenchPolySynth() : AudioStream(0, NULL) {}
    static constexpr int kSend1Offset = 2 * kSynthParts;
    static constexpr int kSend2Offset = 3 * kSynthParts;
    static constexpr int kRightChannelOffset = kSynthParts;
    float gainDB[kSynthParts] = { BENCH_GAIN_DB, BENCH_GAIN_DB, BENCH_GAIN_DB };
    int voiceCount = 1;

    void update() override
    {
        static std::array<polysynth::sample_t, 128> tempBufSendsCommonLeft;
        static std::array<polysynth::sample_t, 128> tempBufSendsCommonRight;
        static std::array<polysynth::sample_t, 128> tempBufPostfaderLeft;
        static std::array<polysynth::sample_t, 128> tempBufPostfaderRight;
        static std::array<std::array<polysynth::sample_t, 128>, 2> tempBufSend;

        for (int partIdx = 0; partIdx < kSynthParts; partIdx++)
        {
            if (voiceCount == 0)
            {
                continue; // (renderMode false)
            }
#ifdef IDLE_SKIP
            if (bench_part_idle(partIdx))
            {
                continue;
            }
#endif
            bool isStereo = false;
            bool hasSends = false;
            audio_block_t* blockPostFaderLeft = allocate();
            audio_block_t* blockPostFaderRight = isStereo ? allocate() : NULL;
            audio_block_t* blockSendsCommonOrSend1 = allocate();
            audio_block_t* blockSend2 = hasSends ? allocate() : NULL;
            if (blockPostFaderLeft == NULL || blockSendsCommonOrSend1 == NULL)
            {
                return;
            }
            std::array<polysynth::AudioBufferRef, 2> tempSendsBufArray = { tempBufSend[0], tempBufSend[1] };

            // STUB: OpenSynth::renderAudioBuffer(bufferMain, bufferAux, id): clears, render() returns at once, mono mix down
            for (auto* b : { &tempBufSendsCommonLeft, &tempBufSendsCommonRight })
                std::fill(b->begin(), b->end(), 0.0f);
            for (auto& aux : tempSendsBufArray)
                std::fill(aux.begin(), aux.end(), 0.0f);
            for (int i = 0; i < 128; i++)
                tempBufSendsCommonLeft[i] = 0.5f * (tempBufSendsCommonLeft[i] + tempBufSendsCommonRight[i]);

            const bool reuseUnmodifiedMono = (!hasSends && !isStereo && gainDB[partIdx] == 0.0f);
            if (!reuseUnmodifiedMono)
            {
                tempBufPostfaderLeft = tempBufSendsCommonLeft;
            }
            polysynth::AudioBufferRef tempBufRef = tempBufSendsCommonLeft;
            polysynth::sdk::saturateSoftClip(tempBufRef, 0.0f);
            {
                auto* src = tempBufSendsCommonLeft.begin();
                auto* dest = blockSendsCommonOrSend1->data;
                for (int i = 0; i < 128; i++)
                    *(dest++) = static_cast<int16_t>(*(src++) * INT16_MAX);
            }
            if (reuseUnmodifiedMono)
            {
                memcpy(blockPostFaderLeft->data, blockSendsCommonOrSend1->data, sizeof(blockPostFaderLeft->data));
            }
            transmit(blockSendsCommonOrSend1, kSend1Offset + partIdx);
            transmit(blockSendsCommonOrSend1, kSend2Offset + partIdx);
            release(blockSendsCommonOrSend1);
            if (!reuseUnmodifiedMono)
            {
                tempBufRef = tempBufPostfaderLeft;
                polysynth::sdk::saturateSoftClip(tempBufRef, gainDB[partIdx]);
                auto* src = tempBufPostfaderLeft.begin();
                auto* dest = blockPostFaderLeft->data;
                for (int i = 0; i < 128; i++)
                    *(dest++) = static_cast<int16_t>(*(src++) * INT16_MAX);
            }
            transmit(blockPostFaderLeft, partIdx);
            transmit(blockPostFaderLeft, kRightChannelOffset + partIdx);
            release(blockPostFaderLeft);
        }
    }
};

// drain: stands for the export queue (record queue, not recording): takes the block, releases it
class Sink : public AudioStream
{
  public:
    Sink() : AudioStream(1, q) {}
    void update() override { if (auto* b = receiveReadOnly(0)) release(b); }
    audio_block_t* q[1];
};
// stands for a playing sample track: a block of non-zero audio into mixer input 0 each cycle
class Track : public AudioStream
{
  public:
    Track() : AudioStream(0, NULL) {}
    void update() override
    {
        if (auto* b = allocate())
        {
            for (int i = 0; i < 128; i++) b->data[i] = (int16_t)((i * 211) & 0x3fff);
            transmit(b, 0);
            release(b);
        }
    }
};

namespace
{
alignas(8) audio_block_t gPool[70];
BenchPolySynth gPoly;
Track gTrack[2];
AudioAmplifier gAmp[kSynthParts][2];
AudioAnalyzeRMS gRms[kSynthParts][2];
Sink gSink[kSynthParts][2];
AudioMixer<11> gMix[2];
AudioAnalyzeRMS gDryRms[2];
AudioConnection* gC[64];
int gN;
void con(AudioStream* s, int si, AudioStream* d, int di) { gC[gN++] = new AudioConnection(s, si, d, di); }
} // namespace

extern "C" void (*__init_array_start[])(void);
extern "C" void (*__init_array_end[])(void);
extern "C" void bench_setup(int sc)
{
    static bool once;
    if (!once) { once = true; for (auto f = __init_array_start; f < __init_array_end; f++) (*f)(); }
    AudioStream::initialize_memory(gPool, 70);
    for (int ch = 0; ch < 2; ch++)
    {
        con(&gTrack[ch], 0, &gMix[ch], 0);
        con(&gMix[ch], 0, &gDryRms[ch], 0);
    }
    for (int p = 0; p < kSynthParts && sc != 2; p++)
    {
        for (int ch = 0; ch < 2; ch++)
        {
            con(&gPoly, ch * kSynthParts + p, &gAmp[p][ch], 0);          // post fader L / R -> panning amplifier
            con(&gPoly, ch * kSynthParts + p, &gRms[p][ch], 0);          // UI analyser
            con(&gAmp[p][ch], 0, &gMix[ch], 8 + p);                      // panning amp -> tracks mixer
            con(&gAmp[p][ch], 0, &gSink[p][ch], 0);                      // panning amp -> export queue
        }
    }
    gPoly.voiceCount = sc == 0 ? 0 : 1;   // sc 0: voices 0 (skipped by stock code), 1: one voice allowed, nothing sounds
}

extern "C" void bench_run(int blocks)
{
    for (int b = 0; b < blocks; b++) AudioStream::update_all();
}

extern "C" int bench_used() { return AudioStream::memory_used; }

// ---- bare metal support
namespace { alignas(8) unsigned char gHeap[64 * 1024]; unsigned gHeapUsed; }
void* operator new(unsigned n) { void* p = gHeap + gHeapUsed; gHeapUsed += (n + 7u) & ~7u; return p; }
void operator delete(void*) {}
void operator delete(void*, unsigned) {}
// newlib's word wise memcpy / memset cost (the plain SYNTH_OPEN image, no TRIM): fastmem.S has the same shape
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
