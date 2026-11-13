#ifndef XBOX_FRAME_TRACE_H
#define XBOX_FRAME_TRACE_H

#include <stddef.h>
#include <stdint.h>

enum XboxTraceProducer {
    XBOX_TRACE_RENDERER,
    XBOX_TRACE_WINDOW,
    XBOX_TRACE_TITLE,
    XBOX_TRACE_PRODUCERS
};

enum XboxTraceEvent {
    XBOX_TRACE_FLIP = 1,
    XBOX_TRACE_HANDOFF,
    XBOX_TRACE_DRAW,
    XBOX_TRACE_PHASE,
    XBOX_TRACE_CAPTURE_BEGIN,
    XBOX_TRACE_CAPTURE_END,
    XBOX_TRACE_CPU_METRIC,
    XBOX_TRACE_GPU_SAMPLE,
    XBOX_TRACE_GUEST_FENCE_WAIT,
    XBOX_TRACE_PAUSE,
    XBOX_TRACE_PROFILE_FRAME
};

enum XboxTracePhase {
    XBOX_PHASE_BOOT = 1,
    XBOX_PHASE_PREVIEW,
    XBOX_PHASE_COUNTDOWN,
    XBOX_PHASE_AHEAD,
    XBOX_PHASE_BEHIND,
    XBOX_PHASE_UNMARKED
};

#if defined(XBOX_FRAME_TELEMETRY)
int xbox_frame_trace_start(const wchar_t *path);
void xbox_frame_trace_stop(void);
int xbox_frame_trace_enabled(void);
void xbox_frame_trace_note(enum XboxTraceProducer producer, enum XboxTraceEvent event,
                           uint32_t frame, uint32_t arg0, uint32_t arg1, uint32_t arg2);
void xbox_frame_trace_guest_ticks(uint32_t elapsed);
uint32_t xbox_frame_trace_guest_tick_count(void);
#else
static inline int xbox_frame_trace_enabled(void) { return 0; }
static inline void xbox_frame_trace_note(enum XboxTraceProducer producer, enum XboxTraceEvent event,
                                         uint32_t frame, uint32_t arg0, uint32_t arg1, uint32_t arg2)
{
    (void)producer; (void)event; (void)frame; (void)arg0; (void)arg1; (void)arg2;
}
static inline void xbox_frame_trace_guest_ticks(uint32_t elapsed) { (void)elapsed; }
static inline uint32_t xbox_frame_trace_guest_tick_count(void) { return 0; }
#endif

#endif
