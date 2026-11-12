#define NV_GPU_HLSL 1
#ifndef NV_GPU_DIAGNOSTIC_COUNTERS
#define NV_GPU_DIAGNOSTIC_COUNTERS 1
#endif
#include "nv2a_gpu_types.h"
StructuredBuffer<NvGpuTri> triangles : register(t0);
StructuredBuffer<NvGpuState> configuration : register(t3);
struct RasterInput { float4 position : SV_Position; nointerpolation uint primitive : TEXCOORD0; };
#ifdef NV_GPU_VERTEX
RasterInput vertex_main(uint id : SV_VertexID)
{
    uint primitive=id/6,vertex=id%6;
    NvGpuTri tri=triangles[primitive];
    float width=(float)configuration[0].width,height=(float)configuration[0].height;
    /* Expand by one pixel: the native rasterizer generates candidates only.
     * Shared exact edge/depth logic below remains the coverage authority. */
    float left=max(0,tri.minx-1),right=min(width,tri.maxx+1);
    float top=max(0,tri.miny-1),bottom=min(height,tri.maxy+1);
    bool east=vertex==1 || vertex==2 || vertex==4;
    bool south=vertex==2 || vertex==4 || vertex==5;
    float x=east?right:left,y=south?bottom:top;
    RasterInput out_value;
    out_value.position=float4(x*2.0f/width-1.0f,1.0f-y*2.0f/height,0.5f,1.0f);
    out_value.primitive=primitive;return out_value;
}
#else
StructuredBuffer<uint> texture_atlas : register(t4);
#ifndef NV_GPU_DEPTH_SEED
#define NV_GPU_DEPTH_SEED 0
#endif
#if NV_GPU_DEPTH_SEED
StructuredBuffer<uint2> batch_depth_seed : register(t5);
StructuredBuffer<uint> depth_seed_control : register(t6);
#endif
/* The ROV orders overlapping pixel invocations and makes preceding writes
 * visible. Keep the per-writer barrier below. The local-scope variant avoids
 * a GPU-wide UAV flush; startup compares exact output before it can render. */
#ifndef NV_GPU_LOCAL_ROV
#define NV_GPU_LOCAL_ROV 0
#endif
#if NV_GPU_LOCAL_ROV
RasterizerOrderedStructuredBuffer<uint2> surface_buffer : register(u1);
#else
globallycoherent RasterizerOrderedStructuredBuffer<uint2> surface_buffer : register(u1);
#endif
RWStructuredBuffer<uint> statistics : register(u2);
/* Only the generated 32x32 startup fixture writes this bounded trace. */
RWStructuredBuffer<uint4> startup_trace : register(u3);
uint gpu_load_texel(NvGpuTexture t,int st,int face,int x,int y)
{
    return texture_atlas[t.texel_base+((uint)face*t.height+(uint)y)*t.width+(uint)x];
}
#ifndef NV_GPU_STAGE_LIVENESS
#define NV_GPU_STAGE_LIVENESS 0
#endif
#if NV_GPU_STAGE_LIVENESS
StructuredBuffer<uint> material_stage_masks : register(t7);
#define GPU_TEXTURE_STAGE_MASK(tri) material_stage_masks[tri.state_index]
#endif
#include "nv2a_gpu_pixel.inc"
#ifndef NV_GPU_LITERAL_CLASS
#define NV_GPU_LITERAL_CLASS 0
#endif
#include "nv2a_gpu_literal.inc"
#ifndef NV_GPU_COMPACT_MATERIALS
#define NV_GPU_COMPACT_MATERIALS 0
#endif
#include "nv2a_gpu_compact.inc"
#include "nv2a_gpu_shade.inc"
#ifndef NV_GPU_PROBE_SHADE
#define NV_GPU_PROBE_SHADE 0
#endif
#if NV_GPU_PROBE_SHADE
/* Optional private replay: force source work to remain observable without
 * touching the ordered framebuffer or the guest visibility counters. */
RWStructuredBuffer<uint> probe_sink : register(u4);
#endif
#ifndef NV_GPU_EMPTY_REJECT
#define NV_GPU_EMPTY_REJECT 0
#endif
#ifndef NV_GPU_EARLY_DEPTH
#define NV_GPU_EARLY_DEPTH 0
#endif
#ifndef NV_GPU_SKIP_UNCHANGED
#define NV_GPU_SKIP_UNCHANGED 0
#endif
float4 pixel_main(RasterInput input) : SV_Target0
{
    int x=(int)input.position.x,y=(int)input.position.y;
    NvGpuTri tri=triangles[input.primitive];NvGpuState state=configuration[tri.state_index];
    NvGpuBary bary=gpu_bary(state,tri,x,y);
    uint at=(uint)y*state.width+(uint)x;
    NvGpuSource source;source.argb=source.keep=0;
    int shade=bary.covered;
#if NV_GPU_DEPTH_SEED
    if(shade && depth_seed_control[0] && input.primitive<depth_seed_control[1])
        shade=gpu_seed_source_visible(state,bary,asfloat(batch_depth_seed[at].y));
#endif
#if NV_GPU_EMPTY_REJECT && !NV_GPU_DIAGNOSTIC_COUNTERS
    /* Coverage misses and frozen-depth failures cannot change any guest
     * output. Exit BEFORE touching ordered storage or its device fence. */
    if(!shade)return 0;
#endif
#if NV_GPU_EARLY_DEPTH && !NV_GPU_PROBE_SHADE
    /* Exact live depth can reject a hidden fragment before texture and
     * combiner work. Keep the ROV read and following write in one ordered
     * invocation; neither a frozen snapshot nor a CPU depth guess is used. */
    uint2 before=surface_buffer[at];
    if(shade && state.depth_available && state.depth_test &&
       !gpu_compare(bary.depth,asfloat(before.y),state.depth_func))return 0;
#endif
    if(shade)source=gpu_source_color(state,tri,bary.l0,bary.l1,bary.l2);
#if NV_GPU_EMPTY_REJECT && !NV_GPU_DIAGNOSTIC_COUNTERS
    /* Alpha/clip rejection has no color, depth or visibility contribution.
     * Diagnostic mode retains the transaction to count live depth failures. */
    if(!source.keep)return 0;
#endif
#if NV_GPU_PROBE_SHADE
    if(source.keep)probe_sink[at]=source.argb;
    return 0;
#else
    /* Surviving viewport candidates enter one short ordered
     * read/modify/write. Texture/combiner work and coverage divergence stay
     * outside this transaction; unchanged candidates can retain their input.
     * The private RTV/viewport bounds guarantee a valid pixel address. */
#if !(NV_GPU_EARLY_DEPTH && !NV_GPU_PROBE_SHADE)
    uint2 before=surface_buffer[at];
#endif
    NvGpuPixel p=gpu_commit(state,bary,source,before.x,asfloat(before.y));
    uint2 after=uint2(p.color,asuint(p.depth));
#if NV_GPU_SKIP_UNCHANGED
    /* Compare storage bits, including signed zero and NaN payloads. A read-only
     * invocation has nothing to publish; every actual writer keeps V54's fence.
     * Do not return here: unchanged output can still count as guest zpass. */
    [branch] if(after.x!=before.x || after.y!=before.y)
#endif
    {
        surface_buffer[at]=after;
        /* Explicit visibility is still required after EVERY surface write. */
        DeviceMemoryBarrier();
    }
    if(state.width==32 && state.height==32 && input.primitive<8)
        startup_trace[input.primitive*1024+at]=uint4(before,p.color,asuint(p.depth));
    /* Shard atomics across 16x16 screen tiles instead of one hot address. */
    uint base=(((uint)y/16)*((state.width+15)/16)+(uint)x/16)*4;
    /* Guest visibility reports are semantic output. Other per-fragment
     * counters are diagnostics and can be omitted in performance runs. */
    if(p.zpass)InterlockedAdd(statistics[base+2],p.zpass);
#if NV_GPU_DIAGNOSTIC_COUNTERS
    if(p.depth_fail)InterlockedAdd(statistics[base],p.depth_fail);
    if(p.pixels)InterlockedAdd(statistics[base+1],p.pixels);
    uint previous=statistics[base+3],observed;
    while((p.pixel_max&0xFFFFFFu)>(previous&0xFFFFFFu)) {
        InterlockedCompareExchange(statistics[base+3],previous,p.pixel_max,observed);
        if(observed==previous)break;previous=observed;
    }
#endif
    return 0; /* Private raster output; guest pixels live in ordered storage. */
#endif
}
#endif
