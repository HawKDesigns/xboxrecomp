#ifndef NV2A_FRAME_PACING_H
#define NV2A_FRAME_PACING_H
#include <stdint.h>
/* Completed presents only. This observes wall time and never drives guest
 * refresh counters, simulation steps, fences or presentation scheduling. */
typedef struct {
    double last_ms, total_ms, max_ms;
    uint64_t intervals, over_33ms, over_50ms, over_100ms;
    int seeded;
} NvFramePacing;
static void nv_frame_pacing_note(NvFramePacing *p, double now_ms)
{
    if (p->seeded && now_ms >= p->last_ms) {
        double ms=now_ms-p->last_ms;
        ++p->intervals;p->total_ms+=ms;
        if(ms>p->max_ms)p->max_ms=ms;
        /* One microsecond tolerance for clock quantization at 30 Hz. */
        p->over_33ms+=ms>(1000.0/30.0)+0.001;
        p->over_50ms+=ms>50.0;p->over_100ms+=ms>100.0;
    }
    p->last_ms=now_ms;p->seeded=1;
}
static void nv_frame_pacing_reset_window(NvFramePacing *p)
{
    /* Keep the previous present, including time spent between reports. */
    p->total_ms=p->max_ms=0;
    p->intervals=p->over_33ms=p->over_50ms=p->over_100ms=0;
}
#endif
