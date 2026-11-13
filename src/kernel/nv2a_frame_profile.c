#include "nv2a_frame_profile.h"
#include <stdlib.h>

#if defined(XBOX_FRAME_TELEMETRY)
static double s_metric[NV_FRAME_METRICS];
static double s_fifo_start;
static int s_fifo_active;
static uint32_t s_last_frame;

/* This state belongs to the FIFO executor. Renderer startup self-tests
 * do not call it. */
int nv2a_frame_profile_enabled(void)
{
    static int configured = -1;
    if (configured < 0) {
        const char *value = getenv("RECOMP_FRAME_PROFILE");
        configured = value && atoi(value) > 0;
    }
    return configured && xbox_frame_trace_enabled();
}

void nv2a_frame_profile_add(unsigned metric, double value)
{
    if (nv2a_frame_profile_enabled())
        s_metric[metric] += value;
}

void nv2a_frame_profile_fifo_enter(double now)
{
    s_fifo_start = now;
    s_fifo_active = 1;
}

void nv2a_frame_profile_fifo_leave(double now)
{
    if (s_fifo_active) s_metric[NV_FRAME_FIFO] += now - s_fifo_start;
    s_fifo_active = 0;
}

void nv2a_frame_profile_flip(uint32_t frame, uint32_t draws, double now)
{
    if (!nv2a_frame_profile_enabled()) return;
    if (s_fifo_active) {
        s_metric[NV_FRAME_FIFO] += now - s_fifo_start;
        s_fifo_start = now;
    }
    s_metric[NV_FRAME_DRAW_REQUESTS] = draws;
    s_last_frame = frame;
    for (unsigned i = 0; i < NV_FRAME_METRICS; ++i) {
        int is_count = i >= NV_FRAME_BATCHES && i < NV_FRAME_DEPENDENCY && i != NV_FRAME_FIFO_DRAW_MS;
        double value = s_metric[i] * (is_count ? 1.0 : 1000.0);
        s_metric[i] = 0;
        if (value >= 1.0)
            xbox_frame_trace_note(XBOX_TRACE_RENDERER, XBOX_TRACE_CPU_METRIC, frame, i,
                value >= UINT32_MAX ? UINT32_MAX : (uint32_t)value, 0);
    }
    xbox_frame_trace_note(XBOX_TRACE_RENDERER, XBOX_TRACE_PROFILE_FRAME, frame, 0, 0, 0);
}

uint32_t nv2a_frame_profile_next_id(void)
{
    return s_last_frame + 1;
}
#endif
