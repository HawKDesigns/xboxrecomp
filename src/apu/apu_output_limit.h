#ifndef APU_OUTPUT_LIMIT_H
#define APU_OUTPUT_LIMIT_H
#include <stdint.h>
#include <math.h>
/* PCM stays wide until every CPU voice has been summed. One common gain
 * preserves stereo balance; immediate attack prevents overload and a 100ms
 * exponential release avoids gain chatter between individual peaks.
 * This protects the DSP bypass mixdown; it does not emulate the guest DSP. */
typedef struct {
    float gain, input_peak;
    uint64_t frames, overload_frames, limited_frames, hard_clipped, nonfinite;
} APUOutputLimit;
static inline void apu_output_limit_reset_counts(APUOutputLimit *s)
{
    float gain=s->gain;
    *s=(APUOutputLimit){0};s->gain=gain;
}
static inline void apu_output_limit(APUOutputLimit *s, const float input[][2],
                                    int16_t output[][2], unsigned count, int enabled)
{
    for(unsigned i=0;i<count;i++) {
        float left=input[i][0],right=input[i][1];
        if(!isfinite(left)){left=0;++s->nonfinite;}
        if(!isfinite(right)){right=0;++s->nonfinite;}
        float peak=fmaxf(fabsf(left),fabsf(right));
        if(peak>s->input_peak)s->input_peak=peak;
        ++s->frames;if(peak>32767.0f)++s->overload_frames;
        if(enabled) {
            float desired=peak>32767.0f?32760.0f/peak:1.0f;
            float release=s->gain+(1.0f-s->gain)*(1.0f/4800.0f);
            if(release>0.9998f)release=1.0f; /* avoid float rounding plateau */
            s->gain=fminf(desired,release);
            if(s->gain<0.99999f)++s->limited_frames;
            left*=s->gain;right*=s->gain;
        } else s->gain=1.0f;
        if(left>32767.0f){left=32767.0f;++s->hard_clipped;}
        if(left< -32768.0f){left= -32768.0f;++s->hard_clipped;}
        if(right>32767.0f){right=32767.0f;++s->hard_clipped;}
        if(right< -32768.0f){right= -32768.0f;++s->hard_clipped;}
        output[i][0]=(int16_t)left;output[i][1]=(int16_t)right;
    }
}
#endif
