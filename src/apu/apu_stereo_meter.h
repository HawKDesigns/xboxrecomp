#ifndef APU_STEREO_METER_H
#define APU_STEREO_METER_H
#include <stdint.h>
#include <math.h>
/* Read-only measurements of the final interleaved PCM copied to the backend.
 * Similarity is the normalized L/R dot product, not a DC-corrected Pearson
 * coefficient. A silent channel yields zero similarity; check both RMS values. */
typedef struct {
    uint64_t frames,nonzero_frames,unequal_frames;
    double left_energy,right_energy,difference_energy,cross;
} APUStereoMeter;
static inline void apu_stereo_meter_add(APUStereoMeter *s,
                                        const int16_t pcm[][2],unsigned count)
{
    for(unsigned i=0;i<count;i++) {
        double l=pcm[i][0]/32768.0,r=pcm[i][1]/32768.0,d=l-r;
        ++s->frames;
        s->nonzero_frames+=pcm[i][0]!=0 || pcm[i][1]!=0;
        s->unequal_frames+=pcm[i][0]!=pcm[i][1];
        s->left_energy+=l*l;s->right_energy+=r*r;
        s->difference_energy+=d*d;s->cross+=l*r;
    }
}
static inline double apu_stereo_meter_rms(double energy,uint64_t frames)
{return frames?sqrt(energy/(double)frames):0.0;}
static inline double apu_stereo_meter_similarity(const APUStereoMeter *s)
{
    double denominator=sqrt(s->left_energy*s->right_energy);
    double result=denominator?s->cross/denominator:0.0;
    return fmax(-1.0,fmin(result,1.0));
}
#endif
