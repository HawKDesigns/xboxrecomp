#ifndef XBOX_MAIN_PC_SAMPLES_H
#define XBOX_MAIN_PC_SAMPLES_H
#include <stdint.h>
#include <string.h>
#define XBOX_MAIN_SAMPLE_COUNT 20
/* One five-second window, sampled every 250 ms. No guest-memory reads. */
typedef struct {
    uint32_t pc[XBOX_MAIN_SAMPLE_COUNT];
    unsigned hits[XBOX_MAIN_SAMPLE_COUNT], used, total;
} XboxMainPcSamples;
static inline void xbox_main_sample_add(XboxMainPcSamples *window, uint32_t pc)
{
    unsigned i;
    for (i = 0; i < window->used; ++i) if (window->pc[i] == pc) break;
    if (i == window->used && window->used < XBOX_MAIN_SAMPLE_COUNT)
        window->pc[window->used++] = pc;
    if (i < window->used) ++window->hits[i];
    ++window->total;
}
static inline int xbox_main_sample_best(const XboxMainPcSamples *window,
                                        uint32_t excluded)
{
    int best = -1;
    for (unsigned i = 0; i < window->used; ++i)
        if (!(excluded & (1u << i)) &&
                (best < 0 || window->hits[i] > window->hits[best])) best = (int)i;
    return best;
}
#endif
