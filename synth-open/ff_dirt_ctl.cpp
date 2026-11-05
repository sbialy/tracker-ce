// DIRT live master bus insert, the cold part (see ff_dirt.h): the preset table, the coefficients (powf and tanf, once per
// preset change, never in the block loop) and the AudioStream object. Built at -Os, the per block DSP is in ff_dirt.cpp.
//
// Copyright (c) 2026 Marcin Spoczynski
// Port of the DIRT module of opentracker-fx (modules/dirt/src/dirt.cpp, commit 6e5a7da), MIT licence, text in licenses/LICENSE-OpenTracker-fx-MIT.txt.
#include "ff_dirt.h"

#include <math.h>
#include <string.h>

namespace ffdirt
{

const Preset kPresets[kPresetCount] = {
    {kSoft, 35, 16, 1, 100, "Dirt Soft"},
    {kHard, 35, 16, 1, 100, "Dirt Hard"},
    {kFold, 35, 16, 1, 100, "Dirt Fold"},
    {kAsym, 35, 16, 1, 100, "Dirt Asym"},
    {kSoft, 15, 8, 4, 70, "Dirt Crush"},
};

namespace
{
constexpr float kBias = 0.3f;                                                                // Asym: tanh(x + b) - tanh(b)
constexpr float kBiasY = kBias * (27.0f + kBias * kBias) / (27.0f + 9.0f * kBias * kBias);   // the rational tanh at kBias
constexpr float kPi = 3.14159265f;
}   // namespace

void makeCoef(Coef& c, const Preset& p)
{
    const float d = float(p.drive > 100 ? 100 : p.drive) * 0.01f;
    const int bits = p.bits < 4 ? 4 : (p.bits > 16 ? 16 : p.bits);
    const uint32_t rate = p.rate < 1 ? 1 : (p.rate > 32 ? 32 : p.rate);
    const bool tanhType = p.type == kSoft || p.type == kAsym;
    const bool asym = p.type == kAsym;
    const float pre = 1.0f + 15.0f * d * d;
    c.type = p.type;
    c.pre = tanhType ? pre * (1.0f / 3.0f) : pre;
    c.post = 1.0f / (1.0f + 0.5f * d);
    c.bias = asym ? kBias * (1.0f / 3.0f) : 0.0f;
    c.biasY = asym ? kBiasY : 0.0f;
    c.dcA = asym ? 2.0f * kPi * 10.0f / kFs * float(rate) : 0.0f;   // 10 Hz, updated once per hold
    c.quant = bits < 16;
    c.mask = ~((1u << (32 - bits)) - 1u);
    c.half = 1u << (31 - bits);
    c.rate = rate;
    c.tone = p.tone < 100;
    c.g = 0.0f;
    if (c.tone)
    {
        const float hz = 200.0f * powf(2.0f, 6.64385619f * float(p.tone) * 0.01f);
        const float g = tanf(kPi * hz / kFs);
        c.g = g / (1.0f + g);
    }
}

}   // namespace ffdirt

#ifdef FEATURE_FF_DIRT

void AudioEffectDirt::update(void)
{
    const uint8_t want = target_;
    float w1;
    if (cur_ != want && wet_ > 0.0f)
    {
        w1 = 0.0f;   // fade the old preset out first, through the dry signal
    }
    else
    {
        if (cur_ != want)
        {
            cur_ = want;
            wet_ = 0.0f;
            st_.reset();
            if (cur_) ffdirt::makeCoef(coef_, ffdirt::kPresets[cur_ - 1]);
        }
        w1 = cur_ ? 1.0f : 0.0f;
    }

    if (cur_ == 0)
    {   // off: the blocks pass untouched
        for (unsigned ch = 0; ch < 2; ch++)
        {
            audio_block_t* b = receiveReadOnly(ch);
            if (b)
            {
                transmit(b, ch);
                release(b);
            }
        }
        return;
    }

    audio_block_t* blk[2] = {receiveWritable(0), receiveWritable(1)};
    if (!blk[0] && !blk[1])
    {   // silent bus: nothing to do, nothing left of the state
        st_.reset();
        wet_ = w1;
        return;
    }
    for (int ch = 0; ch < 2; ch++)
    {
        if (blk[ch]) continue;
        blk[ch] = allocate();
        if (!blk[ch])
        {
            if (blk[ch ^ 1]) release(blk[ch ^ 1]);
            return;
        }
        memset(blk[ch]->data, 0, sizeof(blk[ch]->data));
    }
    ffdirt::process(coef_, st_, blk[0]->data, blk[1]->data, wet_, w1);
    wet_ = w1;
    for (unsigned ch = 0; ch < 2; ch++)
    {
        transmit(blk[ch], ch);
        release(blk[ch]);
    }
}

#endif
