/*
 * thread_cpu.h - CPU time per named runtime thread, for the periodic report.
 *
 * The 5-second renderer report says where wall time goes inside the FIFO
 * executor, but not which thread is saturated. A thread that names itself
 * here gets one [THREAD-CPU] line per report with the CPU time it consumed
 * since the previous report, as milliseconds and as a share of one core.
 * A final line gives the whole process and the remainder that belongs to
 * unnamed threads (graphics driver workers, audio callbacks, OS helpers).
 *
 * Registration takes a lock and looks up the thread clock once.
 * The periodic report costs one time query per registered
 * thread. Nothing here touches guest state.
 */
#ifndef XBOX_THREAD_CPU_H
#define XBOX_THREAD_CPU_H

#include <stdio.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Built into host_time, which defines XBOX_THREAD_CPU for everything that
 * links it. A source compiled on its own (the fixtures that include a
 * runtime .c file directly) gets no-ops, as with XBOX_FRAME_TELEMETRY. */
#if defined(XBOX_THREAD_CPU)

/* Name the calling thread. `role` must outlive the process (a literal).
 * Calling again from the same thread is a no-op. At most 48 threads are
 * tracked; later ones are counted as unnamed. */
void xbox_ThreadCpuRegister(const char *role);

/* The calling thread is about to end: stop reporting it. Optional -- a
 * thread that ends without it is dropped at the first failed time query
 * (POSIX) or reports no further CPU time (Windows). */
void xbox_ThreadCpuUnregister(void);

/* Print one line per registered thread that used CPU since the last call,
 * then the process line. Call from one thread at a time. */
void xbox_ThreadCpuReport(FILE *out);

#else
static inline void xbox_ThreadCpuRegister(const char *role) { (void)role; }
static inline void xbox_ThreadCpuUnregister(void) {}
static inline void xbox_ThreadCpuReport(FILE *out) { (void)out; }
#endif

#ifdef __cplusplus
}
#endif

#endif
