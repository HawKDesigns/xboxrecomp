/* See thread_cpu.h. One source for Win32 and POSIX hosts. */
#if !defined(_WIN32) && !defined(_GNU_SOURCE)
#define _GNU_SOURCE   /* pthread_getcpuclockid, clock_gettime under -std=c11 */
#endif
#ifndef XBOX_THREAD_CPU
#define XBOX_THREAD_CPU 1   /* this file is the implementation */
#endif
#include "thread_cpu.h"
#include "host_time.h"
#include <stdint.h>
#include <string.h>

#if defined(_WIN32)
#include <windows.h>
typedef HANDLE cpu_ref;
#define TLS_FLAG __declspec(thread)
static SRWLOCK s_lock = SRWLOCK_INIT;
static void lock_threads(void) { AcquireSRWLockExclusive(&s_lock); }
static void unlock_threads(void) { ReleaseSRWLockExclusive(&s_lock); }
#else
#include <pthread.h>
#include <sys/resource.h>
#include <time.h>
typedef clockid_t cpu_ref;
#define TLS_FLAG __thread
static pthread_mutex_t s_lock = PTHREAD_MUTEX_INITIALIZER;
static void lock_threads(void) { pthread_mutex_lock(&s_lock); }
static void unlock_threads(void) { pthread_mutex_unlock(&s_lock); }
#endif

#define MAX_THREADS 48

struct tracked {
    const char *role;
    cpu_ref     ref;
    uint64_t    last_ns;      /* CPU time at the previous report */
    int         ordinal;      /* nth thread with this role, from 1 */
    int         live;
};

static struct tracked s_threads[MAX_THREADS];
static int s_count, s_overflow;
static uint64_t s_last_wall_ns, s_last_process_ns;
static TLS_FLAG int s_slot = -1;

/* 0 when the thread's clock can no longer be read. */
static int thread_cpu_ns(cpu_ref ref, uint64_t *ns)
{
#if defined(_WIN32)
    FILETIME created, exited, kernel, user;
    ULARGE_INTEGER k, u;
    if (!GetThreadTimes(ref, &created, &exited, &kernel, &user))
        return 0;
    k.LowPart = kernel.dwLowDateTime; k.HighPart = kernel.dwHighDateTime;
    u.LowPart = user.dwLowDateTime;   u.HighPart = user.dwHighDateTime;
    *ns = (k.QuadPart + u.QuadPart) * 100u;
    return 1;
#else
    struct timespec ts;
    if (clock_gettime(ref, &ts) != 0)
        return 0;
    *ns = (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
    return 1;
#endif
}

static uint64_t process_cpu_ns(void)
{
#if defined(_WIN32)
    FILETIME created, exited, kernel, user;
    ULARGE_INTEGER k, u;
    if (!GetProcessTimes(GetCurrentProcess(), &created, &exited, &kernel, &user))
        return 0;
    k.LowPart = kernel.dwLowDateTime; k.HighPart = kernel.dwHighDateTime;
    u.LowPart = user.dwLowDateTime;   u.HighPart = user.dwHighDateTime;
    return (k.QuadPart + u.QuadPart) * 100u;
#else
    struct timespec ts;
    if (clock_gettime(CLOCK_PROCESS_CPUTIME_ID, &ts) != 0)
        return 0;
    return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
#endif
}

void xbox_ThreadCpuRegister(const char *role)
{
    struct tracked *t;
    int index, i, ordinal = 1;

    if (s_slot >= 0 || !role)
        return;
    lock_threads();
    if (s_count == MAX_THREADS) {
        ++s_overflow;
        s_slot = MAX_THREADS;          /* registered, untracked */
        unlock_threads();
        return;
    }
    index = s_count++;
    t = &s_threads[index];
#if defined(_WIN32)
    if (!DuplicateHandle(GetCurrentProcess(), GetCurrentThread(), GetCurrentProcess(),
                         &t->ref, THREAD_QUERY_LIMITED_INFORMATION, FALSE, 0))
        t->ref = NULL;
#else
    if (pthread_getcpuclockid(pthread_self(), &t->ref) != 0)
        t->ref = (clockid_t)-1;
#endif
    for (i = 0; i < index; i++)
        if (s_threads[i].role && !strcmp(s_threads[i].role, role))
            ordinal++;
    t->role = role;
    t->ordinal = ordinal;
    t->last_ns = 0;
    thread_cpu_ns(t->ref, &t->last_ns);
    s_slot = index;
    t->live = 1;
    unlock_threads();
}

void xbox_ThreadCpuUnregister(void)
{
    if (s_slot < 0 || s_slot >= MAX_THREADS) return;
    lock_threads();
    struct tracked *t = &s_threads[s_slot];
#if defined(_WIN32)
    if (t->ref) CloseHandle(t->ref);
    t->ref = NULL;
#endif
    t->live = 0;
    unlock_threads();
}

void xbox_ThreadCpuReport(FILE *out)
{
    lock_threads();
    uint64_t wall = xbox_HostNowNs(), process = process_cpu_ns();
    uint64_t named_ns = 0;
    double window_ms;
    int i, count = s_count;

    if (!s_last_wall_ns) {             /* first call sets the baseline */
        s_last_wall_ns = wall;
        s_last_process_ns = process;
        for (i = 0; i < count; i++)
            if (s_threads[i].live)
                thread_cpu_ns(s_threads[i].ref, &s_threads[i].last_ns);
        unlock_threads();
        return;
    }
    window_ms = (double)(wall - s_last_wall_ns) / 1e6;
    if (window_ms <= 0.0) {
        unlock_threads();
        return;
    }
    for (i = 0; i < count; i++) {
        struct tracked *t = &s_threads[i];
        uint64_t now_ns, used;
        if (!t->live)
            continue;
        if (!thread_cpu_ns(t->ref, &now_ns)) {
            t->live = 0;               /* ended without unregistering */
            continue;
        }
        used = now_ns >= t->last_ns ? now_ns - t->last_ns : 0;
        t->last_ns = now_ns;
        named_ns += used;
        if (!used)
            continue;
        fprintf(out, "[THREAD-CPU] role=%s ordinal=%d cpu_ms=%.3f window_ms=%.3f busy_pct=%.1f\n",
                t->role, t->ordinal, (double)used / 1e6, window_ms,
                (double)used / 1e4 / window_ms);
    }
    {
        uint64_t used = process >= s_last_process_ns ? process - s_last_process_ns : 0;
        uint64_t other = used >= named_ns ? used - named_ns : 0;
        fprintf(out, "[THREAD-CPU] role=process cpu_ms=%.3f window_ms=%.3f busy_pct=%.1f "
                     "named_ms=%.3f unnamed_ms=%.3f tracked=%d untracked=%d\n",
                (double)used / 1e6, window_ms, (double)used / 1e4 / window_ms,
                (double)named_ns / 1e6, (double)other / 1e6, count, (int)s_overflow);
    }
    s_last_wall_ns = wall;
    s_last_process_ns = process;
    unlock_threads();
}
