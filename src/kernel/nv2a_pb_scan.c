/*
 * Read-only survey of the pushbuffer a title submits.
 *
 * The title builds NV2A commands in guest RAM and advances DMA_PUT; nothing
 * here executes them, so the framebuffer stays black however far the game
 * gets. Before any of that can be made to draw, the question is what it
 * actually asks for -- which methods, on which object classes, how many of
 * them -- because that is the difference between "the existing PGRAPH
 * translator nearly covers this" and "this needs a real one".
 *
 * Purely a reader: it walks the buffer and counts, and never writes to guest
 * memory or to the GPU state. Enabled with RECOMP_PB_SCAN.
 *
 * Pushbuffer encoding (NV20/NV2A), one dword per command header:
 *   (w & 0xE0030003) == 0x00000000  increasing methods
 *   (w & 0xE0030003) == 0x40000000  non-increasing (same method, count params)
 *   (w & 0x00000003) == 0x00000001  jump
 *   (w & 0x00000003) == 0x00000002  call
 *   (w & 0xFFFF0003) == 0x00020000  return
 * For a method header: count = (w >> 18) & 0x7FF, subchannel = (w >> 13) & 7,
 * method = w & 0x1FFC.
 */
#include <stdio.h>
#include <stdint.h>
#include <stddef.h>   /* ptrdiff_t */
#include <stdlib.h>
#include <string.h>
#include <time.h>
#if defined(_WIN32)
#include <windows.h>
#endif
#include "nv2a_fifo.h"

#ifndef XBOX_CONTIG_BASE
#define XBOX_CONTIG_BASE 0x80000000u   /* physical P is at this + P */
#endif

extern ptrdiff_t xbox_GetMemoryOffset(void);

#define PB_MAX_METHODS 4096

static struct { uint32_t method, subch, count; } s_seen[PB_MAX_METHODS];
static int s_seen_count;
/* Method packets carry thousands of parameters. Counting each parameter must
 * not search the growing diagnostic inventory. Preserve its insertion order. */
static uint16_t s_seen_index[8][2048];
static int s_inventory_enabled=-1;

/* Parse health. An inventory is only worth reading if the walk stayed in step
 * with the command stream: a decoder that desynchronises produces plausible
 * looking method numbers out of parameter data, and the counts then describe
 * nothing. Unrecognised words are the tell. */
static uint32_t s_tot_words, s_tot_unknown, s_tot_jumps, s_tot_segments;
static uint32_t s_tot_calls;

/* Executing is opt-in separately from surveying: a survey is read-only, while
 * the executor writes to guest memory. */
extern void nv2a_pb_exec_method(uint32_t subch, uint32_t method, uint32_t param);
extern void nv2a_pb_exec_report(void);
extern uint32_t nv2a_pb_exec_fifo_begin(void);
extern void nv2a_pb_exec_fifo_end(void);
extern void nv2a_pb_exec_fifo_end_continue(int);
extern void nv2a_pb_exec_fifo_idle(void);
extern void nv2a_pb_exec_guest_access(uint64_t,uint64_t);
static int s_exec_enabled = -1;
static int s_fifo_timing=-1;
static double s_fifo_ms;
static uint64_t s_fifo_parameters,s_fifo_words,s_fifo_polls;
typedef struct {uint32_t rng;uint64_t calls,samples;double sample_ms,peak_ms;} FifoCpuProbe;
static FifoCpuProbe s_fifo_read_probe,s_fifo_emit_probe;
static double pb_wall_ms(void)
{
#if defined(_WIN32)
    LARGE_INTEGER now,frequency;QueryPerformanceCounter(&now);QueryPerformanceFrequency(&frequency);
    return now.QuadPart*1000.0/frequency.QuadPart;
#else
    struct timespec now;timespec_get(&now,TIME_UTC);return now.tv_sec*1000.0+now.tv_nsec/1e6;
#endif
}

static int fifo_cpu_probe_begin(FifoCpuProbe *p,double *begin)
{
    ++p->calls;
    if(s_fifo_timing<=0)return 0;
    uint32_t x=p->rng;if(!x)x=0xc18b49e7u;
    x^=x<<13;x^=x>>17;x^=x<<5;p->rng=x;
    if(x&63u)return 0;
    *begin=pb_wall_ms();return 1;
}
static void fifo_cpu_probe_end(FifoCpuProbe *p,double begin)
{
    double elapsed=pb_wall_ms()-begin;
    ++p->samples;p->sample_ms+=elapsed;
    if(elapsed>p->peak_ms)p->peak_ms=elapsed;
}
static void note(uint32_t subch, uint32_t method)
{
    if(s_inventory_enabled<0)s_inventory_enabled=getenv("RECOMP_PB_SCAN")!=NULL;
    if(!s_inventory_enabled)return; /* Detailed inventory is opt-in. */
    int direct=subch<8 && method<0x2000 && !(method&3);
    if(direct) {
        unsigned entry=s_seen_index[subch][method/4];
        if(entry){++s_seen[entry-1].count;return;}
    } else for (int i = 0; i < s_seen_count; i++) {
        if (s_seen[i].method == method && s_seen[i].subch == subch) {
            s_seen[i].count++;
            return;
        }
    }
    if (s_seen_count >= PB_MAX_METHODS) {
        /* Silently dropping past the cap is how a truncated inventory reads as
         * "the title never does that" -- exactly the wrong conclusion when the
         * inventory is being used to decide what to implement. */
        static int warned;
        if (!warned) {
            warned = 1;
            fprintf(stderr, "[PB] method table full at %d -- inventory is"
                            " truncated\n", PB_MAX_METHODS);
        }
    }
    if (s_seen_count < PB_MAX_METHODS) {
        s_seen[s_seen_count].method = method;
        s_seen[s_seen_count].subch  = subch;
        s_seen[s_seen_count].count  = 1;
        if(direct)s_seen_index[subch][method/4]=(uint16_t)(s_seen_count+1);
        s_seen_count++;
    }
}

/* NV097 (Kelvin 3D class) methods worth naming. The point of the survey is to
 * decide what a translator has to implement, and a bare method number does not
 * answer that -- "0x1808 x412" only means something once it reads
 * INLINE_ARRAY. Unnamed ones still get counted. */
static const struct { uint32_t m; const char *name; } NV097_NAMES[] = {
    { 0x0000, "SET_OBJECT" },
    { 0x0100, "NO_OPERATION" },
    { 0x0104, "SET_WARNING_ENABLE" },
    { 0x0130, "SET_FLIP_READ" },
    { 0x0200, "SET_SURFACE_CLIP_HORIZONTAL" },
    { 0x0204, "SET_SURFACE_CLIP_VERTICAL" },
    { 0x0208, "SET_SURFACE_FORMAT" },
    { 0x020C, "SET_SURFACE_PITCH" },
    { 0x0210, "SET_SURFACE_COLOR_OFFSET" },
    { 0x0214, "SET_SURFACE_ZETA_OFFSET" },
    { 0x0300, "SET_ALPHA_TEST_ENABLE" },
    { 0x0304, "SET_BLEND_ENABLE" },
    { 0x030C, "SET_DEPTH_TEST_ENABLE" },
    { 0x0310, "SET_DITHER_ENABLE" },
    { 0x0314, "SET_LIGHTING_ENABLE" },
    { 0x033C, "SET_CULL_FACE_ENABLE" },
    { 0x0340, "SET_DEPTH_MASK" },
    { 0x0350, "SET_CLEAR_DEPTH_VALUE" },
    { 0x1D8C, "SET_CLEAR_DEPTH" },
    { 0x1D90, "SET_COLOR_CLEAR_VALUE" },
    { 0x1D94, "CLEAR_SURFACE" },
    { 0x1D6C, "SET_ZSTENCIL_CLEAR" },
    { 0x0B80, "SET_TRANSFORM_PROGRAM" },
    { 0x0B00, "SET_TRANSFORM_CONSTANT" },
    { 0x1720, "SET_VERTEX_DATA_ARRAY_OFFSET" },
    { 0x1760, "SET_VERTEX_DATA_ARRAY_FORMAT" },
    { 0x17FC, "SET_BEGIN_END" },
    { 0x1800, "ARRAY_ELEMENT16" },
    { 0x1808, "INLINE_ARRAY" },
    { 0x1810, "DRAW_ARRAYS" },
    { 0x1B00, "SET_TEXTURE_OFFSET" },
    { 0x1B04, "SET_TEXTURE_FORMAT" },
    { 0x1B08, "SET_TEXTURE_ADDRESS" },
    { 0x1B0C, "SET_TEXTURE_CONTROL0" },
    { 0x1B14, "SET_TEXTURE_IMAGE_RECT" },
    { 0x0FD8, "SET_COMBINER_*" },
    { 0x0000, NULL },
};

static const char *nv097_name(uint32_t m)
{
    int i;
    for (i = 0; NV097_NAMES[i].name; i++)
        if (NV097_NAMES[i].m == m)
            return NV097_NAMES[i].name;
    return "";
}

void nv2a_pb_scan_report(void)
{
    int i;

    if (s_exec_enabled > 0) {
        if(s_fifo_timing>0)fprintf(stderr,"[GPU-FIFO] execute_ms=%.3f words=%llu parameters=%llu polls=%llu inventory=%d lookup=%s\n",
            s_fifo_ms,(unsigned long long)s_fifo_words,(unsigned long long)s_fifo_parameters,(unsigned long long)s_fifo_polls,s_seen_count,s_inventory_enabled>0?"direct":"disabled");
        double read_est=s_fifo_read_probe.samples?
            s_fifo_read_probe.sample_ms*(double)s_fifo_read_probe.calls/s_fifo_read_probe.samples:0;
        double emit_est=s_fifo_emit_probe.samples?
            s_fifo_emit_probe.sample_ms*(double)s_fifo_emit_probe.calls/s_fifo_emit_probe.samples:0;
        fprintf(stderr,"[GPU-FIFO-CPU] reads=%llu read_samples=%llu read_sample_ms=%.6f read_est_ms=%.3f read_peak_ms=%.6f emits=%llu emit_samples=%llu emit_sample_ms=%.6f emit_est_ms=%.3f emit_peak_ms=%.6f decode_residual_est_ms=%.3f sample_rate=64\n",
            (unsigned long long)s_fifo_read_probe.calls,(unsigned long long)s_fifo_read_probe.samples,
            s_fifo_read_probe.sample_ms,read_est,s_fifo_read_probe.peak_ms,
            (unsigned long long)s_fifo_emit_probe.calls,(unsigned long long)s_fifo_emit_probe.samples,
            s_fifo_emit_probe.sample_ms,emit_est,s_fifo_emit_probe.peak_ms,
            s_fifo_ms-read_est-emit_est);
        uint32_t read_rng=s_fifo_read_probe.rng,emit_rng=s_fifo_emit_probe.rng;
        memset(&s_fifo_read_probe,0,sizeof s_fifo_read_probe);
        memset(&s_fifo_emit_probe,0,sizeof s_fifo_emit_probe);
        s_fifo_read_probe.rng=read_rng;s_fifo_emit_probe.rng=emit_rng;
        s_fifo_ms=0;s_fifo_words=s_fifo_parameters=s_fifo_polls=0;
        nv2a_pb_exec_report();
        fprintf(stderr, "[PB] %u pushbuffer calls followed\n", s_tot_calls);
    }
    if (!s_seen_count || !getenv("RECOMP_PB_SCAN"))
        return;
    fprintf(stderr, "[PB] %u segments, %u words, %u jumps, %u calls,"
                    " %u unrecognised -- %d distinct (subchannel, method)"
                    " pairs\n",
            s_tot_segments, s_tot_words, s_tot_jumps, s_tot_calls,
            s_tot_unknown, s_seen_count);
    for (i = 0; i < s_seen_count; i++)
        fprintf(stderr, "  [PB]   subch %u  method 0x%04X  x%-6u %s\n",
                s_seen[i].subch, s_seen[i].method, s_seen[i].count,
                nv097_name(s_seen[i].method));
    fflush(stderr);
}

/* Walk commands from va to end_va. Returns the words consumed.
 *
 * A CALL runs a subroutine -- a pushbuffer somewhere else in memory -- up to
 * its RETURN, then carries on after the CALL. This used to skip CALLs, which
 * was invisible until a title used them for real: RenderWare on the Xbox
 * compiles static geometry into pushbuffers and draws it with a CALL, so the
 * whole race track went missing from Burnout 3's main view while the
 * geometry it submits inline (environment maps, the HUD) drew fine.
 * The NV2A has one level of subroutine, so a CALL inside a call is not
 * followed. Addresses are physical, like PUT's, and reach guest memory the
 * same way: through the contiguous window. */
static uint32_t pb_walk(uint32_t va, uint32_t end_va, int in_call,
                        uint32_t *jumps, uint32_t *unknown)
{
    const uint8_t *mem = (const uint8_t *)xbox_GetMemoryOffset();
    uint32_t words = 0;

    while (va < end_va && words < 0x100000u) {
        uint32_t w = *(const uint32_t *)(mem + va);
        va += 4;
        words++;

        if ((w & 3u) == 1u || (w & 0xE0000003u) == 0x20000000u) {
            uint32_t target = (w & 3u) == 1u ? (w & 0xFFFFFFFCu)
                                             : (w & 0x1FFFFFFCu);
            (*jumps)++;
            if (!in_call)
                break;                        /* the ring: a jump ends it */
            va = XBOX_CONTIG_BASE | (target & 0x0FFFFFFFu);
            end_va = va + 0x400000u;
            continue;
        }
        if ((w & 0xFFFF0003u) == 0x00020000u) {  /* RETURN */
            if (in_call)
                break;
            continue;
        }
        if ((w & 3u) == 2u) {                    /* CALL */
            if (!in_call) {
                uint32_t sub = XBOX_CONTIG_BASE | ((w & 0xFFFFFFFCu) & 0x0FFFFFFFu);
                s_tot_calls++;
                words += pb_walk(sub, sub + 0x400000u, 1, jumps, unknown);
            }
            continue;
        }
        if ((w & 0x00030003u) == 0u) {
            uint32_t count  = (w >> 18) & 0x7FFu;
            uint32_t subch  = (w >> 13) & 7u;
            uint32_t method =  w & 0x1FFCu;
            int noninc = (w & 0xE0000000u) == 0x40000000u;

            for (uint32_t i = 0; i < count && va < end_va; i++) {
                uint32_t m = noninc ? method : method + i * 4;
                note(subch, m);
                /* Same walk, two consumers: the survey counts, the executor
                 * acts. Keeping them on one decode means they can never
                 * disagree about what the stream said. */
                if (s_exec_enabled)
                    nv2a_pb_exec_method(subch, m,
                                        *(const uint32_t *)(mem + va));
                va += 4;
                words++;
            }
            continue;
        }
        (*unknown)++;
    }
    return words;
}

void nv2a_pb_scan(uint32_t start_va, uint32_t end_va)
{
    uint32_t words, jumps = 0, unknown = 0;

    if (s_exec_enabled < 0)
        s_exec_enabled = getenv("RECOMP_PB_EXEC") != NULL;
    static int scan = -1;
    if (scan < 0)
        scan = getenv("RECOMP_PB_SCAN") != NULL;
    if (!(scan || s_exec_enabled) || end_va <= start_va)
        return;
    if (end_va - start_va > 0x400000u)        /* a sane single-frame bound */
        end_va = start_va + 0x400000u;

    words = pb_walk(start_va, end_va, 0, &jumps, &unknown);

    s_tot_words += words;
    s_tot_unknown += unknown;
    s_tot_jumps += jumps;
    s_tot_segments++;
}

/* Executed FIFO cursor; unlike a survey range this follows actual control
 * flow and retains packet parameters across host polls/budget yields. */
static Nv2aFifo s_fifo;
static int fifo_read(uint32_t address,uint32_t *word)
{
    extern int xbox_Nv2aFifoRead(uint32_t,uint32_t*);
    /* A command stream can alias a deferred semaphore destination too. */
    double begin=0;int sample=fifo_cpu_probe_begin(&s_fifo_read_probe,&begin);
    nv2a_pb_exec_guest_access(address,4);
    int ok=xbox_Nv2aFifoRead(address,word);
    if(sample)fifo_cpu_probe_end(&s_fifo_read_probe,begin);
    return ok;
}
static void fifo_emit(uint32_t subch,uint32_t method,uint32_t param)
{
    ++s_fifo_parameters;
    double begin=0;int sample=fifo_cpu_probe_begin(&s_fifo_emit_probe,&begin);
    note(subch,method);nv2a_pb_exec_method(subch,method,param);
    if(sample)fifo_cpu_probe_end(&s_fifo_emit_probe,begin);
}
uint32_t nv2a_pb_execute_fifo(uint32_t get,uint32_t put,uint32_t lo,uint32_t hi)
{
    if(s_fifo_timing<0){const char *e=getenv("RECOMP_RENDER_TIMING");s_fifo_timing=e && atoi(e)>0;}
    double begin=s_fifo_timing?pb_wall_ms():0;
    uint32_t words=s_fifo.words,jumps=s_fifo.jumps,calls=s_fifo.calls;
    uint32_t budget=nv2a_pb_exec_fifo_begin();
    uint32_t cursor=nv2a_fifo_run(&s_fifo,get,put,lo,hi,fifo_read,fifo_emit,budget);
    /* Only a clean continuation may share its completion wait with the next
     * bounded slice. Empty streams, partial PUTs and decoder errors drain. */
    nv2a_pb_exec_fifo_end_continue(!s_fifo.error && cursor!=put);
    if(s_fifo.words>=words)s_tot_words+=s_fifo.words-words;
    if(s_fifo.words>=words)s_fifo_words+=s_fifo.words-words;
    if(s_fifo.jumps>=jumps)s_tot_jumps+=s_fifo.jumps-jumps;
    if(s_fifo.calls>=calls)s_tot_calls+=s_fifo.calls-calls;
    ++s_tot_segments;s_exec_enabled=1;
    if(s_fifo.error) {
        static uint32_t last_bad=~0u;
        if(last_bad!=cursor) {
            last_bad=cursor;++s_tot_unknown;
            fprintf(stderr,"[PB-FIFO] stopped at=%08X word=%08X put=%08X remaining=%u\n",
                cursor,s_fifo.error_word,put,s_fifo.remaining);
        }
    }
    if(s_fifo_timing){
        double elapsed=pb_wall_ms()-begin;s_fifo_ms+=elapsed;++s_fifo_polls;
        static unsigned slow;
        if(elapsed>=50 && slow++<32)fprintf(stderr,"[GPU-FIFO-SLOW] execute_ms=%.3f words=%u budget=%u cursor=%08X put=%08X remaining=%u method=%04X\n",
            elapsed,s_fifo.words>=words?s_fifo.words-words:0,budget,cursor,put,s_fifo.remaining,s_fifo.method);
    }
    return cursor;
}
uint32_t nv2a_pb_fifo_subroutine(void)
{ return s_fifo.in_call?(s_fifo.return_va&0x0FFFFFFFu)|1u:0; }

/* Called by the owning ACK worker if PUT stops at GET or ring setup vanishes. */
void nv2a_pb_fifo_idle(void){nv2a_pb_exec_fifo_idle();}
