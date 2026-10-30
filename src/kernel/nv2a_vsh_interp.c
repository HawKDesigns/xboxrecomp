/**
 * NV2A vertex program interpreter. See nv2a_vsh_interp.h for why it exists.
 *
 * Encoding. An instruction is four dwords; dword 0 is unused. Fields, as
 * (dword, lowest bit, width):
 *
 *   ILU op        1 25 3      MAC op        1 21 4
 *   const index   1 13 8      input index   1  9 4   (shared by A, B, C)
 *   A negate      1  8 1      A swizzle     1  0 8   (x y z w, 2 bits each,
 *   A temp        2 28 4      A mux         2 26 2    x highest)
 *   B negate      2 25 1      B swizzle     2 17 8
 *   B temp        2 13 4      B mux         2 11 2
 *   C negate      2 10 1      C swizzle     2  2 8
 *   C temp        2 0 2 (high) : 3 30 2 (low)          C mux 3 28 2
 *   MAC temp mask 3 24 4      temp dest     3 20 4
 *   ILU temp mask 3 16 4      out mask      3 12 4
 *   out to o[]    3 11 1      out address   3  3 8   (0: to c[])
 *   out from ILU  3  2 1      a0.x relative 3  1 1   final 3 0 1
 *
 * Masks are x=8 y=4 z=2 w=1. A source mux of 1 reads a temp, 2 an input,
 * 3 a constant. Temp 12 is oPos. When an instruction carries both a MAC and
 * an ILU op, the ILU's temp write goes to R1 whatever the temp field says,
 * and both units read their sources before either writes.
 *
 * The layout is checked against real programs rather than taken on trust:
 * RECOMP_VSH_DUMP disassembles each program the title uploads, and an Xbox
 * D3D program is recognisable -- DP4s against the matrix constants, ending in
 * the runtime's screen-space epilogue (RCC of R12.w, then a MAD into oPos).
 */
#include "nv2a_vsh_interp.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static uint32_t s_program[NV2A_VSH_SLOTS][4];
static float    s_const[NV2A_VSH_CONSTANTS][4];
static uint32_t s_load_slot, s_load_word, s_start_slot;
static uint32_t s_const_load, s_const_word;
static uint32_t s_cxt_write;
/* Program writes are visible a dword at a time. Invalidate before the next
 * execution whenever even an unused/partial instruction word changes. Live
 * constant writes deliberately do not invalidate executable code. */
static int s_native_dirty = 1;

static uint32_t field(const uint32_t *ins, int dw, int lo, int width)
{
    return (ins[dw] >> lo) & ((1u << width) - 1u);
}

/* ---- uploads ------------------------------------------------------------ */

void nv2a_vsh_set_load_slot(uint32_t slot)
{
    s_load_slot = slot;
    s_load_word = 0;
}

void nv2a_vsh_program_word(uint32_t word)
{
    if (s_load_slot < NV2A_VSH_SLOTS) {
        if (s_program[s_load_slot][s_load_word] != word) s_native_dirty = 1;
        s_program[s_load_slot][s_load_word] = word;
    }
    if (++s_load_word == 4) {
        s_load_word = 0;
        s_load_slot++;
    }
}

void nv2a_vsh_set_start_slot(uint32_t slot) {
    if (s_start_slot != slot) s_native_dirty = 1;
    s_start_slot = slot;
}
void nv2a_vsh_set_cxt_write(uint32_t enable) { s_cxt_write = enable; }
int nv2a_vsh_cacheable(void) { return s_cxt_write==0; }

void nv2a_vsh_set_constant_load(uint32_t index)
{
    s_const_load = index;
    s_const_word = 0;
}

void nv2a_vsh_constant_word(uint32_t word)
{
    if (s_const_load < NV2A_VSH_CONSTANTS)
        memcpy(&s_const[s_const_load][s_const_word], &word, 4);
    if (++s_const_word == 4) {
        s_const_word = 0;
        s_const_load++;
    }
}

void nv2a_vsh_set_constant(uint32_t index, const float v[4])
{
    if (index < NV2A_VSH_CONSTANTS)
        memcpy(s_const[index], v, sizeof s_const[index]);
}

void nv2a_vsh_set_instruction(uint32_t slot, const uint32_t words[4])
{
    if (slot < NV2A_VSH_SLOTS) {
        if (memcmp(s_program[slot], words, sizeof s_program[slot])) s_native_dirty = 1;
        memcpy(s_program[slot], words, sizeof s_program[slot]);
    }
}

/* ---- disassembly (RECOMP_VSH_DUMP) --------------------------------------- */

static const char *const k_mac[16] = {
    "nop", "mov", "mul", "add", "mad", "dp3", "dph", "dp4",
    "dst", "min", "max", "slt", "sge", "arl", "mac14", "mac15"
};
static const char *const k_ilu[8] = {
    "nop", "mov", "rcp", "rcc", "rsq", "exp", "log", "lit"
};

static void dis_src(char *b, size_t n, const uint32_t *ins, int which)
{
    static const char sw[] = "xyzw";
    uint32_t neg, swz, mux, reg;

    if (which == 0) {
        neg = field(ins, 1, 8, 1); swz = field(ins, 1, 0, 8);
        mux = field(ins, 2, 26, 2); reg = field(ins, 2, 28, 4);
    } else if (which == 1) {
        neg = field(ins, 2, 25, 1); swz = field(ins, 2, 17, 8);
        mux = field(ins, 2, 11, 2); reg = field(ins, 2, 13, 4);
    } else {
        neg = field(ins, 2, 10, 1); swz = field(ins, 2, 2, 8);
        mux = field(ins, 3, 28, 2);
        reg = (field(ins, 2, 0, 2) << 2) | field(ins, 3, 30, 2);
    }
    if (mux == 1)
        snprintf(b, n, "%sR%u", neg ? "-" : "", reg);
    else if (mux == 2)
        snprintf(b, n, "%sv%u", neg ? "-" : "", field(ins, 1, 9, 4));
    else
        snprintf(b, n, "%sc[%s%u]", neg ? "-" : "",
                 field(ins, 3, 1, 1) ? "a0.x+" : "", field(ins, 1, 13, 8));
    {
        size_t l = strlen(b);
        if (l + 6 < n)
            snprintf(b + l, n - l, ".%c%c%c%c", sw[(swz >> 6) & 3],
                     sw[(swz >> 4) & 3], sw[(swz >> 2) & 3], sw[swz & 3]);
    }
}

static void dump_program(uint32_t start)
{
    uint32_t s;
    fprintf(stderr, "  [VSH] program at slot %u:\n", start);
    for (s = start; s < NV2A_VSH_SLOTS; s++) {
        const uint32_t *ins = s_program[s];
        char a[40], bb[40], c[40];
        dis_src(a, sizeof a, ins, 0);
        dis_src(bb, sizeof bb, ins, 1);
        dis_src(c, sizeof c, ins, 2);
        fprintf(stderr, "  [VSH] %3u %-4s %-4s A=%s B=%s C=%s  R%u mac%X ilu%X"
                " out%s%u m%X%s%s\n", s,
                k_mac[field(ins, 1, 21, 4)], k_ilu[field(ins, 1, 25, 3)],
                a, bb, c, field(ins, 3, 20, 4), field(ins, 3, 24, 4),
                field(ins, 3, 16, 4), field(ins, 3, 11, 1) ? "o" : "c",
                field(ins, 3, 3, 8), field(ins, 3, 12, 4),
                field(ins, 3, 2, 1) ? " ilu" : " mac",
                field(ins, 3, 0, 1) ? " FINAL" : "");
        if (field(ins, 3, 0, 1))
            break;
    }
    fflush(stderr);
}

void nv2a_vsh_trace_state(void)
{
    uint32_t i;
    fprintf(stderr, "[VSTATE] start=%u context_write=%u\n", s_start_slot, s_cxt_write);
    dump_program(s_start_slot);
    for (i = s_start_slot; i < NV2A_VSH_SLOTS; ++i) {
        fprintf(stderr, "[VSTATE] instruction[%u]=%08X %08X %08X %08X\n", i,
            s_program[i][0], s_program[i][1], s_program[i][2], s_program[i][3]);
        if (field(s_program[i], 3, 0, 1)) break;
    }
    for (i = 0; i < NV2A_VSH_CONSTANTS; ++i)
        fprintf(stderr, "[VSTATE] c[%u]=%.9g %.9g %.9g %.9g\n", i,
            s_const[i][0], s_const[i][1], s_const[i][2], s_const[i][3]);
}

const float *nv2a_vsh_constant(uint32_t index)
{
    return s_const[index < NV2A_VSH_CONSTANTS ? index : 0];
}

void nv2a_vsh_constant_component(uint32_t index, uint32_t comp, uint32_t word)
{
    if (index < NV2A_VSH_CONSTANTS && comp < 4)
        memcpy(&s_const[index][comp], &word, 4);
}

/* ---- execution ----------------------------------------------------------- */

typedef struct { float v[4]; } vec4;

static vec4 read_src(const uint32_t *ins, int which,
                     const float in[NV2A_VSH_INPUTS][4],
                     const vec4 *temp, const vec4 *opos, int a0)
{
    uint32_t neg, swz, mux, reg;
    const float *s;
    vec4 r;

    if (which == 0) {
        neg = field(ins, 1, 8, 1); swz = field(ins, 1, 0, 8);
        mux = field(ins, 2, 26, 2); reg = field(ins, 2, 28, 4);
    } else if (which == 1) {
        neg = field(ins, 2, 25, 1); swz = field(ins, 2, 17, 8);
        mux = field(ins, 2, 11, 2); reg = field(ins, 2, 13, 4);
    } else {
        neg = field(ins, 2, 10, 1); swz = field(ins, 2, 2, 8);
        mux = field(ins, 3, 28, 2);
        reg = (field(ins, 2, 0, 2) << 2) | field(ins, 3, 30, 2);
    }

    if (mux == 1)
        s = (reg == 12) ? opos->v : (reg < 12 ? temp[reg].v : temp[0].v);
    else if (mux == 2)
        s = in[field(ins, 1, 9, 4)];
    else {
        int ci = (int)field(ins, 1, 13, 8);
        if (field(ins, 3, 1, 1))
            ci += a0;
        if (ci < 0 || ci >= NV2A_VSH_CONSTANTS)
            ci = 0;
        s = s_const[ci];
    }
    r.v[0] = s[(swz >> 6) & 3];
    r.v[1] = s[(swz >> 4) & 3];
    r.v[2] = s[(swz >> 2) & 3];
    r.v[3] = s[swz & 3];
    if (neg) {
        r.v[0] = -r.v[0]; r.v[1] = -r.v[1];
        r.v[2] = -r.v[2]; r.v[3] = -r.v[3];
    }
    return r;
}

static void write_masked(float *dst, const vec4 *src, uint32_t mask)
{
    if (mask & 8) dst[0] = src->v[0];
    if (mask & 4) dst[1] = src->v[1];
    if (mask & 2) dst[2] = src->v[2];
    if (mask & 1) dst[3] = src->v[3];
}

static vec4 splat(float f)
{
    vec4 r;
    r.v[0] = r.v[1] = r.v[2] = r.v[3] = f;
    return r;
}

static vec4 run_mac(uint32_t op, vec4 a, vec4 b, vec4 c, int *a0)
{
    vec4 r = a;
    int i;

    switch (op) {
    case 1: break;                                            /* mov */
    case 2: for (i = 0; i < 4; i++) r.v[i] = a.v[i] * b.v[i]; break;
    case 3: for (i = 0; i < 4; i++) r.v[i] = a.v[i] + c.v[i]; break;
    case 4: for (i = 0; i < 4; i++) r.v[i] = a.v[i] * b.v[i] + c.v[i]; break;
    case 5: r = splat(a.v[0]*b.v[0] + a.v[1]*b.v[1] + a.v[2]*b.v[2]); break;
    case 6: r = splat(a.v[0]*b.v[0] + a.v[1]*b.v[1] + a.v[2]*b.v[2]
                      + b.v[3]); break;                        /* dph */
    case 7: r = splat(a.v[0]*b.v[0] + a.v[1]*b.v[1] + a.v[2]*b.v[2]
                      + a.v[3]*b.v[3]); break;
    case 8: r.v[0] = 1.0f; r.v[1] = a.v[1] * b.v[1];            /* dst */
            r.v[2] = a.v[2]; r.v[3] = b.v[3]; break;
    case 9: for (i = 0; i < 4; i++) r.v[i] = a.v[i] < b.v[i] ? a.v[i] : b.v[i]; break;
    case 10: for (i = 0; i < 4; i++) r.v[i] = a.v[i] >= b.v[i] ? a.v[i] : b.v[i]; break;
    case 11: for (i = 0; i < 4; i++) r.v[i] = a.v[i] < b.v[i] ? 1.0f : 0.0f; break;
    case 12: for (i = 0; i < 4; i++) r.v[i] = a.v[i] >= b.v[i] ? 1.0f : 0.0f; break;
    case 13: *a0 = (int)floorf(a.v[0] + 0.001f); break;         /* arl */
    default: break;
    }
    return r;
}

static vec4 run_ilu(uint32_t op, vec4 c)
{
    float x = c.v[0];
    vec4 r = c;

    switch (op) {
    case 1: break;                                            /* mov */
    case 2: r = splat(x == 0.0f ? INFINITY : 1.0f / x); break;  /* rcp */
    case 3: {                                                  /* rcc */
        float f = 1.0f / x;
        if (fabsf(f) < 5.42101e-20f) f = f < 0 ? -5.42101e-20f : 5.42101e-20f;
        if (fabsf(f) > 1.884467e19f) f = f < 0 ? -1.884467e19f : 1.884467e19f;
        r = splat(f);
        break;
    }
    case 4: r = splat(1.0f / sqrtf(fabsf(x))); break;           /* rsq */
    case 5: {                                                  /* exp */
        float fl = floorf(x);
        r.v[0] = exp2f(fl); r.v[1] = x - fl; r.v[2] = exp2f(x); r.v[3] = 1.0f;
        break;
    }
    case 6: {                                                  /* log */
        float ax = fabsf(x);
        if (ax == 0.0f) {
            r.v[0] = r.v[2] = -INFINITY; r.v[1] = 1.0f;
        } else {
            float e = floorf(log2f(ax));
            r.v[0] = e; r.v[1] = ax / exp2f(e); r.v[2] = log2f(ax);
        }
        r.v[3] = 1.0f;
        break;
    }
    case 7: {                                                  /* lit */
        float nl = c.v[0] > 0.0f ? c.v[0] : 0.0f;
        float nh = c.v[1] > 0.0f ? c.v[1] : 0.0f;
        float p = c.v[3] < -127.9961f ? -127.9961f
                : (c.v[3] > 127.9961f ? 127.9961f : c.v[3]);
        r.v[0] = 1.0f; r.v[1] = nl;
        r.v[2] = c.v[0] > 0.0f ? powf(nh, p) : 0.0f; r.v[3] = 1.0f;
        break;
    }
    default: break;
    }
    return r;
}

static int nv2a_vsh_run_interpreted(const float in[NV2A_VSH_INPUTS][4], Nv2aVshOutput *out)
{
    static int dump = -1;
    static uint32_t dumped[16];
    static int ndumped;
    vec4 temp[12], opos;
    float outregs[13][4];
    int a0 = 0;
    uint32_t s;

    if (dump < 0)
        dump = getenv("RECOMP_VSH_DUMP") != NULL;
    if (dump && ndumped < 16) {
        /* By content: titles reload different programs into the same slot. */
        uint32_t h = 2166136261u, t;
        int i, seen = 0;
        for (t = s_start_slot; t < NV2A_VSH_SLOTS; t++) {
            for (i = 0; i < 4; i++)
                h = (h ^ s_program[t][i]) * 16777619u;
            if (field(s_program[t], 3, 0, 1))
                break;
        }
        for (i = 0; i < ndumped; i++)
            if (dumped[i] == h) seen = 1;
        if (!seen) {
            dumped[ndumped++] = h;
            dump_program(s_start_slot);
        }
    }

    memset(temp, 0, sizeof temp);
    memset(&opos, 0, sizeof opos);
    memset(outregs, 0, sizeof outregs);
    outregs[3][3] = outregs[4][3] = 1.0f;      /* colours default opaque */
    /* Unwritten texture coordinates are (0,0,0,1), as xemu initialises them:
     * a projective stage divides by q, and q = 0 would put every texel of an
     * unwritten stage at infinity. */
    outregs[9][3] = outregs[10][3] = outregs[11][3] = outregs[12][3] = 1.0f;

    for (s = s_start_slot; s < NV2A_VSH_SLOTS; s++) {
        const uint32_t *ins = s_program[s];
        uint32_t mac = field(ins, 1, 21, 4), ilu = field(ins, 1, 25, 3);
        uint32_t mac_mask = field(ins, 3, 24, 4), ilu_mask = field(ins, 3, 16, 4);
        uint32_t tdst = field(ins, 3, 20, 4);
        uint32_t omask = field(ins, 3, 12, 4), oaddr = field(ins, 3, 3, 8);
        vec4 a = read_src(ins, 0, in, temp, &opos, a0);
        vec4 b = read_src(ins, 1, in, temp, &opos, a0);
        vec4 c = read_src(ins, 2, in, temp, &opos, a0);
        vec4 mres = {{0, 0, 0, 0}}, ires = {{0, 0, 0, 0}};

        if (mac)
            mres = run_mac(mac, a, b, c, &a0);
        if (ilu)
            ires = run_ilu(ilu, c);

        if (mac && mac != 13 && mac_mask) {
            float *d = tdst == 12 ? opos.v : (tdst < 12 ? temp[tdst].v : NULL);
            if (d) write_masked(d, &mres, mac_mask);
        }
        if (ilu && ilu_mask) {
            /* Paired with any MAC op, the ILU can only write R1 -- even when
             * the MAC half writes no temp at all. The D3D epilogue is exactly
             * that: `mul o[0].xyz` beside `rcc R1.x`, then `mad` with R1.x.
             * Sending the rcc to the temp field (R7 there) left R1.x at 0 and
             * put every vertex of every 3D batch at c[59]. */
            uint32_t it = mac ? 1u : tdst;
            float *d = it == 12 ? opos.v : (it < 12 ? temp[it].v : NULL);
            if (d) write_masked(d, &ires, ilu_mask);
        }
        if (omask) {
            const vec4 *src = field(ins, 3, 2, 1) ? &ires : &mres;
            if (!field(ins, 3, 11, 1)) {                  /* to c[] */
                if (s_cxt_write && oaddr < NV2A_VSH_CONSTANTS)
                    write_masked(s_const[oaddr], src, omask);
            } else if (oaddr == 0) {
                write_masked(opos.v, src, omask);
            } else if (oaddr < 13) {
                write_masked(outregs[oaddr], src, omask);
            }
        }
        if (field(ins, 3, 0, 1)) {
            memcpy(out->pos, opos.v, sizeof out->pos);
            memcpy(out->d0, outregs[3], sizeof out->d0);
            memcpy(out->d1, outregs[4], sizeof out->d1);
            memcpy(out->fog, outregs[5], sizeof out->fog);
            memcpy(out->tex[0], outregs[9], sizeof out->tex[0]);
            memcpy(out->tex[1], outregs[10], sizeof out->tex[1]);
            memcpy(out->tex[2], outregs[11], sizeof out->tex[2]);
            memcpy(out->tex[3], outregs[12], sizeof out->tex[3]);
            return 1;
        }
    }
    return 0;
}

/* Generated scalar SSA removes opcode decoding, temporary-register arrays,
 * unused components and per-instruction dispatch. This is CPU execution;
 * the existing exact clipper and pixel renderer retain their current inputs. */
#if defined(NV2A_QPR2_NATIVE_VSH_HEADER_PRESENT)
# if NV2A_QPR2_NATIVE_VSH_HEADER_PRESENT
#  include "nv2a_vsh_compiled.h"
#  define NV_VSH_COMPILED_COUNT (sizeof nv_vsh_compiled / sizeof nv_vsh_compiled[0])
# else
#  include "nv2a_vsh_compiled_fallback.h"
#  define NV_VSH_COMPILED_COUNT 0u
# endif
#elif defined(__has_include) && __has_include("nv2a_vsh_compiled.h")
#include "nv2a_vsh_compiled.h"
#define NV_VSH_COMPILED_COUNT (sizeof nv_vsh_compiled / sizeof nv_vsh_compiled[0])
#else
#include "nv2a_vsh_compiled_fallback.h"
#define NV_VSH_COMPILED_COUNT 0u
#endif

static const NvVshCompiled *s_native;
static int s_native_enabled = -1;
static uint64_t s_native_runs, s_interpreted_runs, s_native_matches, s_native_misses;

static const NvVshCompiled *nv2a_vsh_resolve_native(void)
{
    if (s_native_enabled < 0) {
        const char *e = getenv("RECOMP_VERTEX_NATIVE");
        s_native_enabled = (!e || atoi(e) > 0) && !getenv("RECOMP_VSH_DUMP");
    }
    if (!s_native_enabled || s_cxt_write) return NULL;
    if (s_native_dirty) {
        unsigned end = s_start_slot;
        s_native_dirty = 0;
        s_native = NULL;
        for (; end < NV2A_VSH_SLOTS; ++end) {
            if (s_program[end][3] & 1u) {
                unsigned n = end - s_start_slot + 1;
                for (unsigned i = 0; i < NV_VSH_COMPILED_COUNT; ++i) {
                    const NvVshCompiled *p = &nv_vsh_compiled[i];
                    if (p->count == n && !memcmp(p->words, &s_program[s_start_slot], n * sizeof s_program[0])) {
                        s_native = p;
                        break;
                    }
                }
                break;
            }
        }
        if (s_native) ++s_native_matches; else ++s_native_misses;
    }
    return s_native;
}

uint32_t nv2a_vsh_input_mask(void)
{
    const NvVshCompiled *p = nv2a_vsh_resolve_native();
    return p ? p->inputs : 0xffffu;
}
int nv2a_vsh_snapshot(Nv2aVshSnapshot *s)
{
    const NvVshCompiled *p=nv2a_vsh_resolve_native();
    if(!p || !p->position_kind)return 0;
    s->program_id=(unsigned)(p-nv_vsh_compiled);s->count=p->count;
    s->inputs=p->inputs;s->position_kind=p->position_kind;s->words=p->words;s->constants=s_const;
    return 1;
}
unsigned nv2a_vsh_known_count(void){return (unsigned)NV_VSH_COMPILED_COUNT;}
const uint32_t (*nv2a_vsh_known_program(unsigned id,unsigned *count))[4]
{
    if(id>=nv2a_vsh_known_count()){*count=0;return NULL;}
    *count=nv_vsh_compiled[id].count;return nv_vsh_compiled[id].words;
}

int nv2a_vsh_run(const float in[NV2A_VSH_INPUTS][4], Nv2aVshOutput *out)
{
    const NvVshCompiled *p = nv2a_vsh_resolve_native();
    if (p) {
        ++s_native_runs;
        p->run(in, out);
        return 1;
    }
    ++s_interpreted_runs;
    return nv2a_vsh_run_interpreted(in, out);
}

void nv2a_vsh_report(void)
{
    fprintf(stderr, "[VERTEX-NATIVE] enabled=%u variants=%u native_vertices=%llu interpreted_vertices=%llu program_matches=%llu program_misses=%llu\n",
        (unsigned)(s_native_enabled > 0), (unsigned)NV_VSH_COMPILED_COUNT,
        (unsigned long long)s_native_runs, (unsigned long long)s_interpreted_runs,
        (unsigned long long)s_native_matches, (unsigned long long)s_native_misses);
    s_native_runs = s_interpreted_runs = s_native_matches = s_native_misses = 0;
}
