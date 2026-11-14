#ifndef NV2A_FRAME_PROFILE_H
#define NV2A_FRAME_PROFILE_H

#include "platform/frame_trace.h"

/* CPU timings use the FIFO owner, except guest waits which may overlap it.
 * All renderer counters accumulate to the next completed flip. */
enum NvFrameMetric {
    NV_FRAME_FIFO,
    NV_FRAME_UPLOAD,
    NV_FRAME_GEOMETRY,
    NV_FRAME_RASTER_SUBMIT,
    NV_FRAME_TARGET_BIND,
    NV_FRAME_CURRENT_MAP,
    NV_FRAME_QUEUED_MAP,
    NV_FRAME_PUBLICATION,
    NV_FRAME_COMPLETION_MAP,
    NV_FRAME_REPORT_MAP,
    NV_FRAME_BATCHES,
    NV_FRAME_TRIANGLES,
    NV_FRAME_DRAW_REQUESTS,
    NV_FRAME_GPU_SELECTED,
    NV_FRAME_GPU_DROPPED,
    NV_FRAME_GPU_INVALID,
    NV_FRAME_FIFO_DRAW_MS = 16,
    NV_FRAME_FIFO_DRAW_SAMPLES,
    NV_FRAME_FIFO_WORDS,
    NV_FRAME_TARGET_DRAIN_COUNT = 24,
    NV_FRAME_RAW_BOUNDS = 30,
    NV_FRAME_RAW_BOUNDS_VERTICES,
    NV_FRAME_DEPENDENCY = 32,
    NV_FRAME_RAW_BOUNDARY = NV_FRAME_DEPENDENCY + 6 * 4,
    NV_FRAME_DEPTH_CLEAR_RETIRED = NV_FRAME_RAW_BOUNDARY + 5,
    NV_FRAME_COMPLETION_MARKERS,
    NV_FRAME_COMPLETION_REUSED,
    NV_FRAME_COMPLETION_EARLY_RELEASES,
    NV_FRAME_COMPLETION_FORCED_MARKERS,
    NV_FRAME_METRICS
};

#if defined(XBOX_FRAME_TELEMETRY)
int nv2a_frame_profile_enabled(void);
void nv2a_frame_profile_add(unsigned metric, double value);
void nv2a_frame_profile_fifo_enter(double now);
void nv2a_frame_profile_fifo_leave(double now);
void nv2a_frame_profile_flip(uint32_t frame, uint32_t draws, double now);
uint32_t nv2a_frame_profile_next_id(void);
#else
static inline int nv2a_frame_profile_enabled(void) { return 0; }
static inline void nv2a_frame_profile_add(unsigned metric,double value)
{ (void)metric; (void)value; }
static inline void nv2a_frame_profile_fifo_enter(double now) { (void)now; }
static inline void nv2a_frame_profile_fifo_leave(double now) { (void)now; }
static inline void nv2a_frame_profile_flip(uint32_t frame,uint32_t draws,double now)
{ (void)frame; (void)draws; (void)now; }
static inline uint32_t nv2a_frame_profile_next_id(void) { return 0; }
#endif

#endif
