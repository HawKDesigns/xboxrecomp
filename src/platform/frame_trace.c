/* Fixed-size producer rings. Producers publish records without allocating,
 * formatting, waiting, or touching the stream. Only the writer owns the file. */
#if defined(_WIN32) || defined(XBOX_FRAME_TRACE_WIN32_SHIM)
#define TRACE_WINDOWS 1
#include <windows.h>
#else
/* POSIX hosts: the platform layer supplies the same Interlocked, thread and
 * sleep primitives (win32_compat.c), so the recorder below is one source. */
#include "win32_compat.h"
#endif
#include <stdio.h>
#include "frame_trace.h"
#include "host_time.h"

#define RING_SIZE 4096u
#define WRITE_BATCH 128u
#define TRACE_FOOTER 0xFFFFFFFFu

typedef struct {
    uint64_t timestamp_ns;
    uint32_t sequence, thread, event, producer, frame, arg0, arg1, arg2;
} TraceRecord;

_Static_assert(sizeof(TraceRecord) == 40, "frame trace record size");

typedef struct {
    volatile LONG write, read, writing, dropped;
    uint32_t sequence;
    TraceRecord records[RING_SIZE];
} TraceRing;

static TraceRing s_rings[XBOX_TRACE_PRODUCERS];
static volatile LONG s_state, s_guest_ticks, s_vblank_events;
enum { TRACE_OFF, TRACE_RUNNING, TRACE_STOPPING };
static uint64_t s_origin;
static HANDLE s_writer;
static volatile LONG s_thread_ids[XBOX_TRACE_THREAD_ROLES];
static volatile uint32_t *s_main_pc;

static LONG load(volatile LONG *value)
{
    return InterlockedCompareExchange(value, 0, 0);
}

int xbox_frame_trace_enabled(void)
{
    return load(&s_state) == TRACE_RUNNING;
}

void xbox_frame_trace_guest_ticks(uint32_t elapsed)
{
    if (elapsed && xbox_frame_trace_enabled())
        InterlockedExchangeAdd(&s_guest_ticks, (LONG)elapsed);
}

uint32_t xbox_frame_trace_guest_tick_count(void)
{
    return (uint32_t)load(&s_guest_ticks);
}

void xbox_frame_trace_thread(enum XboxTraceThread role)
{
    InterlockedExchange(&s_thread_ids[role], (LONG)GetCurrentThreadId());
}

void xbox_frame_trace_main_pc(volatile uint32_t *pc)
{
    s_main_pc = pc;
}

#if defined(TRACE_WINDOWS)
static void sample_threads(HANDLE *handles, DWORD *ids)
{
    for (unsigned role = 0; role < XBOX_TRACE_THREAD_ROLES; ++role) {
        DWORD id = (DWORD)load(&s_thread_ids[role]);
        if (id != ids[role]) {
            if (handles[role]) CloseHandle(handles[role]);
            handles[role] = id ? OpenThread(THREAD_QUERY_LIMITED_INFORMATION, FALSE, id) : NULL;
            ids[role] = handles[role] ? id : 0;
        }
        FILETIME creation, exit, kernel, user;
        if (!handles[role] || !GetThreadTimes(handles[role], &creation, &exit, &kernel, &user))
            continue;
        uint64_t total = (((uint64_t)kernel.dwHighDateTime << 32) | kernel.dwLowDateTime) +
                         (((uint64_t)user.dwHighDateTime << 32) | user.dwLowDateTime);
        xbox_frame_trace_note(XBOX_TRACE_TITLE, XBOX_TRACE_THREAD_CPU, role,
                              (uint32_t)total, (uint32_t)(total >> 32), id);
    }
}

#endif

void xbox_frame_trace_vblank_event(void)
{
    if (xbox_frame_trace_enabled())
        InterlockedIncrement(&s_vblank_events);
}

uint32_t xbox_frame_trace_vblank_event_count(void)
{
    return (uint32_t)load(&s_vblank_events);
}

void xbox_frame_trace_note(enum XboxTraceProducer producer, enum XboxTraceEvent event,
                           uint32_t frame, uint32_t arg0, uint32_t arg1, uint32_t arg2)
{
    if (!xbox_frame_trace_enabled())
        return;
    TraceRing *ring = &s_rings[producer];
    if (InterlockedCompareExchange(&ring->writing, 1, 0)) {
        InterlockedIncrement(&ring->dropped);
        return;
    }
    if (xbox_frame_trace_enabled()) {
        uint32_t write = (uint32_t)load(&ring->write);
        uint32_t sequence = ++ring->sequence;
        if (write - (uint32_t)load(&ring->read) >= RING_SIZE) {
            InterlockedIncrement(&ring->dropped);
        } else {
            TraceRecord record = {xbox_HostNowNs() - s_origin, sequence,
                GetCurrentThreadId(), event, producer, frame, arg0, arg1, arg2};
            ring->records[write % RING_SIZE] = record;
            InterlockedExchange(&ring->write, (LONG)(write + 1));
        }
    }
    InterlockedExchange(&ring->writing, 0);
}

static DWORD WINAPI trace_writer(LPVOID arg)
{
    FILE *file = arg;
    const uint32_t header[] = {0x52545051u, 4, sizeof(TraceRecord), XBOX_TRACE_PRODUCERS};
#if defined(TRACE_WINDOWS)
    HANDLE handles[XBOX_TRACE_THREAD_ROLES] = {0};
    DWORD ids[XBOX_TRACE_THREAD_ROLES] = {0};
    uint64_t next_cpu = 0;
#endif
    uint64_t next_pc = 0;
    if (fwrite(header, sizeof header, 1, file) != 1 || fflush(file))
        goto failed;
    for (;;) {
        int stopping = load(&s_state) == TRACE_STOPPING;
        int busy = 0, wrote = 0;
        if (!stopping) {
            uint64_t now = xbox_HostNowNs();
#if defined(TRACE_WINDOWS)
            if (now >= next_cpu) {
                sample_threads(handles, ids);
                next_cpu = now + 1000000000ull;
            }
#endif
            if (s_main_pc && now >= next_pc) {
                uint32_t pc = *s_main_pc;
                xbox_frame_trace_note(XBOX_TRACE_TITLE, XBOX_TRACE_GUEST_PC, 0, pc,
                                      (uint32_t)load(&s_thread_ids[XBOX_TRACE_GUEST_MAIN]), 0);
                next_pc = now + 50000000ull;
            }
        }
        for (unsigned i = 0; i < XBOX_TRACE_PRODUCERS; ++i) {
            TraceRing *ring = &s_rings[i];
            if (stopping && load(&ring->writing))
                busy = 1;
            uint32_t read = (uint32_t)load(&ring->read);
            uint32_t end = (uint32_t)load(&ring->write);
            while (read != end) {
                unsigned offset = read % RING_SIZE;
                uint32_t count = end - read;
                if (count > WRITE_BATCH) count = WRITE_BATCH;
                if (count > RING_SIZE - offset) count = RING_SIZE - offset;
                if (fwrite(ring->records + offset, sizeof(TraceRecord), count, file) != count)
                    goto failed;
                read += count;
                InterlockedExchange(&ring->read, (LONG)read);
                wrote = 1;
            }
        }
        if (wrote && fflush(file)) goto failed;
        if (stopping && !busy) break;
        Sleep(stopping ? 1 : 50);
    }
    for (unsigned i = 0; i < XBOX_TRACE_PRODUCERS; ++i) {
        TraceRecord footer = {xbox_HostNowNs() - s_origin, 0, 0, TRACE_FOOTER,
            i, 0, (uint32_t)load(&s_rings[i].dropped), 0, 0};
        if (fwrite(&footer, sizeof footer, 1, file) != 1) goto failed;
    }
#if defined(TRACE_WINDOWS)
    for (unsigned i = 0; i < XBOX_TRACE_THREAD_ROLES; ++i)
        if (handles[i]) CloseHandle(handles[i]);
#endif
    return fclose(file) ? 1 : 0;

failed:
    InterlockedExchange(&s_state, TRACE_OFF);
#if defined(TRACE_WINDOWS)
    for (unsigned i = 0; i < XBOX_TRACE_THREAD_ROLES; ++i)
        if (handles[i]) CloseHandle(handles[i]);
#endif
    fclose(file);
    return 1;
}

static int trace_start_file(FILE *file)
{
    if (!file) return 0;
    s_origin = xbox_HostNowNs();
    InterlockedExchange(&s_state, TRACE_RUNNING);
    s_writer = CreateThread(NULL, 0, trace_writer, file, 0, NULL);
    if (!s_writer) {
        InterlockedExchange(&s_state, TRACE_OFF);
        fclose(file);
        return 0;
    }
    return 1;
}

#if defined(_WIN32) || defined(XBOX_FRAME_TRACE_WIN32_SHIM)
int xbox_frame_trace_start(const wchar_t *path)
{
    if (!path || !*path || s_writer || load(&s_state))
        return 0;
    return trace_start_file(_wfopen(path, L"wb"));
}
#endif

int xbox_frame_trace_start_utf8(const char *path)
{
    if (!path || !*path || s_writer || load(&s_state))
        return 0;
    return trace_start_file(fopen(path, "wb"));
}

void xbox_frame_trace_stop(void)
{
    if (!s_writer) return;
    InterlockedExchange(&s_state, TRACE_STOPPING);
    WaitForSingleObject(s_writer, 250);
    /* The process owns the handle. On timeout the writer may still be draining
     * and the absent footer will identify an incomplete trace. */
}
