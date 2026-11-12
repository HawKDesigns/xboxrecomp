/* No game-specific vertex programs are distributed with the runtime.
 * A local generated nv2a_vsh_compiled.h enables the exact-match fast path. */
#ifndef NV2A_VSH_COMPILED_FALLBACK_H
#define NV2A_VSH_COMPILED_FALLBACK_H
typedef void (*NvVshNative)(const float in[16][4], Nv2aVshOutput *out);
typedef struct {
    unsigned count, inputs, position_kind;
    const uint32_t (*words)[4];
    NvVshNative run;
} NvVshCompiled;
static const NvVshCompiled nv_vsh_compiled[1] = {{0}};
#endif
