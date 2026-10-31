/* Hardware pixel rendering for the working pushbuffer executor. The draw
 * stays resident until a CPU/guest completion boundary, not one readback per
 * triangle. Each compute group owns one 16x16 tile and replays its ordered
 * primitive list, preserving depth, alpha and blending without pixel races. */
#include "nv2a_gpu_raster.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#if defined(_WIN32)
#define COBJMACROS
#include <windows.h>
#include <d3d11.h>
#include <d3d11_3.h>
#include <d3dcompiler.h>
#include <dxgi.h>
#include "../d3d/d3d8_swizzle.h"
#include "nv2a_raster_rules.h"
#include "nv2a_gpu_shader.h"

typedef struct {
    ID3D11Buffer *buffer, *readback;
    ID3D11ShaderResourceView *srv;
    ID3D11UnorderedAccessView *uav;
    unsigned bytes, stride;
    uint8_t *upload_copy;
    unsigned upload_capacity,upload_valid;
} GpuBuffer;
#include "nv2a_gpu_geometry_state.h"
#include "nv2a_gpu_depth_seed.h"
#include "nv2a_gpu_material_usage.h"
#if defined(NV2A_QPR2_LITERAL_HEADER_PRESENT)
# if NV2A_QPR2_LITERAL_HEADER_PRESENT
#  include "nv2a_gpu_literal_programs.h"
#  define NV_GPU_LITERAL_AVAILABLE 1u
# else
#  include "nv2a_gpu_literal_fallback.h"
#  define NV_GPU_LITERAL_AVAILABLE 0u
# endif
#elif defined(__has_include) && __has_include("nv2a_gpu_literal_programs.h")
#include "nv2a_gpu_literal_programs.h"
#define NV_GPU_LITERAL_AVAILABLE 1u
#else
#include "nv2a_gpu_literal_fallback.h"
#define NV_GPU_LITERAL_AVAILABLE 0u
#endif
#define NV_GPU_UPLOAD_SLOTS 4u
typedef struct {GpuBuffer triangles,state,references,tiles,stage_masks;} GpuUploadSlot;
typedef struct {
    unsigned atlas_base, queued;
    NvGpuTexture meta;
    const uint8_t *address, *palette_address;
    uint8_t *raw, *palette;
    uint32_t *decoded;
    size_t bytes, palette_bytes, decoded_bytes;
    uint64_t used, version, gpu_serial;
} GpuTexture;
typedef struct {uint32_t color,depth_bits;} GpuSurface;
#include "nv2a_gpu_surface_copy.h"
typedef struct {
    GpuBuffer color,depth,surface;
    int native;
    uint64_t used;
} GpuTargetBuffers;
/* Copies queued at target switches have private staging storage. Host pixel
 * publication is delayed only until the next existing CPU/fence dependency. */
#define NV_GPU_PENDING_READBACKS 4u
extern ptrdiff_t xbox_GetMemoryOffset(void);
typedef struct {
    ID3D11Buffer *surface,*stats;
    GpuBuffer snapshot;
    uint64_t serial;
    uint32_t *host_baseline;
    unsigned baseline_bytes;int baseline_valid;
    unsigned surface_bytes,stats_bytes;
    int has_stats,publish_surface,surface_copy_pending;
    NvGpuState target;
    uint8_t *host_color;
    float *host_depth;
    unsigned depth_stride,left,right,top,bottom,triangles;
} GpuPendingReadback;
static struct {
    int init, active, selftest, resident,readback_failed;
    struct {
        int enabled,testing;
        ID3D11ComputeShader *shader;
        GpuBuffer parameters,texels;
        uint64_t serial,snapshots,packs,hits,bytes,eligible,misses;
    } bridge;
    struct {
        int enabled,testing,active,retire_pixels;
        uint64_t retired,retired_bytes,counter_reads,empty_retired;
        uint64_t restores,checks,changed,conflicts,allocation_failures,bytes;
    } resume;
    struct {
        int enabled;
        GpuBuffer fallback,stage_masks;
        uint32_t masks[NV_GPU_MAX_STATES];
        unsigned program_count;
        struct {uint32_t hash,words[36];uint64_t states,triangles;} program[32];
        uint64_t batches,mask_batches,fallback_batches,dead_tri_stages,off_tri_stages,untracked_states;
    } material;
    struct {
        int enabled;
        unsigned compiling,testing,variant,ready,rejected;
        unsigned compact_enabled,compact_compiling,compact_ready,compact_rejected;
        ID3D11PixelShader *shader[NV_GPU_LITERAL_CLASSES];
        uint8_t state_class[NV_GPU_MAX_STATES];
        uint64_t draws,triangles,generic_draws,class_triangles[NV_GPU_LITERAL_CLASSES];
    } literal;
    NvGpuTri *queued;
    NvGpuState queued_state[NV_GPU_MAX_STATES];
    unsigned queued_count, state_count, queued_refs;
    struct { uint64_t hash; unsigned index_plus_one; } state_intern[256];
    int state_intern_enabled;
    uint64_t state_intern_hits,state_intern_misses,state_intern_bytes_saved;
    ID3D11Device *device;
    ID3D11DeviceContext *context;
    ID3D11ComputeShader *shader;
    ID3D11ComputeShader *simple_shader;
    ID3D11ComputeShader *report_shader;
    ID3D11ComputeShader *clear_shader;
    GpuBuffer clear_parameters;
    int queue_simple, array_registers;
    ID3D11VertexShader *vertex_shader, *coverage_vertex_shader;
    ID3D11PixelShader *pixel_shader, *simple_pixel_shader;
    ID3D11RasterizerState *rasterizer;
    ID3D11RasterizerState2 *coverage_rasterizer;
    ID3D11Buffer *vertex_ids;
    int coverage, native_batch;
    struct { int enabled; uint64_t fused_draws, runs_saved, triangles; } micro_fuse;
    uint8_t queued_kind[NV_GPU_MAX_TRIANGLES];
    uint64_t queue_uploads;
    ID3D11Texture2D *raster_target;
    ID3D11RenderTargetView *raster_view;
    unsigned raster_width,raster_height;
    int native,uav_only;
    int native_diagnostics,vector_combiner,quad_sampling,empty_reject,early_depth;
    int local_rov,local_rov_rejected;
    unsigned skip_unchanged,store_ready,store_rejected;
    uint64_t empty_reject_batches,empty_reject_triangles;
    struct {
        int enabled,prefix_enabled;
        uint32_t control_value;
        GpuBuffer image,control;
        uint64_t batches,copies,bytes,unsafe,cold,small_batches,allocation_failures;
        uint64_t partial_batches,partial_triangles;
        int reuse_enabled,snapshot_valid,depth_dirty;
        uint64_t reuses,reused_bytes;
    } seed;
    int auto_enabled,auto_done,auto_busy,auto_choose_compute,target_written;
    double auto_native_ms,auto_compute_ms;
    int readback_queue,readback_queue_test;
    int skip_empty_stats,stats_required,stats_initialized;
    uint64_t empty_stats_skips,stats_initializations;
    GpuPendingReadback pending[NV_GPU_PENDING_READBACKS];
    unsigned pending_count;
    uint64_t queued_readbacks,pending_peak,pending_dependencies,pending_capacity;
    double pending_submit_ms;
    struct {uint64_t binds,queued,readbacks,pixels,empty_skips;double map_wait_ms;} target_shape[4];
    struct {
        int enabled,unpublished,feedback_enabled;
        GpuBuffer marker;
        ID3D11Buffer *staging;
        unsigned staging_bytes,serial;
        uint64_t waits,avoided_pixels,access_drains;
        uint64_t feedback_snapshots,feedback_fallbacks;
        double wait_ms;
    } completion;
    struct {
        int enabled;
        uint64_t deferred,issued,avoided,avoided_bytes;
        double enqueue_ms;
    } deferred_readback;
    int upload_stream,upload_cache;
    unsigned upload_cursor;
    GpuUploadSlot upload_spare[NV_GPU_UPLOAD_SLOTS-1];
    uint64_t upload_writes,upload_reuses,upload_bytes,upload_compared,upload_rotations,geometry_reuses;
    int buffer_native;
    GpuTargetBuffers target_spare[3];
    uint64_t target_tick;
    GpuBuffer triangles, references, tiles, state, color, depth, surface, stats, atlas, startup_trace;
    GpuTexture cache[128];
    GpuTexture *bound[4];
    unsigned texture_hot[4],texture_hot_count;
    uint8_t texture_index[256];
    int texture_hot_enabled;
    uint64_t texture_hot_hits,texture_index_hits,texture_full_scans;
    uint64_t tick;
    NvGpuState target;
    uint8_t *host_color;
    float *host_depth;
    unsigned depth_stride;
    unsigned dirty_left,dirty_right,dirty_top,dirty_bottom;
    uint32_t *color_upload, *refs;
    float *depth_upload;
    GpuSurface *surface_upload;
    NvGpuTile tile_upload[NV_GPU_MAX_TILES];
    NvGpuCount completed;
    uint64_t draws, dispatches, simple_dispatches, simple_tris, tris, groups, uploads, readbacks, fallbacks;
    uint64_t color_uploads,depth_uploads,color_reuses,depth_reuses;
    uint64_t report_resets;
    int report_only_enabled;
    uint64_t report_only,report_fallback,report_bytes;
    double report_wait_ms;
    uint64_t clear_commands;
    uint64_t texture_hits,texture_updates;
    double decode_ms,atlas_ms;
    int bulk_texture;
    uint64_t bulk_images,bulk_texels,scalar_images;
    struct {
        int enabled,ready,compiling;
        ID3D11PixelShader *shader;
        GpuBuffer sink;
        uint64_t replays,scratch_bytes,skips;
    } shade_probe;
    uint64_t target_binds,target_creates,target_pool_hits;
    double target_create_ms;
    uint64_t clear_skips[6];
    uint64_t disjoint_writes;
    unsigned pending_triangles,slow_waits,slow_targets;
    double clear_ms;
    double submit_ms, readback_ms, texture_ms, target_ms, map_ms, copy_ms;
    struct {uint64_t batches;double upload_ms,raw_ms,raster_ms,total_ms;} flush_time;
    int readback_simd;
    struct { int enabled; uint64_t batches, older_targets, current_bytes; } readback_batch;
    struct {
        double enqueue_ms,map_wait_ms,unpack_ms,publish_ms,counters_ms,unmap_ms,capture_ms;
        uint64_t current,queued,pixels,simd_rows,scalar_rows;
        uint64_t surface_copies,surface_partial,surface_empty,surface_bytes,surface_bytes_avoided;
    } readback_time;
    struct {char name[64];uint64_t count;double ms;} sync_reason[20];
} g;

/* Initialization owns only private offscreen data. Guest execution continues
 * on CPU until this release/acquire publication makes the context available.
 * The immediate context has one owner at a time, including its self-test. */
static volatile LONG s_startup; /* 0 new, 1 preparing, 2 ready, 3 disabled */
static volatile LONG s_startup_thread;
static int gpu_initialize(void);
static int gpu_rc_simple_kind(Nv2aCombiner rc);
static int gpu_startup_owned(void)
{
    return InterlockedCompareExchange(&s_startup,0,0)==1 &&
           GetCurrentThreadId()==(DWORD)InterlockedCompareExchange(&s_startup_thread,0,0);
}
// Optional nonblocking GPU timestamps use the same context owner.
#include "nv2a_gpu_timing.inc"
static int gpu_shade_probe_initialize(void);
static void gpu_shade_probe_replay(unsigned token,unsigned simple,unsigned first,unsigned end,unsigned factor);
static DWORD WINAPI gpu_prepare_thread(LPVOID unused)
{
    (void)unused;InterlockedExchange(&s_startup_thread,(LONG)GetCurrentThreadId());
    int ok=gpu_initialize();
    InterlockedExchange(&s_startup,ok?2:3);
    return 0;
}

/* Bounded, opt-in F8 diagnostics: all file access stays on the renderer thread.
 * Records contain plain values/decoded texels, never native object pointers. */
static volatile LONG s_capture_requested;
static FILE *s_capture;
static uint64_t s_capture_bytes,s_capture_versions[128];
static unsigned s_capture_presents;
static void gpu_capture_close(const char *reason)
{
    if(!s_capture)return;
    fclose(s_capture);s_capture=NULL;
    fprintf(stderr,"[GPU-CAPTURE] stopped reason=%s bytes=%llu presents=%u\n",reason,
        (unsigned long long)s_capture_bytes,s_capture_presents);
}
void nv_gpu_capture_request(void)
{
    if(getenv("RECOMP_GPU_CAPTURE"))InterlockedExchange(&s_capture_requested,1);
}
static int gpu_capture_header(unsigned type,size_t bytes)
{
    if(!s_capture)return 0;
    if(bytes>192u*1024u*1024u || s_capture_bytes+bytes+8>192u*1024u*1024u) {
        gpu_capture_close("size limit");return 0;
    }
    uint32_t h[2]={type,(uint32_t)bytes};
    if(fwrite(h,sizeof h,1,s_capture)!=1){gpu_capture_close("write failure");return 0;}
    s_capture_bytes+=bytes+8;return 1;
}
static void gpu_capture_data(const void *data,size_t bytes)
{
    if(s_capture && fwrite(data,1,bytes,s_capture)!=bytes)gpu_capture_close("write failure");
}
static int gpu_readback(const char *reason);
static int gpu_pending_drain(void);
static int gpu_pending_access(const void *address,size_t bytes);
static int gpu_queue_readback(void);
static int gpu_backend_benchmark(void);
static GpuPendingReadback *gpu_bridge_source(const NvGpuTexture*,const NvGpuTextureSource*);
static int gpu_bridge_prepare(unsigned);
static int gpu_bridge_pack(GpuPendingReadback*,unsigned,unsigned);
static int gpu_bridge_initialize(void);
static int gpu_bridge_selftest(void);
static void gpu_resume_save(GpuPendingReadback*);
static GpuPendingReadback *gpu_resume_source(const NvGpuState*,const uint8_t*,const float*,unsigned);
static int gpu_resume_target(const NvGpuState*,uint8_t*,float*,unsigned);
static int gpu_resume_selftest(void);
static int gpu_capture_begin(void)
{
    if(g.selftest || !InterlockedExchange(&s_capture_requested,0))return 1;
    if(!gpu_readback("capture-begin"))return 0;
    gpu_capture_close("new request");const char *path=getenv("RECOMP_GPU_CAPTURE");
    if(!path || !*path)return 1;
    s_capture=fopen(path,"wb");if(!s_capture){fprintf(stderr,"[GPU-CAPTURE] cannot open file\n");return 1;}
    uint32_t h[4]={0x51475235u,1,sizeof(NvGpuState),sizeof(NvGpuTri)};
    if(fwrite(h,sizeof h,1,s_capture)!=1){gpu_capture_close("write failure");return 1;}
    s_capture_bytes=sizeof h;s_capture_presents=0;memset(s_capture_versions,0,sizeof s_capture_versions);
    fprintf(stderr,"[GPU-CAPTURE] armed next two presented frames: %s\n",path);return 1;
}
void nv_gpu_capture_present(const uint32_t *pixels,unsigned width,unsigned height)
{
    gpu_timing_frame();
    /* The setup worker may be writing a private failure capture while the
     * guest still presents CPU frames. That stream has exactly one owner. */
    if(InterlockedCompareExchange(&s_startup,0,0)!=2)return;
    ++raw.cache_frame; /* Cache admission epoch only; no timing/simulation change. */
    size_t bytes=(size_t)width*height*4;
    if(!s_capture || !pixels || width>1024 || height>1024)return;
    uint32_t h[2]={width,height};
    if(gpu_capture_header(5,8+bytes)){gpu_capture_data(h,8);gpu_capture_data(pixels,bytes);}
    if(s_capture && ++s_capture_presents>=2)gpu_capture_close("two frames completed");
}
static double gpu_ms(void)
{
    LARGE_INTEGER t, f;QueryPerformanceCounter(&t);QueryPerformanceFrequency(&f);
    return (double)t.QuadPart*1000.0/f.QuadPart;
}
static void gpu_buffer_release(GpuBuffer *b)
{
    if (b->srv) ID3D11ShaderResourceView_Release(b->srv);
    if (b->uav) ID3D11UnorderedAccessView_Release(b->uav);
    if (b->readback) ID3D11Buffer_Release(b->readback);
    if (b->buffer) ID3D11Buffer_Release(b->buffer);
    free(b->upload_copy);
    memset(b,0,sizeof *b);
}
static int gpu_buffer_create(GpuBuffer *b, unsigned bytes, unsigned stride, int output)
{
    D3D11_BUFFER_DESC desc;D3D11_SUBRESOURCE_DATA data;
    memset(&desc,0,sizeof desc);memset(&data,0,sizeof data);
    desc.ByteWidth=bytes;desc.Usage=D3D11_USAGE_DEFAULT;
    desc.BindFlags=output==2?(D3D11_BIND_UNORDERED_ACCESS|D3D11_BIND_SHADER_RESOURCE):
        (output?D3D11_BIND_UNORDERED_ACCESS:D3D11_BIND_SHADER_RESOURCE);
    desc.MiscFlags=D3D11_RESOURCE_MISC_BUFFER_STRUCTURED;desc.StructureByteStride=stride;
    if (FAILED(ID3D11Device_CreateBuffer(g.device,&desc,NULL,&b->buffer))) goto fail;
    b->bytes=bytes;b->stride=stride;
    if (output) {
        D3D11_UNORDERED_ACCESS_VIEW_DESC view;memset(&view,0,sizeof view);
        view.Format=DXGI_FORMAT_UNKNOWN;view.ViewDimension=D3D11_UAV_DIMENSION_BUFFER;
        view.Buffer.NumElements=bytes/stride;
        if (FAILED(ID3D11Device_CreateUnorderedAccessView(g.device,(ID3D11Resource*)b->buffer,&view,&b->uav))) goto fail;
        desc.Usage=D3D11_USAGE_STAGING;desc.BindFlags=0;desc.CPUAccessFlags=D3D11_CPU_ACCESS_READ;
        /* Staging buffers have no structured-buffer bindings or misc flags. */
        desc.MiscFlags=0;desc.StructureByteStride=0;
        if (output==1 && FAILED(ID3D11Device_CreateBuffer(g.device,&desc,NULL,&b->readback))) goto fail;
    }
    if(output!=1) {
        D3D11_SHADER_RESOURCE_VIEW_DESC view;memset(&view,0,sizeof view);
        view.Format=DXGI_FORMAT_UNKNOWN;view.ViewDimension=D3D11_SRV_DIMENSION_BUFFER;
        view.Buffer.NumElements=bytes/stride;
        if (FAILED(ID3D11Device_CreateShaderResourceView(g.device,(ID3D11Resource*)b->buffer,&view,&b->srv))) goto fail;
    }
    return 1;
fail:
    gpu_buffer_release(b);return 0;
}
static int gpu_upload(GpuBuffer *b, const void *data, unsigned bytes, unsigned stride)
{
    if (bytes>b->bytes || stride!=b->stride) {
        GpuBuffer next={0};unsigned elements=(bytes+stride-1)/stride, capacity=1;
        while(capacity<elements)capacity*=2;
        if(!gpu_buffer_create(&next,capacity*stride,stride,0))return 0;
        gpu_buffer_release(b);*b=next;
    }
    /* SRV-only input storage is immutable between these uploads. Byte-exact
     * proof permits reuse; an address/asset label or a hash alone does not. */
    if(g.upload_cache && !g.selftest && b->upload_valid==bytes) {
        g.upload_compared+=bytes;
        if(!memcmp(b->upload_copy,data,bytes)){++g.upload_reuses;if(stride==sizeof(NvGpuTri))++g.geometry_reuses;return 1;}
    }
    D3D11_BOX box={0,0,0,bytes,1,1};
    ID3D11DeviceContext_UpdateSubresource(g.context,(ID3D11Resource*)b->buffer,0,&box,data,0,0);
    if(!g.selftest){++g.upload_writes;g.upload_bytes+=bytes;}
    b->upload_valid=0;
    if(g.upload_cache && !g.selftest) {
        if(bytes>b->upload_capacity) {
            uint8_t *copy=realloc(b->upload_copy,bytes);
            if(!copy)return 1; /* GPU submission succeeded; only reuse is lost. */
            b->upload_copy=copy;b->upload_capacity=bytes;
        }
        memcpy(b->upload_copy,data,bytes);b->upload_valid=bytes;
    }
    return 1;
}
/* Per-batch, byte-exact state sharing. The hash only chooses one candidate;
 * equal GPU state records (including texture atlas versions) are verified in
 * full. Texture slots referenced by queued draws remain pinned until flush. */
static uint64_t gpu_state_hash(const NvGpuState *state)
{
    const uint32_t *words=(const uint32_t*)state;
    uint64_t hash=14695981039346656037ull;
    for(size_t i=0;i<sizeof *state/sizeof *words;i++){
        hash^=words[i];hash*=1099511628211ull;
    }
    return hash;
}
static size_t gpu_color_at(const NvGpuState *s, unsigned x, unsigned y)
{
    if (((s->format>>8)&15)==2) {
        unsigned w=1u<<((s->format>>16)&15), h=1u<<((s->format>>24)&15);
        return (size_t)swizzle_offset(x,y,w,h)*s->bpp;
    }
    return (size_t)y*s->pitch+(size_t)x*s->bpp;
}
static int gpu_flush(void);
static unsigned gpu_target_shape(unsigned width,unsigned height)
{
    if(width==640 && height==480)return 0;
    if(width==256 && height==256)return 1;
    if(width==128 && height==128)return 2;
    return 3;
}
/* A staging buffer is indexed by complete surface rows. Readback consumers
 * publish only the dirty rectangle; transfer its containing row span while
 * leaving all other staging bytes unobserved. The paired GPU snapshot remains
 * complete for feedback and target resume. */
static void gpu_copy_dirty_surface(ID3D11Resource *destination,ID3D11Resource *source,
                                   unsigned width,unsigned height,unsigned top,unsigned bottom)
{
    unsigned full=width*height*(unsigned)sizeof(GpuSurface);
    if(top>=bottom || top>=height){
        if(!g.selftest){++g.readback_time.surface_empty;g.readback_time.surface_bytes_avoided+=full;}
        return;
    }
    if(bottom>height)bottom=height;
    unsigned first=top*width*(unsigned)sizeof(GpuSurface);
    unsigned last=bottom*width*(unsigned)sizeof(GpuSurface);
    if(first==0 && last==full)
        ID3D11DeviceContext_CopyResource(g.context,destination,source);
    else {
        D3D11_BOX rows={first,0,0,last,1,1};
        ID3D11DeviceContext_CopySubresourceRegion(g.context,destination,0,first,0,0,source,0,&rows);
    }
    if(!g.selftest){
        ++g.readback_time.surface_copies;
        g.readback_time.surface_partial+=(last-first)<full;
        g.readback_time.surface_bytes+=last-first;
        g.readback_time.surface_bytes_avoided+=full-(last-first);
    }
}
/* Enqueue only: callers must flush rendering first and keep this target's
 * resources/stats unchanged until gpu_readback_current_mode consumes them. */
static double gpu_readback_enqueue_current(void)
{
    double begin=gpu_ms();
    unsigned timing=gpu_timing_batch_begin(1);if(timing)gpu_timing_phase_begin(NV_GPU_TIME_COPY,(uint64_t)g.target.width*g.target.height);
    if(g.native)gpu_copy_dirty_surface((ID3D11Resource*)g.surface.readback,(ID3D11Resource*)g.surface.buffer,
        g.target.width,g.target.height,g.dirty_top,g.dirty_bottom);
    else {
        ID3D11DeviceContext_CopyResource(g.context,(ID3D11Resource*)g.color.readback,(ID3D11Resource*)g.color.buffer);
        ID3D11DeviceContext_CopyResource(g.context,(ID3D11Resource*)g.depth.readback,(ID3D11Resource*)g.depth.buffer);
    }
    if(g.stats_required)ID3D11DeviceContext_CopyResource(g.context,(ID3D11Resource*)g.stats.readback,(ID3D11Resource*)g.stats.buffer);
    if(timing){gpu_timing_phase_end(NV_GPU_TIME_COPY);gpu_timing_batch_end(timing);}
    double elapsed=gpu_ms()-begin;
    if(!g.selftest)g.readback_time.enqueue_ms+=elapsed;
    return elapsed;
}
static int gpu_readback_current_mode(const char *reason,int enqueued,double enqueue_ms)
{
    if(!enqueued && !gpu_flush()){g.readback_failed=1;return 0;}
    if(!g.active)return 1;
    /* Keep legacy elapsed timers inclusive of this copy's enqueue, but never
     * include the older targets' wait/publication a second time. */
    double begin=gpu_ms()-enqueue_ms;
    D3D11_MAPPED_SUBRESOURCE color,depth,surface,stats;
    memset(&color,0,sizeof color);memset(&depth,0,sizeof depth);memset(&surface,0,sizeof surface);memset(&stats,0,sizeof stats);
    if(!enqueued)gpu_readback_enqueue_current();
    double map_begin=gpu_ms();
    HRESULT hr;
    if(g.native) {
        hr=ID3D11DeviceContext_Map(g.context,(ID3D11Resource*)g.surface.readback,0,D3D11_MAP_READ,0,&surface);
        if(FAILED(hr))goto fail;
    } else {
        hr=ID3D11DeviceContext_Map(g.context,(ID3D11Resource*)g.color.readback,0,D3D11_MAP_READ,0,&color);
        if(FAILED(hr))goto fail;
        hr=ID3D11DeviceContext_Map(g.context,(ID3D11Resource*)g.depth.readback,0,D3D11_MAP_READ,0,&depth);
        if(FAILED(hr))goto fail;
    }
    if(g.stats_required) {
        hr=ID3D11DeviceContext_Map(g.context,(ID3D11Resource*)g.stats.readback,0,D3D11_MAP_READ,0,&stats);
        if(FAILED(hr))goto fail;
    }else if(!g.selftest)++g.empty_stats_skips;
    double unpack_begin=gpu_ms();
    if(!g.selftest){
        g.readback_time.map_wait_ms+=unpack_begin-map_begin;
        g.target_shape[gpu_target_shape(g.target.width,g.target.height)].map_wait_ms+=unpack_begin-map_begin;
    }
    if(g.native) {
        const GpuSurface *pixels=surface.pData;
        for(unsigned y=g.dirty_top;y<g.dirty_bottom;y++) {
            unsigned at=y*g.target.width+g.dirty_left,count=g.dirty_right-g.dirty_left;
            int used=nv_gpu_surface_copy(pixels+at,g.color_upload+at,g.depth_upload+at,count,g.readback_simd);
            if(!g.selftest){g.readback_time.simd_rows+=used;g.readback_time.scalar_rows+=!used;g.readback_time.pixels+=count;}
        }
    }
    double capture_begin=gpu_ms();
    if(!g.selftest)g.readback_time.unpack_ms+=capture_begin-unpack_begin;
    const uint32_t *colors=g.native?g.color_upload:color.pData;
    const float *depths=g.native?g.depth_upload:depth.pData;
    if(s_capture) {
        uint32_t h[2]={g.target.width,g.target.height};size_t bytes=(size_t)h[0]*h[1]*4;
        if(gpu_capture_header(4,8+bytes*2)){gpu_capture_data(h,8);gpu_capture_data(colors,bytes);gpu_capture_data(depths,bytes);}
    }
    double copy_begin=gpu_ms();
    if(!g.selftest){g.map_ms+=copy_begin-begin;g.readback_time.capture_ms+=copy_begin-capture_begin;}
    for(unsigned y=g.dirty_top;y<g.dirty_bottom;y++) {
        unsigned at=y*g.target.width+g.dirty_left;
        unsigned count=g.dirty_right-g.dirty_left;
        /* Canonical mirrors describe the completed resident GPU buffers.
         * Subsequent guest waits need not force identical data back onto GPU. */
        if(!g.native) {
            memcpy(g.color_upload+at,colors+at,(size_t)count*4);
            memcpy(g.depth_upload+at,depths+at,(size_t)count*4);
        }
        if(g.target.bpp==4 && ((g.target.format>>8)&15)!=2)
            memcpy(g.host_color+(size_t)y*g.target.pitch+g.dirty_left*4,
                   colors+at,(size_t)count*4);
        else for(unsigned x=g.dirty_left;x<g.dirty_right;x++) {
            uint32_t v=colors[y*g.target.width+x];
            uint8_t *dest=g.host_color+gpu_color_at(&g.target,x,y);
            if(g.target.bpp==4)memcpy(dest,&v,4);
            else {uint16_t c=(uint16_t)(((v>>8)&0xF800)|((v>>5)&0x07E0)|((v>>3)&31));memcpy(dest,&c,2);}
        }
        if(g.host_depth)memcpy(g.host_depth+(size_t)y*g.depth_stride+g.dirty_left,
                              depths+at,(size_t)count*4);
    }
    double counters_begin=gpu_ms();
    if(!g.selftest){g.copy_ms+=counters_begin-copy_begin;g.readback_time.publish_ms+=counters_begin-copy_begin;}
    if(g.stats_required)for(unsigned bin=0;bin<NV_GPU_MAX_TILES;bin++) {
        uint32_t *v=(uint32_t*)stats.pData+bin*4;
        g.completed.depth_fail+=v[0];g.completed.pixels+=v[1];g.completed.zpass+=v[2];
        if((v[3]&0xFFFFFF)>(g.completed.pixel_max&0xFFFFFF))g.completed.pixel_max=v[3];
    }
    double unmap_begin=gpu_ms();
    if(!g.selftest)g.readback_time.counters_ms+=unmap_begin-counters_begin;
    if(g.native)ID3D11DeviceContext_Unmap(g.context,(ID3D11Resource*)g.surface.readback,0);
    else {
        ID3D11DeviceContext_Unmap(g.context,(ID3D11Resource*)g.color.readback,0);
        ID3D11DeviceContext_Unmap(g.context,(ID3D11Resource*)g.depth.readback,0);
    }
    if(stats.pData)ID3D11DeviceContext_Unmap(g.context,(ID3D11Resource*)g.stats.readback,0);
    g.active=0;g.completion.unpublished=0;
    if(!g.selftest){
        double end=gpu_ms(),ms=end-begin;++g.readbacks;g.readback_ms+=ms;
        unsigned shape=gpu_target_shape(g.target.width,g.target.height);
        ++g.target_shape[shape].readbacks;
        g.target_shape[shape].pixels+=(uint64_t)(g.dirty_right-g.dirty_left)*(g.dirty_bottom-g.dirty_top);
        ++g.readback_time.current;g.readback_time.unmap_ms+=end-unmap_begin;
        if(ms>=25 && g.slow_waits++<32)fprintf(stderr,"[GPU-WAIT-SLOW] reason=%s size=%ux%u triangles=%u wait_copy_ms=%.3f\n",reason,g.target.width,g.target.height,g.pending_triangles,ms);
        unsigned i;for(i=0;i<19;i++)if(!g.sync_reason[i].name[0] || !strcmp(g.sync_reason[i].name,reason))break;
        if(!g.sync_reason[i].name[0])snprintf(g.sync_reason[i].name,sizeof g.sync_reason[i].name,"%s",i==19?"other":reason);
        ++g.sync_reason[i].count;g.sync_reason[i].ms+=ms;
    }
    return 1;
fail:
    if(color.pData)ID3D11DeviceContext_Unmap(g.context,(ID3D11Resource*)g.color.readback,0);
    if(depth.pData)ID3D11DeviceContext_Unmap(g.context,(ID3D11Resource*)g.depth.readback,0);
    if(surface.pData)ID3D11DeviceContext_Unmap(g.context,(ID3D11Resource*)g.surface.readback,0);
    if(stats.pData)ID3D11DeviceContext_Unmap(g.context,(ID3D11Resource*)g.stats.readback,0);
    g.readback_failed=1;
    fprintf(stderr,"[GPU-RASTER] hardware readback failed hr=%08lX removed=%08lX\n",
        (unsigned long)hr,(unsigned long)ID3D11Device_GetDeviceRemovedReason(g.device));
    return 0;
}
static int gpu_readback_current(const char *reason)
{
    return gpu_readback_current_mode(reason,0,0);
}
/* Tiled aperture addresses alias contiguous RAM at different native pointers.
 * Main RAM is a separate mapping and must not be folded into that alias. */
static uintptr_t gpu_canonical_host(const void *address)
{
    uintptr_t at=(uintptr_t)address,base=(uintptr_t)xbox_GetMemoryOffset();
    if(at>=base && at-base>=0xF0000000ull && at-base<0xF4000000ull)
        at-=0x70000000ull;
    return at;
}
static int gpu_host_overlap(const void *address,size_t bytes,const void *other,size_t other_bytes)
{
    if(!address || !other || !bytes || !other_bytes)return 0;
    uintptr_t a=gpu_canonical_host(address),b=gpu_canonical_host(other);
    if(bytes>UINTPTR_MAX-a || other_bytes>UINTPTR_MAX-b)return 1;
    return a<b+other_bytes && b<a+bytes;
}
static size_t gpu_target_color_bytes(const NvGpuState *s)
{
    return ((s->format>>8)&15)==2?(size_t)s->width*s->height*s->bpp:
        (size_t)(s->height-1)*s->pitch+(size_t)s->width*s->bpp;
}
static size_t gpu_target_depth_bytes(const NvGpuState *s,unsigned stride)
{
    return ((size_t)(s->height-1)*stride+s->width)*sizeof(float);
}
static int gpu_pending_access(const void *address,size_t bytes)
{
    for(unsigned i=0;i<g.pending_count;i++) {
        GpuPendingReadback *p=&g.pending[i];
        if(gpu_host_overlap(address,bytes,p->host_color,gpu_target_color_bytes(&p->target)) ||
           gpu_host_overlap(address,bytes,p->host_depth,gpu_target_depth_bytes(&p->target,p->depth_stride))) {
            if(!g.selftest)++g.pending_dependencies;
            return gpu_pending_drain();
        }
    }
    return 1;
}
int nv_gpu_guest_access(uint64_t address,uint64_t bytes)
{
    /* Only the FIFO/executor owner calls this, after startup publication. */
    if(InterlockedCompareExchange(&s_startup,0,0)!=2 || !bytes)return 1;
    uintptr_t base=(uintptr_t)xbox_GetMemoryOffset();
    if(address>UINT32_MAX || bytes>0x100000000ull-address || address>UINTPTR_MAX-base || bytes>SIZE_MAX)
        return ((g.resume.active || g.completion.unpublished) && g.active?
            gpu_readback("completion-invalid-access"):gpu_pending_drain())?1:-1;
    const void *p=(const void*)(base+(uintptr_t)address);
    if(!gpu_pending_access(p,(size_t)bytes))return -1;
    /* A resumed target can have an older queued version at the same address.
     * A real CPU access must observe its latest active writes, including when
     * another dependency has already consumed that older queued version. */
    if((g.resume.active || g.completion.unpublished) && g.active &&
       (gpu_host_overlap(p,(size_t)bytes,g.host_color,gpu_target_color_bytes(&g.target)) ||
        gpu_host_overlap(p,(size_t)bytes,g.host_depth,gpu_target_depth_bytes(&g.target,g.depth_stride)))) {
        if(g.completion.unpublished && !g.selftest)++g.completion.access_drains;
        return gpu_readback_current(g.resume.active?"resumed-host-access":"completion-host-access")?1:-1;
    }
    return 1;
}
int nv_gpu_guest_read(uint64_t address,uint64_t bytes)
{
    if(InterlockedCompareExchange(&s_startup,0,0)!=2 || !bytes)return 1;
    uintptr_t base=(uintptr_t)xbox_GetMemoryOffset();
    if(address>UINT32_MAX || bytes>0x100000000ull-address || address>UINTPTR_MAX-base || bytes>SIZE_MAX)
        return gpu_readback("completion-chain-invalid")?1:-1;
    const void *p=(const void*)(base+(uintptr_t)address);
    if(!gpu_pending_access(p,(size_t)bytes))return -1;
    if(g.active && (gpu_host_overlap(p,(size_t)bytes,g.host_color,gpu_target_color_bytes(&g.target)) ||
       gpu_host_overlap(p,(size_t)bytes,g.host_depth,gpu_target_depth_bytes(&g.target,g.depth_stride))))
        return gpu_readback_current("completion-chain-read")?1:-1;
    return 1;
}
static int gpu_staging_buffer(ID3D11Buffer **buffer,unsigned *capacity,unsigned bytes)
{
    if(*buffer && *capacity==bytes)return 1;
    D3D11_BUFFER_DESC desc;memset(&desc,0,sizeof desc);
    desc.ByteWidth=bytes;desc.Usage=D3D11_USAGE_STAGING;desc.CPUAccessFlags=D3D11_CPU_ACCESS_READ;
    ID3D11Buffer *next=NULL;
    if(FAILED(ID3D11Device_CreateBuffer(g.device,&desc,NULL,&next)))return 0;
    if(*buffer)ID3D11Buffer_Release(*buffer);
    *buffer=next;*capacity=bytes;return 1;
}
static int gpu_queue_readback(void)
{
    if(!g.active)return 1;
    if(g.pending_count==NV_GPU_PENDING_READBACKS) {
        if(!g.selftest)++g.pending_capacity;
        if(!gpu_pending_drain())return -1;
    }
    if(!gpu_flush())return -1;
    GpuPendingReadback *p=&g.pending[g.pending_count];
    p->has_stats=g.stats_required;p->publish_surface=1;p->surface_copy_pending=0;
    /* Allocation failure is recoverable before copying: use normal sync. */
    if(!gpu_staging_buffer(&p->surface,&p->surface_bytes,g.surface.bytes) ||
       (p->has_stats && !gpu_staging_buffer(&p->stats,&p->stats_bytes,g.stats.bytes))) {
        g.readback_queue=0;
        fprintf(stderr,"[GPU-RASTER] queued staging allocation failed; synchronous target switches retained\n");
        return 0;
    }
    p->serial=0;
    if(g.bridge.enabled && (!g.selftest || g.bridge.testing) && !s_capture && g.target.bpp==4) {
        if(p->snapshot.bytes!=g.surface.bytes) {
            gpu_buffer_release(&p->snapshot);
            if(!gpu_buffer_create(&p->snapshot,g.surface.bytes,8,0)) {
                g.bridge.enabled=0;
                fprintf(stderr,"[GPU-TEXTURE-BRIDGE] snapshot allocation failed; CPU texture dependencies retained\n");
            }
        }
        if(g.bridge.enabled) {
            ID3D11DeviceContext_CopyResource(g.context,(ID3D11Resource*)p->snapshot.buffer,(ID3D11Resource*)g.surface.buffer);
            p->serial=++g.bridge.serial;
            if(!g.selftest)++g.bridge.snapshots;
        }
    }
    gpu_resume_save(p);
    int defer_surface=g.deferred_readback.enabled && p->serial && p->snapshot.buffer;
    double begin=gpu_ms();
    /* Profile a deferred surface at its actual transfer, not this empty
     * enqueue point (saved statistics still copy here independently). */
    unsigned timing=defer_surface?0:gpu_timing_batch_begin(1);if(timing)gpu_timing_phase_begin(NV_GPU_TIME_COPY,(uint64_t)g.target.width*g.target.height);
    /* An immutable paired snapshot already owns this version for texture
     * handoff/resume. Delay its host transfer until a real dependency drains
     * it: an exact resume may retire the older pixel publication entirely.
     * Without that snapshot, copy now before the live surface is reused. */
    if(defer_surface) {
        p->surface_copy_pending=1;
        if(!g.selftest)++g.deferred_readback.deferred;
    } else gpu_copy_dirty_surface((ID3D11Resource*)p->surface,(ID3D11Resource*)g.surface.buffer,
        g.target.width,g.target.height,g.dirty_top,g.dirty_bottom);
    if(p->has_stats)ID3D11DeviceContext_CopyResource(g.context,(ID3D11Resource*)p->stats,(ID3D11Resource*)g.stats.buffer);
    if(timing){gpu_timing_phase_end(NV_GPU_TIME_COPY);gpu_timing_batch_end(timing);}
    p->target=g.target;p->host_color=g.host_color;p->host_depth=g.host_depth;p->depth_stride=g.depth_stride;
    p->left=g.dirty_left;p->right=g.dirty_right;p->top=g.dirty_top;p->bottom=g.dirty_bottom;
    p->triangles=g.pending_triangles;++g.pending_count;
    if(!g.selftest){++g.queued_readbacks;g.pending_submit_ms+=gpu_ms()-begin;
        ++g.target_shape[gpu_target_shape(g.target.width,g.target.height)].queued;
        if(g.pending_count>g.pending_peak)g.pending_peak=g.pending_count;}
    g.active=g.resident=0;g.completion.unpublished=0;return 1;
}
static int gpu_pending_drain(void)
{
    /* Submit the current target too before the first Map can block. The
     * dependency only publishes saved copies, but later work can run ahead. */
    if(g.pending_count && !gpu_flush()){g.readback_failed=1;return 0;}
    /* Submit ALL required saved-version transfers before the first blocking
     * Map. Each source is its private immutable snapshot, never the current
     * render surface. Retired pixels have a later full owner, but their
     * independently saved visibility counters still complete below. */
    double enqueue_begin=gpu_ms();
    for(unsigned i=0;i<g.pending_count;i++) {
        GpuPendingReadback *p=&g.pending[i];
        if(!p->surface_copy_pending)continue;
        if(p->publish_surface) {
            unsigned timing=gpu_timing_batch_begin(1);
            if(timing)gpu_timing_phase_begin(NV_GPU_TIME_COPY,(uint64_t)p->target.width*p->target.height);
            gpu_copy_dirty_surface((ID3D11Resource*)p->surface,(ID3D11Resource*)p->snapshot.buffer,
                p->target.width,p->target.height,p->top,p->bottom);
            if(timing){gpu_timing_phase_end(NV_GPU_TIME_COPY);gpu_timing_batch_end(timing);}
            if(!g.selftest)++g.deferred_readback.issued;
        } else if(!g.selftest) {
            ++g.deferred_readback.avoided;g.deferred_readback.avoided_bytes+=p->surface_bytes;
        }
        p->surface_copy_pending=0;
    }
    if(!g.selftest)g.deferred_readback.enqueue_ms+=gpu_ms()-enqueue_begin;
    for(unsigned i=0;i<g.pending_count;i++) {
        GpuPendingReadback *p=&g.pending[i];
        /* A successful exact resume owns all earlier dirty pixels. Its
         * original GPU snapshot remains available, but CPU consumers must
         * observe the later full version. Keep only this version's counters. */
        if(!p->publish_surface && !p->has_stats) {
            if(!g.selftest)++g.resume.empty_retired;
            continue;
        }
        D3D11_MAPPED_SUBRESOURCE surface={0},stats={0};double begin=gpu_ms();HRESULT hr=0;
        if(p->publish_surface)hr=ID3D11DeviceContext_Map(g.context,(ID3D11Resource*)p->surface,0,D3D11_MAP_READ,0,&surface);
        if(SUCCEEDED(hr) && p->has_stats)hr=ID3D11DeviceContext_Map(g.context,(ID3D11Resource*)p->stats,0,D3D11_MAP_READ,0,&stats);
        if(FAILED(hr)) {
            if(surface.pData)ID3D11DeviceContext_Unmap(g.context,(ID3D11Resource*)p->surface,0);
            if(stats.pData)ID3D11DeviceContext_Unmap(g.context,(ID3D11Resource*)p->stats,0);
            g.readback_failed=1;
            fprintf(stderr,"[GPU-RASTER] queued target readback failed hr=%08lX removed=%08lX\n",
                (unsigned long)hr,(unsigned long)ID3D11Device_GetDeviceRemovedReason(g.device));
            return 0;
        }
        double copy_begin=gpu_ms();const GpuSurface *pixels=surface.pData;
        if(!g.selftest){
            g.readback_time.map_wait_ms+=copy_begin-begin;
            g.target_shape[gpu_target_shape(p->target.width,p->target.height)].map_wait_ms+=copy_begin-begin;
        }
        if(p->publish_surface)for(unsigned y=p->top;y<p->bottom;y++) {
            unsigned at=y*p->target.width+p->left,count=p->right-p->left;int used=0;
            if(p->target.bpp==4 && ((p->target.format>>8)&15)!=2){
                void *dest=p->host_color+(size_t)y*p->target.pitch+p->left*4u;
                void *z=p->host_depth?p->host_depth+(size_t)y*p->depth_stride+p->left:NULL;
                used=nv_gpu_surface_copy(pixels+at,dest,z,count,g.readback_simd);
            }else for(unsigned x=p->left;x<p->right;x++) {
                const GpuSurface *pixel=pixels+y*p->target.width+x;
                uint8_t *dest=p->host_color+gpu_color_at(&p->target,x,y);
                if(p->target.bpp==4)memcpy(dest,&pixel->color,4);
                else {uint32_t v=pixel->color;uint16_t packed=(uint16_t)(((v>>8)&0xF800)|((v>>5)&0x07E0)|((v>>3)&31));memcpy(dest,&packed,2);}
                if(p->host_depth)memcpy(p->host_depth+(size_t)y*p->depth_stride+x,&pixel->depth_bits,4);
            }
            if(!g.selftest){g.readback_time.simd_rows+=used;g.readback_time.scalar_rows+=!used;g.readback_time.pixels+=count;}
        }
        double counters_begin=gpu_ms();
        if(!g.selftest)g.readback_time.publish_ms+=counters_begin-copy_begin;
        if(p->has_stats)for(unsigned bin=0;bin<NV_GPU_MAX_TILES;bin++) {
            const uint32_t *v=(const uint32_t*)stats.pData+bin*4;
            g.completed.depth_fail+=v[0];g.completed.pixels+=v[1];g.completed.zpass+=v[2];
            if((v[3]&0xFFFFFF)>(g.completed.pixel_max&0xFFFFFF))g.completed.pixel_max=v[3];
        }
        double unmap_begin=gpu_ms();
        if(!g.selftest)g.readback_time.counters_ms+=unmap_begin-counters_begin;
        if(surface.pData)ID3D11DeviceContext_Unmap(g.context,(ID3D11Resource*)p->surface,0);
        if(stats.pData)ID3D11DeviceContext_Unmap(g.context,(ID3D11Resource*)p->stats,0);
        else if(!g.selftest)++g.empty_stats_skips;
        if(!g.selftest) {
            double end=gpu_ms(),ms=end-begin;++g.readbacks;g.readback_ms+=ms;
            unsigned shape=gpu_target_shape(p->target.width,p->target.height);
            ++g.target_shape[shape].readbacks;
            if(p->publish_surface)g.target_shape[shape].pixels+=(uint64_t)(p->right-p->left)*(p->bottom-p->top);
            ++g.readback_time.queued;g.readback_time.unmap_ms+=end-unmap_begin;
            g.map_ms+=copy_begin-begin;g.copy_ms+=end-copy_begin;
            const char *reason=p->publish_surface?"target-queued":"target-counters";
            g.resume.counter_reads+=!p->publish_surface;
            unsigned slot;for(slot=0;slot<19;slot++)if(!g.sync_reason[slot].name[0] || !strcmp(g.sync_reason[slot].name,reason))break;
            if(!g.sync_reason[slot].name[0])snprintf(g.sync_reason[slot].name,sizeof g.sync_reason[slot].name,"%s",slot==19?"other":reason);
            ++g.sync_reason[slot].count;g.sync_reason[slot].ms+=ms;
        }
    }
    g.pending_count=0;return 1;
}
static int gpu_readback(const char *reason)
{
    /* This full completion already requires both older and current targets.
     * Queue the current copy before older Maps can block. pending_drain only
     * submits immutable snapshot copies and publishes CPU mirrors: it cannot
     * change the current GPU surface or its saved statistics. CPU publication
     * remains oldest-to-newest, including resumed/aliased targets. Local state
     * prevents an early-copy flag surviving failures or another target bind. */
    int enqueued=g.readback_batch.enabled && g.pending_count && g.active;
    double enqueue_ms=0;
    if(enqueued) {
        if(!gpu_flush()){g.readback_failed=1;return 0;}
        enqueue_ms=gpu_readback_enqueue_current();
        if(!g.selftest) {
            ++g.readback_batch.batches;g.readback_batch.older_targets+=g.pending_count;
            g.readback_batch.current_bytes+=g.native?g.surface.bytes:g.color.bytes+g.depth.bytes;
            if(g.stats_required)g.readback_batch.current_bytes+=g.stats.bytes;
        }
    }
    if(!gpu_pending_drain())return 0;
    return gpu_readback_current_mode(reason,enqueued,enqueue_ms);
}
int nv_gpu_sync_named(NvGpuCount *count,const char *reason)
{
    memset(count,0,sizeof *count);
    if(InterlockedCompareExchange(&s_startup,0,0)!=2 && !gpu_startup_owned())return 1;
    if(!gpu_readback(reason?reason:"explicit"))return -1;
    *count=g.completed;memset(&g.completed,0,sizeof g.completed);
    /* Changing storage/backend is safe only after every accepted target has
     * completed, outside callers which already selected a draw/clear path. */
    if(g.auto_choose_compute) {
        g.native=0;g.resident=0;g.auto_choose_compute=0;
        fprintf(stderr,"[GPU-BACKEND] selected=compute reason=verified-live-batch native_ms=%.3f compute_ms=%.3f\n",g.auto_native_ms,g.auto_compute_ms);
    }
    return 1;
}
int nv_gpu_sync_completion(NvGpuCount *count)
{
    memset(count,0,sizeof *count);
    if(InterlockedCompareExchange(&s_startup,0,0)!=2 || !g.completion.enabled ||
       !g.native || g.native_diagnostics || g.selftest || s_capture || g.auto_enabled)return 0;
    /* The four-byte marker follows every submitted draw and saved target
     * copy in the immediate context. Fences need GPU completion, while their
     * framebuffer pixels can remain resident until a CPU consumer arrives. */
    if(!g.completion.marker.buffer && !gpu_buffer_create(&g.completion.marker,4,4,0)){
        g.completion.enabled=0;return 0;
    }
    if(!gpu_staging_buffer(&g.completion.staging,&g.completion.staging_bytes,4)){
        g.completion.enabled=0;return 0;
    }
    if(!gpu_flush()){g.readback_failed=1;return -1;}
    uint32_t serial=++g.completion.serial;
    ID3D11DeviceContext_UpdateSubresource(g.context,(ID3D11Resource*)g.completion.marker.buffer,0,NULL,&serial,0,0);
    ID3D11DeviceContext_CopyResource(g.context,(ID3D11Resource*)g.completion.staging,
                                     (ID3D11Resource*)g.completion.marker.buffer);
    D3D11_MAPPED_SUBRESOURCE mapped={0};double begin=gpu_ms();
    HRESULT hr=ID3D11DeviceContext_Map(g.context,(ID3D11Resource*)g.completion.staging,0,D3D11_MAP_READ,0,&mapped);
    if(FAILED(hr)){
        g.readback_failed=1;
        fprintf(stderr,"[GPU-COMPLETION] marker Map failed hr=%08lX removed=%08lX\n",
            (unsigned long)hr,(unsigned long)ID3D11Device_GetDeviceRemovedReason(g.device));return -1;
    }
    uint32_t observed=0;memcpy(&observed,mapped.pData,4);
    ID3D11DeviceContext_Unmap(g.context,(ID3D11Resource*)g.completion.staging,0);
    if(observed!=serial){g.readback_failed=1;fprintf(stderr,"[GPU-COMPLETION] marker mismatch\n");return -1;}
    ++g.completion.waits;g.completion.wait_ms+=gpu_ms()-begin;
    if(g.active){g.completion.unpublished=1;
        g.completion.avoided_pixels+=(uint64_t)g.target.width*g.target.height;}
    return 1;
}
int nv_gpu_sync_report(NvGpuCount *count)
{
    /* GET_REPORT consumes the visibility count, but no framebuffer pixels.
     * Older targets or queued guest completions still use the full path;
     * this branch only retains one live native surface with valid counters. */
    if(InterlockedCompareExchange(&s_startup,0,0)!=2 || !g.report_only_enabled || !g.native || !g.active ||
       g.pending_count || !g.report_shader ||
       g.native_diagnostics || g.selftest || s_capture || g.auto_enabled) {
        ++g.report_fallback;return 0;
    }
    if(!gpu_flush())return -1;
    if(!g.stats_required){++g.report_fallback;return 0;}
    ID3D11DeviceContext_CopyResource(g.context,(ID3D11Resource*)g.stats.readback,
                                     (ID3D11Resource*)g.stats.buffer);
    D3D11_MAPPED_SUBRESOURCE stats={0};double begin=gpu_ms();
    HRESULT hr=ID3D11DeviceContext_Map(g.context,(ID3D11Resource*)g.stats.readback,
                                       0,D3D11_MAP_READ,0,&stats);
    if(FAILED(hr)) {
        g.readback_failed=1;
        fprintf(stderr,"[GPU-RASTER] visibility counter readback failed hr=%08lX removed=%08lX\n",
            (unsigned long)hr,(unsigned long)ID3D11Device_GetDeviceRemovedReason(g.device));
        return -1;
    }
    NvGpuCount result=g.completed;
    const uint32_t *values=stats.pData;
    for(unsigned bin=0;bin<NV_GPU_MAX_TILES;bin++) {
        const uint32_t *v=values+bin*4;
        result.depth_fail+=v[0];result.pixels+=v[1];result.zpass+=v[2];
        if((v[3]&0xFFFFFF)>(result.pixel_max&0xFFFFFF))result.pixel_max=v[3];
    }
    ID3D11DeviceContext_Unmap(g.context,(ID3D11Resource*)g.stats.readback,0);
    /* The validated reset shader clears guest zpass in command order. Native
     * diagnostics are excluded above; no old diagnostics can be counted twice. */
    ID3D11UnorderedAccessView *uav[3]={NULL,NULL,g.stats.uav};
    ID3D11DeviceContext_CSSetShader(g.context,g.report_shader,NULL,0);
    ID3D11DeviceContext_CSSetUnorderedAccessViews(g.context,0,3,uav,NULL);
    ID3D11DeviceContext_Dispatch(g.context,(g.stats.bytes/16+63)/64,1,1);
    memset(uav,0,sizeof uav);ID3D11DeviceContext_CSSetUnorderedAccessViews(g.context,0,3,uav,NULL);
    *count=result;memset(&g.completed,0,sizeof g.completed);
    g.stats_required=0;
    ++g.report_only;g.report_bytes+=g.stats.bytes;g.report_wait_ms+=gpu_ms()-begin;
    return 1;
}
int nv_gpu_submit(void)
{
    if(InterlockedCompareExchange(&s_startup,0,0)!=2 && !gpu_startup_owned())return 1;
    return gpu_flush()?1:-1;
}
int nv_gpu_has_pending_work(void)
{
    return InterlockedCompareExchange(&s_startup,0,0)==2 &&
        (g.active || g.pending_count || g.queued_count);
}
int nv_gpu_sync(NvGpuCount *count){return nv_gpu_sync_named(count,"explicit");}
static int gpu_ranges_disjoint(uintptr_t a,size_t bytes,uintptr_t b,size_t other)
{
    if(!a || !b || !bytes || !other || bytes>UINTPTR_MAX-a || other>UINTPTR_MAX-b)return 0;
    return a+bytes<=b || b+other<=a;
}
int nv_gpu_cpu_write_disjoint(const void *address,size_t bytes)
{
    if(InterlockedCompareExchange(&s_startup,0,0)!=2 && !gpu_startup_owned())return 0;
    static int enabled=-1;
    if(enabled<0){const char *e=getenv("RECOMP_GPU_DISJOINT_CLEAR");enabled=!e || atoi(e)>0;}
    if(!enabled || !g.active)return 0;
    for(unsigned i=0;i<g.pending_count;i++) {
        GpuPendingReadback *p=&g.pending[i];
        if(gpu_host_overlap(address,bytes,p->host_color,gpu_target_color_bytes(&p->target)) ||
           gpu_host_overlap(address,bytes,p->host_depth,gpu_target_depth_bytes(&p->target,p->depth_stride)))return 0;
    }
    size_t color_bytes=((g.target.format>>8)&15)==2?
        (size_t)g.target.width*g.target.height*g.target.bpp:
        (size_t)(g.target.height-1)*g.target.pitch+(size_t)g.target.width*g.target.bpp;
    if(!gpu_ranges_disjoint((uintptr_t)address,bytes,(uintptr_t)g.host_color,color_bytes))return 0;
    if(g.host_depth && !gpu_ranges_disjoint((uintptr_t)address,bytes,(uintptr_t)g.host_depth,
       ((size_t)(g.target.height-1)*g.depth_stride+g.target.width)*sizeof(float)))return 0;
    if(!g.selftest)++g.disjoint_writes;
    return 1;
}
static int gpu_native_target(unsigned width,unsigned height)
{
    if(g.uav_only)return 1;
    if(g.raster_view && g.raster_width==width && g.raster_height==height)return 1;
    if(g.raster_view){ID3D11RenderTargetView_Release(g.raster_view);g.raster_view=NULL;}
    if(g.raster_target){ID3D11Texture2D_Release(g.raster_target);g.raster_target=NULL;}
    D3D11_TEXTURE2D_DESC desc;memset(&desc,0,sizeof desc);
    desc.Width=width;desc.Height=height;desc.MipLevels=desc.ArraySize=1;
    desc.Format=DXGI_FORMAT_R8G8B8A8_UNORM;desc.SampleDesc.Count=1;
    desc.Usage=D3D11_USAGE_DEFAULT;desc.BindFlags=D3D11_BIND_RENDER_TARGET;
    if(FAILED(ID3D11Device_CreateTexture2D(g.device,&desc,NULL,&g.raster_target)) ||
       FAILED(ID3D11Device_CreateRenderTargetView(g.device,(ID3D11Resource*)g.raster_target,NULL,&g.raster_view)))return 0;
    g.raster_width=width;g.raster_height=height;return 1;
}
/* Cache allocations, never unsynchronized pixel contents. The existing
 * target-change readback completes before a buffer enters this pool. Mirrors
 * are invalidated on every exchange and reloaded from coherent CPU memory. */
static int gpu_target_buffers(unsigned bytes)
{
    unsigned current=g.buffer_native?g.surface.bytes/2:g.color.bytes;
    if(g.buffer_native==g.native && current==bytes &&
       (g.native?g.surface.buffer!=NULL:(g.color.buffer && g.depth.buffer)))return 1;
    GpuTargetBuffers next;memset(&next,0,sizeof next);next.native=g.native;
    unsigned slot=0;uint64_t oldest=UINT64_MAX;int hit=0;
    const char *pool=getenv("RECOMP_GPU_TARGET_POOL");int cache=!pool || atoi(pool)>0;
    if(cache)for(unsigned i=0;i<3;i++) {
        GpuTargetBuffers *t=&g.target_spare[i];unsigned size=t->native?t->surface.bytes/2:t->color.bytes;
        if(size==bytes && t->native==g.native){slot=i;hit=1;break;}
        if(t->used<oldest){oldest=t->used;slot=i;}
    }
    double begin=gpu_ms();
    if(hit)next=g.target_spare[slot];
    else {
        if(g.native){if(!gpu_buffer_create(&next.surface,bytes*2,8,1))return 0;}
        else if(!gpu_buffer_create(&next.color,bytes,4,1) || !gpu_buffer_create(&next.depth,bytes,4,1)) {
            gpu_buffer_release(&next.color);gpu_buffer_release(&next.depth);return 0;
        }
        if(cache){gpu_buffer_release(&g.target_spare[slot].color);gpu_buffer_release(&g.target_spare[slot].depth);gpu_buffer_release(&g.target_spare[slot].surface);}
    }
    if(cache) {
        GpuTargetBuffers old={g.color,g.depth,g.surface,g.buffer_native,++g.target_tick};
        g.target_spare[slot]=old;
    } else {gpu_buffer_release(&g.color);gpu_buffer_release(&g.depth);gpu_buffer_release(&g.surface);}
    g.color=next.color;g.depth=next.depth;g.surface=next.surface;g.buffer_native=next.native;g.resident=0;
    if(!g.selftest){g.target_pool_hits+=hit;g.target_creates+=!hit;g.target_create_ms+=gpu_ms()-begin;}
    return 1;
}
#include "nv2a_gpu_target_resume.inc"
static int gpu_target(const NvGpuState *s,uint8_t *color,float *depth,unsigned stride)
{
    if(g.active && (g.host_color!=color || g.host_depth!=depth || g.depth_stride!=stride ||
       g.target.width!=s->width || g.target.height!=s->height || g.target.pitch!=s->pitch ||
       g.target.bpp!=s->bpp || g.target.format!=s->format)) {
        int queued=0;
        /* A bind with no writes has no new GPU pixels or visibility results.
         * Its CPU mirror is already authoritative, so do not queue a copy. */
        if(!g.selftest && !s_capture && !g.target_written && !g.queued_count && !g.stats_required){
            ++g.target_shape[gpu_target_shape(g.target.width,g.target.height)].empty_skips;
            g.active=g.resident=0;
        } else if(g.native && g.readback_queue && (!g.selftest || g.readback_queue_test) && !s_capture)
            queued=gpu_queue_readback();
        if(queued<0)return -1;
        if(g.active){
            if(!queued && !gpu_readback("target-change"))return -1;
        }
    }
    if(g.active)return 1;
    int resumed=gpu_resume_target(s,color,depth,stride);
    if(resumed){
        if(resumed>0){g.seed.snapshot_valid=0;g.seed.depth_dirty=1;}
        return resumed>0?1:0;
    }
    if(!gpu_pending_access(color,gpu_target_color_bytes(s)) ||
       (depth && !gpu_pending_access(depth,gpu_target_depth_bytes(s,stride))))return -1;
    double bind_begin=gpu_ms();
    if(g.native && !gpu_native_target(s->width,s->height))return 0;
    unsigned bytes=s->width*s->height*4;
    if(!gpu_target_buffers(bytes))return 0;
    int color_changed=!g.resident,depth_changed=!g.resident;
    for(unsigned y=0;y<s->height;y++) {
        unsigned at=y*s->width;
        if(s->bpp==4 && ((s->format>>8)&15)!=2) {
            const uint8_t *row=color+(size_t)y*s->pitch;size_t length=(size_t)s->width*4;
            if(!g.resident || memcmp(g.color_upload+at,row,length)) {
                color_changed=1;memcpy(g.color_upload+at,row,length);
            }
        }
        else for(unsigned x=0;x<s->width;x++) {
            const uint8_t *p=color+gpu_color_at(s,x,y);uint32_t v;
            if(s->bpp==4)memcpy(&v,p,4);
            else {uint16_t c;memcpy(&c,p,2);v=0xFF000000u|((c&0xF800u)<<8)|((c&0x07E0u)<<5)|((c&31u)<<3);}
            if(!g.resident || g.color_upload[at+x]!=v){color_changed=1;g.color_upload[at+x]=v;}
        }
        if(depth) {
            const float *row=depth+(size_t)y*stride;size_t length=(size_t)s->width*4;
            if(!g.resident || memcmp(g.depth_upload+at,row,length)) {
                depth_changed=1;memcpy(g.depth_upload+at,row,length);
            }
        } else for(unsigned x=0;x<s->width;x++)
            if(!g.resident || g.depth_upload[at+x]!=3.4e38f){depth_changed=1;g.depth_upload[at+x]=3.4e38f;}
    }
    if(g.native) {
        if(color_changed || depth_changed) {
            for(unsigned at=0;at<bytes/4;at++) {
                g.surface_upload[at].color=g.color_upload[at];
                memcpy(&g.surface_upload[at].depth_bits,g.depth_upload+at,4);
            }
            ID3D11DeviceContext_UpdateSubresource(g.context,(ID3D11Resource*)g.surface.buffer,0,NULL,g.surface_upload,0,0);
        }
    } else {
        if(color_changed)ID3D11DeviceContext_UpdateSubresource(g.context,(ID3D11Resource*)g.color.buffer,0,NULL,g.color_upload,0,0);
        if(depth_changed)ID3D11DeviceContext_UpdateSubresource(g.context,(ID3D11Resource*)g.depth.buffer,0,NULL,g.depth_upload,0,0);
    }
    g.stats_required=g.stats_initialized=0;
    if(gpu_capture_header(1,sizeof *s+(size_t)bytes*2)) {
        gpu_capture_data(s,sizeof *s);gpu_capture_data(g.color_upload,bytes);gpu_capture_data(g.depth_upload,bytes);
    }
    g.resident=1;
    if(!g.selftest){g.color_uploads+=color_changed;g.depth_uploads+=depth_changed;g.color_reuses+=!color_changed;g.depth_reuses+=!depth_changed;}
    g.target=*s;g.host_color=color;g.host_depth=depth;g.depth_stride=stride;
    g.active=1;g.resume.active=0;g.completion.unpublished=0;g.target_written=0;g.pending_triangles=0;g.dirty_left=s->width;g.dirty_top=s->height;g.dirty_right=g.dirty_bottom=0;
    g.seed.snapshot_valid=0;g.seed.depth_dirty=1;
    if(!g.selftest){++g.target_binds;
        ++g.target_shape[gpu_target_shape(s->width,s->height)].binds;
        double ms=gpu_ms()-bind_begin;
        if(ms>=25 && g.slow_targets++<32)fprintf(stderr,"[GPU-TARGET-SLOW] size=%ux%u bpp=%u prepare_ms=%.3f\n",s->width,s->height,s->bpp,ms);}
    return 1;
}
static void gpu_texture_release(GpuTexture *t)
{
free(t->raw);free(t->palette);free(t->decoded);memset(t,0,sizeof *t);
}
/* Short locality cache for the common case: a mesh submits adjacent pieces
 * using the same one to four textures. Indices are only hints. Slot metadata
 * and the original byte comparison still decide whether pixels can be reused. */
static int gpu_texture_identity(const GpuTexture *t,const NvGpuTexture *meta,
    const NvGpuTextureSource *source)
{
    return t->decoded && t->address==source->raw && t->palette_address==source->palette &&
           t->bytes==source->bytes && t->palette_bytes==source->palette_bytes &&
           t->meta.width==meta->width && t->meta.height==meta->height &&
           t->meta.cube==meta->cube && t->meta.format==meta->format;
}
static unsigned gpu_texture_bucket(const NvGpuTexture *meta,const NvGpuTextureSource *source)
{
    uintptr_t a=(uintptr_t)source->raw,b=(uintptr_t)source->palette;
    uint64_t value=(uint64_t)(a>>5)^(uint64_t)(a>>17)^(uint64_t)(b>>7)^
        ((uint64_t)meta->width<<16)^((uint64_t)meta->height<<32)^
        ((uint64_t)meta->format<<48)^source->bytes;
    value^=value>>29;value*=0x9e3779b97f4a7c15ull;return (unsigned)(value>>56);
}
static void gpu_texture_promote(GpuTexture *slot)
{
    if(!g.texture_hot_enabled)return;
    unsigned index=(unsigned)(slot-g.cache),end=0;
    NvGpuTextureSource key={0};
    key.raw=slot->address;key.palette=slot->palette_address;key.bytes=slot->bytes;
    g.texture_index[gpu_texture_bucket(&slot->meta,&key)]=(uint8_t)(index+1);
    while(end<g.texture_hot_count && g.texture_hot[end]!=index)++end;
    if(end==4)end=3;
    else if(end==g.texture_hot_count && g.texture_hot_count<4)++g.texture_hot_count;
    for(unsigned i=end;i>0;i--)g.texture_hot[i]=g.texture_hot[i-1];
    g.texture_hot[0]=index;
}
static GpuTexture *gpu_texture(const NvGpuTexture *meta,const NvGpuTextureSource *source)
{
    GpuTexture *slot=NULL;uint64_t oldest=UINT64_MAX;
    size_t texels=(size_t)meta->width*meta->height*(meta->cube?6:1);
    if(!texels || texels>4*1024*1024 || !source->raw || !source->bytes ||
       source->bytes>32*1024*1024 || source->palette_bytes>1024 || !source->decode)return NULL;
    if(g.texture_hot_enabled)for(unsigned i=0;i<g.texture_hot_count;i++) {
        GpuTexture *t=&g.cache[g.texture_hot[i]];
        if(gpu_texture_identity(t,meta,source)){
            slot=t;if(!g.selftest)++g.texture_hot_hits;break;
        }
    }
    if(!slot && g.texture_hot_enabled){
        unsigned found=g.texture_index[gpu_texture_bucket(meta,source)];
        if(found && gpu_texture_identity(&g.cache[found-1],meta,source)){
            slot=&g.cache[found-1];if(!g.selftest)++g.texture_index_hits;
        }
    }
    if(!slot){if(!g.selftest)++g.texture_full_scans;
    for(unsigned i=0;i<128;i++) {
        GpuTexture *t=&g.cache[i];
        if(gpu_texture_identity(t,meta,source)){slot=t;break;}
        if(t->used!=g.tick && t->used<oldest){oldest=t->used;slot=t;}
    }}
    if(!slot)return NULL;
    GpuPendingReadback *bridge=gpu_bridge_source(meta,source);
    if(bridge && !gpu_bridge_prepare((unsigned)texels)) {
        g.bridge.enabled=0;
        fprintf(stderr,"[GPU-TEXTURE-BRIDGE] pack allocation failed; coherent CPU fallback\n");
        if(!gpu_pending_drain())return NULL;
        bridge=NULL;
    }
    if(bridge && slot->decoded && slot->gpu_serial==bridge->serial &&
       slot->address==source->raw && slot->bytes==source->bytes &&
       slot->meta.width==meta->width && slot->meta.height==meta->height &&
       slot->meta.format==meta->format && !slot->meta.cube) {
        slot->used=g.tick;if(!g.selftest)++g.bridge.hits;gpu_texture_promote(slot);return slot;
    }
    if(!bridge && !slot->gpu_serial && slot->decoded && slot->address==source->raw && slot->palette_address==source->palette &&
       slot->bytes==source->bytes && slot->palette_bytes==source->palette_bytes &&
       slot->meta.width==meta->width && slot->meta.height==meta->height &&
       slot->meta.cube==meta->cube && slot->meta.format==meta->format &&
       !memcmp(slot->raw,source->raw,source->bytes) &&
       (!source->palette_bytes || !memcmp(slot->palette,source->palette,source->palette_bytes))) {
        if(!g.selftest)++g.texture_hits;
        slot->used=g.tick;gpu_texture_promote(slot);return slot;
    }
    if(slot->queued && !gpu_flush())return NULL;
    gpu_texture_release(slot);
    /* Keep decoded host/GPU texture storage bounded to about 128 MiB each. */
    for(;;) {
        size_t total=texels*4;GpuTexture *victim=NULL;oldest=UINT64_MAX;
        for(unsigned i=0;i<128;i++) {
            GpuTexture *t=&g.cache[i];total+=t->decoded_bytes;
            if(t->decoded && t->used!=g.tick && t->used<oldest){oldest=t->used;victim=t;}
        }
        if(total<=128u*1024u*1024u)break;
        if(!victim)return NULL;
        if(victim->queued && !gpu_flush())return NULL;
        gpu_texture_release(victim);
    }
    double decode_begin=gpu_ms();
    slot->decoded=bridge?calloc(texels,4):malloc(texels*4);
    if(!bridge)slot->raw=malloc(source->bytes);
    if(source->palette_bytes)slot->palette=malloc(source->palette_bytes);
    if((!bridge && !slot->raw) || !slot->decoded || (source->palette_bytes && !slot->palette))goto fail;
    if(!bridge) {
        memcpy(slot->raw,source->raw,source->bytes);
        if(source->palette_bytes)memcpy(slot->palette,source->palette,source->palette_bytes);
        int bulk=g.bulk_texture && source->decode_image &&
            source->decode_image(source->opaque,slot->raw,source->bytes,slot->decoded,texels);
        if(!bulk) {
            for(unsigned face=0;face<(meta->cube?6u:1u);face++)
                for(unsigned y=0;y<meta->height;y++)for(unsigned x=0;x<meta->width;x++) {
                    uint32_t v;
                    if(!source->decode(source->opaque,face,x,y,&v))goto fail;
                    slot->decoded[((size_t)face*meta->height+y)*meta->width+x]=v;
                }
        }
        if(!g.selftest){if(bulk){++g.bulk_images;g.bulk_texels+=texels;}else ++g.scalar_images;}
    }
    double decoded=gpu_ms();if(!g.selftest)g.decode_ms+=decoded-decode_begin;
    /* First-fit variable ranges in a persistent GPU atlas. Only a cache
     * update uploads bytes; sampling has one SRV and no resource switch. */
    unsigned base=0;
    for(;;) {
        base=0;
        for(unsigned pass=0;pass<128;pass++) {
            unsigned end=base;int overlap=0;
            for(unsigned i=0;i<128;i++) {
                GpuTexture *t=&g.cache[i];unsigned length=(unsigned)(t->decoded_bytes/4);
                if(t==slot || !length)continue;
                if(base<t->atlas_base+length && base+texels>t->atlas_base) {
                    if(t->atlas_base+length>end)end=t->atlas_base+length;
                    overlap=1;
                }
            }
            if(!overlap)break;
            base=end;
        }
        if((size_t)base+texels<=32u*1024u*1024u)break;
        GpuTexture *victim=NULL;oldest=UINT64_MAX;
        for(unsigned i=0;i<128;i++) {
            GpuTexture *t=&g.cache[i];
            if(t!=slot && t->decoded && t->used!=g.tick && t->used<oldest){oldest=t->used;victim=t;}
        }
        if(!victim)goto fail;
        if(victim->queued && !gpu_flush())goto fail;
        gpu_texture_release(victim);
    }
    D3D11_BOX box={base*4,0,0,(unsigned)(base+texels)*4,1,1};
    if(bridge) {
        if(!gpu_bridge_pack(bridge,base,(unsigned)texels))goto fail;
        slot->gpu_serial=bridge->serial;
    } else ID3D11DeviceContext_UpdateSubresource(g.context,(ID3D11Resource*)g.atlas.buffer,0,&box,slot->decoded,0,0);
    slot->atlas_base=base;
    slot->meta=*meta;slot->address=source->raw;slot->palette_address=source->palette;
    slot->bytes=source->bytes;slot->palette_bytes=source->palette_bytes;
    slot->decoded_bytes=texels*4;slot->used=g.tick;slot->version=g.tick;
    if(!g.selftest){
        ++g.uploads;++g.texture_updates;double now=gpu_ms();g.atlas_ms+=now-decoded;
        if(now-decode_begin>50)fprintf(stderr,"[GPU-TEXTURE-SLOW] size=%ux%u format=%02X decode_ms=%.3f atlas_ms=%.3f total_ms=%.3f\n",
            meta->width,meta->height,meta->format,decoded-decode_begin,now-decoded,now-decode_begin);
    }
    gpu_texture_promote(slot);return slot;
fail:
    gpu_texture_release(slot);return NULL;
}
static int gpu_initialize(void);
static int gpu_selftest(void);
static int gpu_material_selftest(void);
static unsigned gpu_literal_program(unsigned triangle);
static void gpu_literal_prepare(void);
static void gpu_literal_report(void);
static int gpu_mixed_selftest(void);
static int gpu_store_selftest(void);
static int gpu_report_selftest(void);
static int gpu_clear_selftest(void);
static int gpu_queued_readback_selftest(void);
int nv_gpu_available(void)
{
    LONG status=InterlockedCompareExchange(&s_startup,0,0);
    if(status==2)return 1;
    if(status==3)return 0;
    if(status==1)return gpu_startup_owned() && g.selftest && g.init==2;
    const char *enabled=getenv("RECOMP_GPU_RASTER");
    if(!enabled || atoi(enabled)<=0) {InterlockedExchange(&s_startup,3);return 0;}
    if(InterlockedCompareExchange(&s_startup,1,0)!=0)return 0;
    const char *async=getenv("RECOMP_GPU_ASYNC");
    if(async && atoi(async)<=0) {
        gpu_prepare_thread(NULL);return InterlockedCompareExchange(&s_startup,0,0)==2;
    }
    HANDLE thread=CreateThread(NULL,0,gpu_prepare_thread,NULL,0,NULL);
    if(!thread) {
        fprintf(stderr,"[GPU-RASTER] background setup thread failed; CPU rendering retained\n");
        InterlockedExchange(&s_startup,3);return 0;
    }
    CloseHandle(thread);
    fprintf(stderr,"[GPU-RASTER] preparing hardware in background; guest renders on CPU until ready\n");
    return 0;
}
/* Queue across guest draw/state changes; one tile owner replays all states
 * in original order. Cache slots stay pinned until their queued users dispatch.
 * Flush submits commands, while readback is still reserved for CPU/guest waits. */
/* Copy once before this batch; later depth writes can only tighten the bound.
 * No Map, CPU readback/wait, explicit Flush or guest completion is added. */
#include "nv2a_gpu_material_usage.inc"
static void gpu_depth_seed_prepare(void)
{
    if(!g.seed.enabled)return;
    uint32_t control[4]={0};
    int useful=0;
    unsigned prefix=g.seed.prefix_enabled?
        nv_gpu_depth_seed_prefix(g.queued_state,g.state_count,g.queued,g.queued_count,&useful):
        (nv_gpu_depth_seed_batch_safe(g.queued_state,g.state_count) &&
         nv_gpu_depth_seed_batch_useful(g.queued_state,g.state_count)?(useful=1,g.queued_count):0);
    if(!useful || !prefix){
        if(!g.selftest)++g.seed.unsafe;
    }else if(!g.selftest && !g.target_written){++g.seed.cold;
    }else if(!g.selftest && prefix<128){++g.seed.small_batches;
    }else{
        unsigned bytes=g.surface.bytes;
        if(g.seed.image.bytes!=bytes){
            GpuBuffer next={0};
            if(gpu_buffer_create(&next,bytes,sizeof(GpuSurface),0)){
                gpu_buffer_release(&g.seed.image);g.seed.image=next;
            }else if(!g.selftest)++g.seed.allocation_failures;
        }
        if(g.seed.image.bytes==bytes){
            int reuse=g.seed.reuse_enabled && g.seed.snapshot_valid && !g.seed.depth_dirty;
            if(!reuse){
                gpu_timing_phase_begin(NV_GPU_TIME_COPY,(uint64_t)g.target.width*g.target.height);
                ID3D11DeviceContext_CopyResource(g.context,(ID3D11Resource*)g.seed.image.buffer,(ID3D11Resource*)g.surface.buffer);
                gpu_timing_phase_end(NV_GPU_TIME_COPY);
                g.seed.snapshot_valid=1;g.seed.depth_dirty=0;
                if(!g.selftest){++g.seed.copies;g.seed.bytes+=bytes;}
            }else if(!g.selftest){++g.seed.reuses;g.seed.reused_bytes+=bytes;}
            control[0]=1;control[1]=prefix;
            if(!g.selftest && prefix<g.queued_count){++g.seed.partial_batches;g.seed.partial_triangles+=prefix;}
        }
    }
    if(g.seed.control_value!=control[1]){
        ID3D11DeviceContext_UpdateSubresource(g.context,(ID3D11Resource*)g.seed.control.buffer,0,NULL,control,0,0);
        g.seed.control_value=control[1];
    }
    if(!g.selftest)++g.seed.batches;
}
/* Consecutive short material runs can use the already-validated general ROV
 * program. It reads each triangle's own state; the indexed triangle order and
 * all target/visibility boundaries remain exactly as submitted. Large runs
 * retain their specialized pixel shaders. */
static unsigned gpu_native_span(unsigned first,int *simple,unsigned *literal,int *fused)
{
    unsigned end=first+1;
    *simple=g.queued_kind[first]!=0;*literal=gpu_literal_program(first);*fused=0;
    while(end<g.queued_count && (g.queued_kind[end]!=0)==*simple &&
          gpu_literal_program(end)==*literal)++end;
    if(!g.micro_fuse.enabled || !g.native_batch || end-first>32u)return end;
    unsigned combined=end,runs=1;
    while(combined<g.queued_count){
        unsigned next=combined+1,kind=g.queued_kind[combined]!=0,id=gpu_literal_program(combined);
        while(next<g.queued_count && (g.queued_kind[next]!=0)==kind &&
              gpu_literal_program(next)==id)++next;
        if(next-combined>32u || next-first>128u)break;
        combined=next;++runs;
    }
    if(runs>1){*simple=0;*literal=0;*fused=(int)runs;return combined;}
    return end;
}
static int gpu_flush(void)
{
    if(!g.queued_count)return 1;
    /* Benchmark only a fresh target's real immutable queue. No prior GPU
     * writes may be missing from its CPU input, and captures remain exact. */
    if(g.auto_enabled && !g.auto_done && !g.auto_busy && !g.selftest && g.native &&
       !s_capture && !g.pending_count && !g.target_written && !g.pending_triangles &&
       g.queued_count>=256 && g.target.width>=128 && g.target.height>=128)
        if(gpu_backend_benchmark()<0)return 0;
    double begin=gpu_ms();unsigned tiles=0,refs=0;
    GpuBuffer *triangles=&g.triangles,*states=&g.state,*references=&g.references,*tile_data=&g.tiles,*stage_masks=&g.material.stage_masks;
    /* Rotate complete immutable input sets; the immediate context retains
     * command order and handles wraparound hazards through UpdateSubresource.
     * This is bounded streaming, not unsynchronized CPU writes to live VRAM. */
    if(g.upload_stream && !g.selftest) {
        unsigned slot=g.upload_cursor++%NV_GPU_UPLOAD_SLOTS;++g.upload_rotations;
        if(slot){GpuUploadSlot *next=&g.upload_spare[slot-1];
            triangles=&next->triangles;states=&next->state;references=&next->references;tile_data=&next->tiles;stage_masks=&next->stage_masks;}
    }
    /* The graphics rasterizer creates pixel candidates without tile lists. */
    if((!g.native && (!nv_gpu_bin(g.queued,g.queued_count,g.target.width,g.target.height,
                   g.tile_upload,&tiles,g.refs,NV_GPU_MAX_REFERENCES,&refs) ||
       !gpu_upload(references,g.refs,refs*4,4) ||
       !gpu_upload(tile_data,g.tile_upload,tiles*sizeof(NvGpuTile),sizeof(NvGpuTile)))) ||
       !(raw.count?gpu_raw_upload(triangles):gpu_upload(triangles,g.queued,g.queued_count*sizeof(NvGpuTri),sizeof(NvGpuTri))) ||
       !gpu_upload(states,g.queued_state,g.state_count*sizeof(NvGpuState),sizeof(NvGpuState))) {
        fprintf(stderr,"[GPU-RASTER] queued upload failed; cannot complete accepted draws\n");
        return 0;
    }
    ID3D11ShaderResourceView *material_srv=gpu_material_prepare(stage_masks);
    double upload_end=gpu_ms();
    unsigned timing=gpu_timing_batch_begin(0);
    if(raw.count && !gpu_raw_dispatch(triangles,states)){gpu_timing_batch_end(timing);return 0;}
    double raw_end=gpu_ms();
    unsigned commands=0,simple_commands=0,simple_triangles=0;
    int mixed=0;if(g.native)for(unsigned i=1;i<g.queued_count;i++)if(g.queued_kind[i]!=g.queued_kind[0]){mixed=1;break;}
    if(s_capture) {
        for(unsigned q=0;q<g.state_count;q++)for(unsigned st=0;st<4;st++) {
            NvGpuTexture *meta=&g.queued_state[q].tex[st];
            if(!meta->valid)continue;
            unsigned slot=meta->cache_slot;GpuTexture *t=&g.cache[slot];
            if(s_capture_versions[slot]==t->version)continue;
            uint32_t h[3]={slot,t->atlas_base,(uint32_t)(t->decoded_bytes/4)};
            if(gpu_capture_header(2,12+t->decoded_bytes)){gpu_capture_data(h,12);gpu_capture_data(t->decoded,t->decoded_bytes);s_capture_versions[slot]=t->version;}
        }
        uint32_t h[3]={g.native?((g.coverage?4u:0u)|(mixed?9u:(g.queued_kind[0]?3u:1u))):(g.queue_simple?2u:0u),g.state_count,g.queued_count};
        size_t state_bytes=(size_t)g.state_count*sizeof(NvGpuState),tri_bytes=(size_t)g.queued_count*sizeof(NvGpuTri);
        if(gpu_capture_header(3,12+state_bytes+tri_bytes)){gpu_capture_data(h,12);gpu_capture_data(g.queued_state,state_bytes);gpu_capture_data(g.queued,tri_bytes);}
    }
    /* Quiet native shaders can write only the guest visibility counter.
     * Preserve every state's enable, including earlier flushed batches. A
     * target with no counter-writing draws has a provably zero contribution;
     * stale storage is never read. Initialize lazily before the FIRST writer. */
    int stats_needed=!g.skip_empty_stats || !g.native || g.native_diagnostics || g.selftest || s_capture!=NULL;
    for(unsigned i=0;i<g.state_count && !stats_needed;i++)stats_needed=g.queued_state[i].zpass_enable!=0;
    if(stats_needed) {
        if(!g.stats_initialized) {
            static const uint32_t zeros[NV_GPU_MAX_TILES*4]={0};
            ID3D11DeviceContext_UpdateSubresource(g.context,(ID3D11Resource*)g.stats.buffer,0,NULL,zeros,0,0);
            g.stats_initialized=1;if(!g.selftest)++g.stats_initializations;
        }
        g.stats_required=1;
    }
    ID3D11ShaderResourceView *srv[5]={triangles->srv,references->srv,tile_data->srv,states->srv,g.atlas.srv};
    ID3D11UnorderedAccessView *uav[4]={g.native?g.surface.uav:g.color.uav,g.native?NULL:g.depth.uav,g.stats.uav,NULL};
    if(g.native) {
        gpu_literal_prepare();
        gpu_depth_seed_prepare();
        ID3D11DeviceContext_PSSetShaderResources(g.context,7,1,&material_srv);
        ID3D11ShaderResourceView *seed_srv[2]={g.seed.enabled?g.seed.image.srv:NULL,g.seed.enabled?g.seed.control.srv:NULL};
        ID3D11DeviceContext_PSSetShaderResources(g.context,5,2,seed_srv);
        D3D11_VIEWPORT view={0,0,(float)g.target.width,(float)g.target.height,0,1};
        ID3D11DeviceContext_IASetInputLayout(g.context,NULL);
        if(g.native_batch)ID3D11DeviceContext_IASetIndexBuffer(g.context,g.vertex_ids,DXGI_FORMAT_R32_UINT,0);
        ID3D11DeviceContext_IASetPrimitiveTopology(g.context,D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        ID3D11DeviceContext_RSSetViewports(g.context,1,&view);
        if(g.coverage){D3D11_RECT scissor={0,0,(LONG)g.target.width,(LONG)g.target.height};ID3D11DeviceContext_RSSetScissorRects(g.context,1,&scissor);}
        ID3D11DeviceContext_RSSetState(g.context,g.coverage?(ID3D11RasterizerState*)g.coverage_rasterizer:g.rasterizer);
        ID3D11DeviceContext_VSSetShader(g.context,g.coverage?g.coverage_vertex_shader:g.vertex_shader,NULL,0);
        ID3D11DeviceContext_VSSetShaderResources(g.context,0,5,srv);
        ID3D11DeviceContext_PSSetShaderResources(g.context,0,5,srv);
        ID3D11UnorderedAccessView *native_uav[4]={NULL,g.surface.uav,g.stats.uav,g.startup_trace.uav};
        if(g.uav_only)ID3D11DeviceContext_OMSetRenderTargetsAndUnorderedAccessViews(g.context,0,NULL,NULL,0,4,native_uav,NULL);
        else ID3D11DeviceContext_OMSetRenderTargetsAndUnorderedAccessViews(g.context,1,&g.raster_view,NULL,1,3,native_uav+1,NULL);
        /* One immutable upload spans adjacent material classes. Indexed IDs
         * address the original triangle/state without modifying pixel shaders. */
        unsigned factor=g.coverage?3u:6u;
        if(gpu_timing_material_wants()){
            unsigned runs=0;
            for(unsigned first=0;first<g.queued_count;){
                int simple,fused;unsigned literal;
                unsigned end=gpu_native_span(first,&simple,&literal,&fused);
                ++runs;first=end;
            }
            gpu_timing_material_select(runs);
        }
        gpu_timing_phase_begin(NV_GPU_TIME_PIXELS,g.queued_count);
        for(unsigned first=0;first<g.queued_count;){
            int simple,fused;unsigned literal;
            unsigned end=gpu_native_span(first,&simple,&literal,&fused);
            ID3D11PixelShader *pixel=simple?g.simple_pixel_shader:(literal?g.literal.shader[literal-1]:g.pixel_shader);
            ID3D11DeviceContext_PSSetShader(g.context,pixel,NULL,0);
            unsigned material_token=gpu_timing_material_begin(simple?1u:(literal?literal+1u:0u),end-first);
            /* Non-indexed SV_VertexID restarts at zero. */
            if(g.native_batch)ID3D11DeviceContext_DrawIndexed(g.context,factor*(end-first),factor*first,0);
            else ID3D11DeviceContext_Draw(g.context,factor*(end-first),0);
            gpu_timing_material_end(material_token);
            gpu_shade_probe_replay(material_token,simple,first,end,factor);
            ++commands;if(simple){++simple_commands;simple_triangles+=end-first;}
            if(!g.selftest){
                if(fused){++g.micro_fuse.fused_draws;g.micro_fuse.runs_saved+=(unsigned)fused-1;g.micro_fuse.triangles+=end-first;}
                if(literal){++g.literal.draws;g.literal.triangles+=end-first;g.literal.class_triangles[literal-1]+=end-first;}
                else if(!simple)++g.literal.generic_draws;
            }
            first=end;
        }
        gpu_timing_phase_end(NV_GPU_TIME_PIXELS);
        memset(srv,0,sizeof srv);memset(uav,0,sizeof uav);
        ID3D11DeviceContext_VSSetShaderResources(g.context,0,5,srv);
        ID3D11DeviceContext_PSSetShaderResources(g.context,0,5,srv);
        memset(seed_srv,0,sizeof seed_srv);ID3D11DeviceContext_PSSetShaderResources(g.context,5,2,seed_srv);
        material_srv=NULL;ID3D11DeviceContext_PSSetShaderResources(g.context,7,1,&material_srv);
        ID3D11DeviceContext_OMSetRenderTargetsAndUnorderedAccessViews(g.context,0,NULL,NULL,0,4,uav,NULL);
    } else {
    ID3D11DeviceContext_CSSetShader(g.context,g.queue_simple?g.simple_shader:g.shader,NULL,0);
    ID3D11DeviceContext_CSSetShaderResources(g.context,0,5,srv);
    ID3D11DeviceContext_CSSetUnorderedAccessViews(g.context,0,3,uav,NULL);
    gpu_timing_phase_begin(NV_GPU_TIME_PIXELS,g.queued_count);
    ID3D11DeviceContext_Dispatch(g.context,tiles*4,1,1);
    gpu_timing_phase_end(NV_GPU_TIME_PIXELS);
    commands=1;if(g.queue_simple){simple_commands=1;simple_triangles=g.queued_count;}
    memset(srv,0,sizeof srv);memset(uav,0,sizeof uav);
    ID3D11DeviceContext_CSSetShaderResources(g.context,0,5,srv);
    ID3D11DeviceContext_CSSetUnorderedAccessViews(g.context,0,3,uav,NULL);
    }
    gpu_timing_batch_end(timing);
    /* After this ordered batch, the seed remains exact only if no draw could
     * write depth. Alpha/coverage failures may leave depth unchanged; marking
     * a possible writer dirty is conservative and preserves bitwise output. */
    if(g.native && g.seed.enabled)for(unsigned i=0;i<g.state_count;i++)
        if(g.queued_state[i].depth_available && g.queued_state[i].depth_mask){g.seed.depth_dirty=1;break;}
    g.target_written=1;g.pending_triangles+=g.queued_count;
    if(!g.selftest){
        double end=gpu_ms();++g.flush_time.batches;g.flush_time.upload_ms+=upload_end-begin;
        g.flush_time.raw_ms+=raw_end-upload_end;g.flush_time.raster_ms+=end-raw_end;g.flush_time.total_ms+=end-begin;
        if(g.native && g.empty_reject && !g.native_diagnostics){++g.empty_reject_batches;g.empty_reject_triangles+=g.queued_count;}
        ++g.queue_uploads;g.dispatches+=commands;g.groups+=tiles*4;g.submit_ms+=end-begin;
        g.simple_dispatches+=simple_commands;g.simple_tris+=simple_triangles;}
    g.queued_count=g.state_count=g.queued_refs=0;
    memset(g.state_intern,0,sizeof g.state_intern);
    for(unsigned i=0;i<128;i++)g.cache[i].queued=0;
    gpu_raw_reset();
    return 1;
}
static int gpu_clear_skip(unsigned reason)
{
    if(!g.selftest)++g.clear_skips[reason];return 0;
}
static int gpu_clear(unsigned left,unsigned top,unsigned right,unsigned bottom,unsigned mask,uint32_t color,float depth)
{
    if(!g.active)return gpu_clear_skip(0);
    if(!g.native || !g.clear_shader)return gpu_clear_skip(1);
    if(left>=right || top>=bottom || right>g.target.width || bottom>g.target.height)return gpu_clear_skip(5);
    double begin=gpu_ms();
    if(!gpu_flush())return -1;
    uint32_t parameters[9]={g.target.width,g.target.height,left,top,right,bottom,mask,color,0};
    memcpy(parameters+8,&depth,4);
    ID3D11DeviceContext_UpdateSubresource(g.context,(ID3D11Resource*)g.clear_parameters.buffer,0,NULL,parameters,0,0);
    ID3D11ShaderResourceView *srv[5]={g.clear_parameters.srv,NULL,NULL,NULL,NULL};
    ID3D11UnorderedAccessView *uav[3]={g.surface.uav,NULL,NULL};
    ID3D11DeviceContext_CSSetShader(g.context,g.clear_shader,NULL,0);
    ID3D11DeviceContext_CSSetShaderResources(g.context,0,5,srv);
    ID3D11DeviceContext_CSSetUnorderedAccessViews(g.context,0,3,uav,NULL);
    ID3D11DeviceContext_Dispatch(g.context,((right-left)*(bottom-top)+63)/64,1,1);
    memset(srv,0,sizeof srv);memset(uav,0,sizeof uav);
    ID3D11DeviceContext_CSSetShaderResources(g.context,0,5,srv);
    ID3D11DeviceContext_CSSetUnorderedAccessViews(g.context,0,3,uav,NULL);
    g.target_written=1;
    if(mask&2u)g.seed.depth_dirty=1;
    if(left<g.dirty_left)g.dirty_left=left;if(right>g.dirty_right)g.dirty_right=right;
    if(top<g.dirty_top)g.dirty_top=top;if(bottom>g.dirty_bottom)g.dirty_bottom=bottom;
    if(gpu_capture_header(8,sizeof parameters))gpu_capture_data(parameters,sizeof parameters);
    if(!g.selftest){++g.clear_commands;g.clear_ms+=gpu_ms()-begin;}
    return 1;
}
int nv_gpu_clear_color(const NvGpuState *s,uint8_t *color,
                       unsigned left,unsigned top,unsigned right,unsigned bottom,uint32_t value)
{
    if(InterlockedCompareExchange(&s_startup,0,0)!=2 && !gpu_startup_owned())return 0;
    if(!g.active)return gpu_clear_skip(0);
    if(color!=g.host_color)return gpu_clear_skip(2);
    if(s->width!=g.target.width || s->height!=g.target.height ||
       s->pitch!=g.target.pitch || s->bpp!=g.target.bpp || s->format!=g.target.format) {
        if(!g.selftest && g.clear_skips[3]<4)fprintf(stderr,"[GPU-CLEAR-LAYOUT] requested=%ux%u pitch=%u format=%08X active=%ux%u pitch=%u format=%08X\n",
            s->width,s->height,s->pitch,s->format,g.target.width,g.target.height,g.target.pitch,g.target.format);
        return gpu_clear_skip(3);
    }
    if(s->bpp==2)value=0xFF000000u|(value&0xF80000u)|(value&0x00FC00u)|(value&0x0000F8u);
    return gpu_clear(left,top,right,bottom,1,value,0);
}
int nv_gpu_clear_depth(float *depth,unsigned stride,float value)
{
    if(InterlockedCompareExchange(&s_startup,0,0)!=2 && !gpu_startup_owned())return 0;
    if(!g.active)return gpu_clear_skip(0);
    if(!depth || depth!=g.host_depth || stride!=g.depth_stride)return gpu_clear_skip(4);
    /* Match the existing CPU depth-cache clear, which clears the full cache. */
    return gpu_clear(0,0,g.target.width,g.target.height,2,0,value);
}
int nv_gpu_reset_report(void)
{
    if(!nv_gpu_available() || !g.report_shader)return 0;
    if(!gpu_pending_drain())return -1;
    int submitted=g.active;
    if(g.active) {
        if(!gpu_flush())return -1;
        if(g.stats_initialized) {
            ID3D11UnorderedAccessView *uav[3]={NULL,NULL,g.stats.uav};
            ID3D11DeviceContext_CSSetShader(g.context,g.report_shader,NULL,0);
            ID3D11DeviceContext_CSSetUnorderedAccessViews(g.context,0,3,uav,NULL);
            ID3D11DeviceContext_Dispatch(g.context,(g.stats.bytes/16+63)/64,1,1);
            memset(uav,0,sizeof uav);
            ID3D11DeviceContext_CSSetUnorderedAccessViews(g.context,0,3,uav,NULL);
        }
        if(g.skip_empty_stats && g.native && !g.native_diagnostics && !g.selftest && !s_capture)
            g.stats_required=0;
    }
    /* Target changes can accumulate counts before their publication. Those
     * visibility counts precede this reset too; retain pixel/depth diagnostics. */
    g.completed.zpass=0;
    uint32_t zero=0;if(gpu_capture_header(7,4))gpu_capture_data(&zero,4);
    if(!g.selftest && submitted)++g.report_resets;
    return 1;
}
static int gpu_draw_ordered(const NvGpuState *s,const NvGpuTri *tri,unsigned n,
                uint8_t *color,float *depth,unsigned stride,const NvGpuTextureSource texture[4],int raw_reserve,int raw_use_rc)
{
    if(!nv_gpu_available())return 0;
    if(!gpu_capture_begin())return -1;
    double begin=gpu_ms();unsigned refs=0;
    if(!color || !n || n>NV_GPU_MAX_TRIANGLES || !s->width || !s->height ||
       s->width>1024 || s->height>1024 || (s->bpp!=2 && s->bpp!=4))goto fallback;
    int simple=g.native?g.simple_pixel_shader!=NULL:g.simple_shader!=NULL;
    if(simple && !gpu_rc_simple_kind(s->rc)) {
        if(raw_reserve)simple=!raw_use_rc;
        else for(unsigned i=0;i<n;i++)if(tri[i].use_rc){simple=0;break;}
    }
    if(raw_reserve) {
        /* The old 1x1 placeholders contributed exactly one tile reference
         * each. GPU preparation writes their complete final records later. */
        refs=n;
        if(refs>NV_GPU_MAX_REFERENCES)return 0;
    } else for(unsigned i=0;i<n;i++) {
        const NvGpuTri *t=tri+i;
        if(t->minx<0 || t->miny<0 || t->maxx>(int)s->width || t->maxy>(int)s->height ||
           t->minx>=t->maxx || t->miny>=t->maxy)goto fallback;
        refs+=(unsigned)(((t->maxx-1)/16-t->minx/16+1)*((t->maxy-1)/16-t->miny/16+1));
        if(refs>NV_GPU_MAX_REFERENCES)goto fallback;
    }
    /* A completion token can leave the producer's guest pixels unpublished.
     * Exact ARGB feedback can use the existing private GPU snapshot and
     * texture bridge; it still retains a pending CPU publication obligation.
     * Depth, palettes, aliases, partial layouts and ambiguous older versions
     * continue through the coherent full readback. Check ALL sampled stages
     * before queuing so an unsupported second stage cannot see stale RAM. */
    if(g.completion.unpublished && g.active){
        int feedback=0,exact=g.completion.feedback_enabled && g.native && g.readback_queue && g.bridge.enabled &&
            !s_capture && !g.pending_count && g.target.bpp==4 &&
            ((color!=g.host_color || depth!=g.host_depth) || g.resume.enabled);
        size_t cb=gpu_target_color_bytes(&g.target);
        size_t db=g.host_depth?gpu_target_depth_bytes(&g.target,g.depth_stride):0;
        for(unsigned st=0;st<4;st++)if(s->tex[st].valid){
            const NvGpuTexture *meta=&s->tex[st];
            const NvGpuTextureSource *src=&texture[st];
            int color_hit=gpu_host_overlap(src->raw,src->bytes,g.host_color,cb);
            int depth_hit=gpu_host_overlap(src->raw,src->bytes,g.host_depth,db) ||
                gpu_host_overlap(src->palette,src->palette_bytes,g.host_depth,db);
            int palette_hit=gpu_host_overlap(src->palette,src->palette_bytes,g.host_color,cb);
            if(!color_hit && !depth_hit && !palette_hit)continue;
            feedback=1;
            if(depth_hit || palette_hit || !color_hit || !src->gpu_argb_layout ||
               src->palette_bytes || meta->cube ||
               gpu_canonical_host(src->raw)!=gpu_canonical_host(g.host_color) ||
               src->bytes!=cb || cb!=(size_t)g.target.width*g.target.height*4u ||
               meta->width!=g.target.width || meta->height!=g.target.height ||
               !((src->gpu_argb_layout==1 && meta->format==0x12 &&
                   ((g.target.format>>8)&15)!=2 && g.target.pitch==g.target.width*4u) ||
                 (src->gpu_argb_layout==2 && meta->format==0x06 &&
                   ((g.target.format>>8)&15)==2)))exact=0;
        }
        if(feedback){
            int queued=exact?gpu_queue_readback():0;
            if(queued<0)return -1;
            if(queued){if(!g.selftest)++g.completion.feedback_snapshots;}
            else {
                if(!g.selftest)++g.completion.feedback_fallbacks;
                if(!gpu_readback("completion-texture-read"))return -1;
            }
        }
    }
    double phase=gpu_ms();
    int target=gpu_target(s,color,depth,stride);
    if(!g.selftest)g.target_ms+=gpu_ms()-phase;
    if(target<0)return -1;
    if(!target)goto fallback;
    if((g.queued_count && (!g.native || !g.native_batch) && g.queue_simple!=simple) ||
       g.queued_count+n>NV_GPU_MAX_TRIANGLES || g.state_count==NV_GPU_MAX_STATES ||
       g.queued_refs+refs>NV_GPU_MAX_REFERENCES) {
        if(!gpu_flush())return -1;
    }
    for(unsigned i=0;i<4;i++)if(s->tex[i].valid) {
        if((!gpu_bridge_source(&s->tex[i],&texture[i]) && !gpu_pending_access(texture[i].raw,texture[i].bytes)) ||
           !gpu_pending_access(texture[i].palette,texture[i].palette_bytes))return -1;
    }
    ++g.tick;
    NvGpuState next=*s;phase=gpu_ms();
    for(int i=0;i<4;i++) {
        g.bound[i]=NULL;
        if(s->tex[i].valid) {
            g.bound[i]=gpu_texture(&s->tex[i],&texture[i]);
            if(!g.bound[i])goto fallback;
            next.tex[i].cache_slot=(uint32_t)(g.bound[i]-g.cache);
            next.tex[i].texel_base=g.bound[i]->atlas_base;
        }
    }
    if(!g.selftest)g.texture_ms+=gpu_ms()-phase;
    /* Texture replacement above may have dispatched the earlier queue. */
    g.queue_simple=simple;
    unsigned index=g.state_count;
    if(g.state_intern_enabled && !g.selftest){
        uint64_t hash=gpu_state_hash(&next);
        unsigned slot=(unsigned)hash&255u;
        unsigned old=g.state_intern[slot].index_plus_one;
        if(old && old<=g.state_count && g.state_intern[slot].hash==hash &&
           !memcmp(&next,&g.queued_state[old-1],sizeof next)){
            index=old-1;++g.state_intern_hits;
            g.state_intern_bytes_saved+=sizeof next;
        }else{
            index=g.state_count++;g.queued_state[index]=next;
            g.state_intern[slot].hash=hash;g.state_intern[slot].index_plus_one=index+1;
            ++g.state_intern_misses;
        }
    }else{++g.state_count;g.queued_state[index]=next;}
    for(unsigned i=0;i<n;i++) {
        g.queued_kind[g.queued_count+i]=(uint8_t)simple;
        raw.slots[g.queued_count+i]=0;
        if(!raw_reserve)g.queued[g.queued_count+i]=tri[i];
        else {g.queued[g.queued_count+i].use_rc=(unsigned)raw_use_rc;
              g.queued[g.queued_count+i].textured=s->tex[0].valid;}
        g.queued[g.queued_count+i].state_index=index;
        if(!raw_reserve) {
            if((unsigned)tri[i].minx<g.dirty_left)g.dirty_left=(unsigned)tri[i].minx;
            if((unsigned)tri[i].miny<g.dirty_top)g.dirty_top=(unsigned)tri[i].miny;
            if((unsigned)tri[i].maxx>g.dirty_right)g.dirty_right=(unsigned)tri[i].maxx;
            if((unsigned)tri[i].maxy>g.dirty_bottom)g.dirty_bottom=(unsigned)tri[i].maxy;
        }
    }
    g.queued_count+=n;g.queued_refs+=refs;
    for(int i=0;i<4;i++)if(g.bound[i])g.bound[i]->queued=1;
    if(!g.selftest){++g.draws;g.tris+=n;g.submit_ms+=gpu_ms()-begin;}
    return 1;
fallback:
    if(!g.selftest)++g.fallbacks;
    return 0;
}
int nv_gpu_draw(const NvGpuState *s,const NvGpuTri *tri,unsigned n,
                uint8_t *color,float *depth,unsigned stride,const NvGpuTextureSource texture[4])
{
    return gpu_draw_ordered(s,tri,n,color,depth,stride,texture,0,0);
}
int nv_gpu_reserve_raw(const NvGpuState *s,unsigned n,int use_rc,
                       uint8_t *color,float *depth,unsigned stride,const NvGpuTextureSource texture[4])
{
    return gpu_draw_ordered(s,NULL,n,color,depth,stride,texture,1,use_rc);
}
/* Cache only compiler output. The source hash includes shared types/kernel;
 * the cached program must still pass hardware creation and the offscreen check.
 * No raw pointers or guest pixels are written to this file. */
#define GPU_SHADER_FLAGS (D3DCOMPILE_IEEE_STRICTNESS|D3DCOMPILE_OPTIMIZATION_LEVEL3)
/* Keys describe compiler inputs, not the CPU overlay/build number. Includes
 * are expanded into these strings by the build dependency before compilation. */
/* Bit 3 distinguishes visibility-only native shaders; bit 4 selects grouped
 * named-register writes in the general pixel shader. Bit 5 selects shared
 * bilinear addresses; bit 6 selects the frozen batch-depth rejection bound. */
static unsigned gpu_native_variant(unsigned base)
{
    return base|(g.native_diagnostics?0u:8u)|((g.vector_combiner && !g.array_registers && !(base&2u))?16u:0u)|(g.quad_sampling?32u:0u)|(g.seed.enabled?64u:0u)|(g.empty_reject?128u:0u)|(g.material.enabled?256u:0u)|(g.skip_unchanged?512u:0u)|((g.local_rov && (base&2u))?1024u:0u)|(g.early_depth?2048u:0u);
}
static void gpu_reference_counts(NvGpuCount *reference)
{
    if(g.native && !g.native_diagnostics)
        reference->depth_fail=reference->pixels=reference->pixel_max=0;
}
static uint64_t gpu_compile_key(const char *source,size_t bytes,const char *entry,
                                const char *profile,unsigned variant)
{
    uint64_t h=14695981039346656037ull;
    const char *tag="QPR2-DXBC-v2";
    const char *parts[]={tag,entry,profile};
    for(unsigned p=0;p<3;p++)for(size_t i=0;i<=strlen(parts[p]);i++){h^=(unsigned char)parts[p][i];h*=1099511628211ull;}
    for(size_t i=0;i<bytes;i++){h^=(unsigned char)source[i];h*=1099511628211ull;}
    uint32_t settings[2]={GPU_SHADER_FLAGS,variant};
    for(unsigned p=0;p<2;p++)for(unsigned shift=0;shift<32;shift+=8){h^=(settings[p]>>shift)&255u;h*=1099511628211ull;}
    return h;
}
static int gpu_shader_load(const char *path,uint64_t key,ID3D11ComputeShader **shader)
{
    FILE *f=fopen(path,"rb");if(!f)return 0;
    uint64_t header[3];void *data=NULL;int ok=0;
    if(fread(header,sizeof header,1,f)==1 && header[0]==0x5150523244584243ull &&
       header[1]==key && header[2]>0 && header[2]<=16u*1024u*1024u) {
        data=malloc((size_t)header[2]);
        if(data && fread(data,1,(size_t)header[2],f)==(size_t)header[2] && fgetc(f)==EOF)
            ok=SUCCEEDED(ID3D11Device_CreateComputeShader(g.device,data,(size_t)header[2],NULL,shader));
    }
    free(data);fclose(f);return ok;
}
static int gpu_shader_cached(const char *path,uint64_t key)
{
    return gpu_shader_load(path,key,&g.shader);
}
static void gpu_shader_save(const char *path,uint64_t key,ID3DBlob *code)
{
    FILE *f=fopen(path,"wb");if(!f)return;
    uint64_t header[3]={0x5150523244584243ull,key,ID3D10Blob_GetBufferSize(code)};
    int ok=fwrite(header,sizeof header,1,f)==1 &&
        fwrite(ID3D10Blob_GetBufferPointer(code),1,(size_t)header[2],f)==header[2];
    if(fclose(f)!=0)ok=0;
    if(!ok)remove(path);
}
static int gpu_general_initialize(int array_registers,char *cache_path)
{
    uint64_t key=gpu_compile_key(nv_gpu_shader_source,sizeof(nv_gpu_shader_source)-1,"main","cs_5_0",array_registers?1u:0u);
    snprintf(cache_path,128,"shader-cache/qpr2-gpu-%016llx.dxbc",(unsigned long long)key);
    double begin=gpu_ms();int cached=gpu_shader_cached(cache_path,key);
    if(!cached) {
        D3D_SHADER_MACRO macros[2]={{"NV_GPU_ARRAY_REGISTERS","1"},{NULL,NULL}};
        ID3DBlob *code=NULL,*errors=NULL;
        HRESULT hr=D3DCompile(nv_gpu_shader_source,sizeof(nv_gpu_shader_source)-1,"qpr2_gpu_raster",
            array_registers?macros:NULL,NULL,"main","cs_5_0",
            GPU_SHADER_FLAGS,0,&code,&errors);
        if(FAILED(hr)) {
            fprintf(stderr,"[GPU-RASTER] general shader registers=%s compile failed: %s\n",
                array_registers?"array":"named",errors?(char*)ID3D10Blob_GetBufferPointer(errors):"no diagnostic");
            if(errors)ID3D10Blob_Release(errors);if(code)ID3D10Blob_Release(code);return 0;
        }
        if(errors)ID3D10Blob_Release(errors);
        hr=ID3D11Device_CreateComputeShader(g.device,ID3D10Blob_GetBufferPointer(code),ID3D10Blob_GetBufferSize(code),NULL,&g.shader);
        if(SUCCEEDED(hr))gpu_shader_save(cache_path,key,code);
        ID3D10Blob_Release(code);if(FAILED(hr))return 0;
    }
    g.array_registers=array_registers;
    fprintf(stderr,"[GPU-RASTER] general shader registers=%s cache_hit=%d setup_ms=%.3f\n",
        array_registers?"array":"named",cached,gpu_ms()-begin);
    return 1;
}
static int gpu_simple_initialize(char *cache_path)
{
    const char *enabled=getenv("RECOMP_GPU_SIMPLE");
    if(enabled && atoi(enabled)<=0)return 0;
    uint64_t key=gpu_compile_key(nv_gpu_shader_source,sizeof(nv_gpu_shader_source)-1,"main","cs_5_0",2u);
    snprintf(cache_path,128,"shader-cache/qpr2-gpu-simple-%016llx.dxbc",(unsigned long long)key);
    double begin=gpu_ms();int cached=gpu_shader_load(cache_path,key,&g.simple_shader);
    if(!cached) {
        D3D_SHADER_MACRO macros[2]={{"NV_GPU_SIMPLE_MATERIAL","1"},{NULL,NULL}};
        ID3DBlob *code=NULL,*errors=NULL;
        HRESULT hr=D3DCompile(nv_gpu_shader_source,sizeof(nv_gpu_shader_source)-1,"qpr2_gpu_simple",
            macros,NULL,"main","cs_5_0",GPU_SHADER_FLAGS,0,&code,&errors);
        if(FAILED(hr)) {
            fprintf(stderr,"[GPU-RASTER] simple material compile failed: %s; general compute retained\n",
                errors?(char*)ID3D10Blob_GetBufferPointer(errors):"no diagnostic");
            if(errors)ID3D10Blob_Release(errors);if(code)ID3D10Blob_Release(code);return 0;
        }
        if(errors)ID3D10Blob_Release(errors);
        hr=ID3D11Device_CreateComputeShader(g.device,ID3D10Blob_GetBufferPointer(code),ID3D10Blob_GetBufferSize(code),NULL,&g.simple_shader);
        if(SUCCEEDED(hr))gpu_shader_save(cache_path,key,code);
        ID3D10Blob_Release(code);if(FAILED(hr))return 0;
    }
    fprintf(stderr,"[GPU-RASTER] simple material shader cache_hit=%d setup_ms=%.3f\n",cached,gpu_ms()-begin);
    return 1;
}
static int gpu_report_initialize(char *cache_path)
{
    const char *enabled=getenv("RECOMP_GPU_REPORT_RESET");if(enabled && atoi(enabled)<=0)return 0;
    uint64_t key=gpu_compile_key(nv_gpu_report_reset_source,sizeof(nv_gpu_report_reset_source)-1,"main","cs_5_0",0);
    snprintf(cache_path,128,"shader-cache/qpr2-report-reset-%016llx.dxbc",(unsigned long long)key);
    if(gpu_shader_load(cache_path,key,&g.report_shader))return 1;
    ID3DBlob *code=NULL,*errors=NULL;
    HRESULT hr=D3DCompile(nv_gpu_report_reset_source,sizeof(nv_gpu_report_reset_source)-1,"qpr2_report_reset",
        NULL,NULL,"main","cs_5_0",GPU_SHADER_FLAGS,0,&code,&errors);
    if(SUCCEEDED(hr)) {
        hr=ID3D11Device_CreateComputeShader(g.device,ID3D10Blob_GetBufferPointer(code),ID3D10Blob_GetBufferSize(code),NULL,&g.report_shader);
        if(SUCCEEDED(hr))gpu_shader_save(cache_path,key,code);
    } else fprintf(stderr,"[GPU-RASTER] visibility-reset compile failed: %s; synchronous resets retained\n",
        errors?(char*)ID3D10Blob_GetBufferPointer(errors):"no diagnostic");
    if(errors)ID3D10Blob_Release(errors);if(code)ID3D10Blob_Release(code);return SUCCEEDED(hr);
}

static int gpu_clear_initialize(char *cache_path)
{
    const char *enabled=getenv("RECOMP_GPU_CLEAR");
    if(!g.native || (enabled && atoi(enabled)<=0))return 0;
    uint64_t key=gpu_compile_key(nv_gpu_clear_source,sizeof(nv_gpu_clear_source)-1,"main","cs_5_0",0);
    snprintf(cache_path,128,"shader-cache/qpr2-clear-%016llx.dxbc",(unsigned long long)key);
    if(!gpu_buffer_create(&g.clear_parameters,9*4,4,0))return 0;
    if(gpu_shader_load(cache_path,key,&g.clear_shader))return 1;
    ID3DBlob *code=NULL,*errors=NULL;
    HRESULT hr=D3DCompile(nv_gpu_clear_source,sizeof(nv_gpu_clear_source)-1,"qpr2_gpu_clear",
        NULL,NULL,"main","cs_5_0",GPU_SHADER_FLAGS,0,&code,&errors);
    if(SUCCEEDED(hr)) {
        hr=ID3D11Device_CreateComputeShader(g.device,ID3D10Blob_GetBufferPointer(code),ID3D10Blob_GetBufferSize(code),NULL,&g.clear_shader);
        if(SUCCEEDED(hr))gpu_shader_save(cache_path,key,code);
    } else fprintf(stderr,"[GPU-RASTER] surface-clear compile failed: %s; synchronous clears retained\n",
        errors?(char*)ID3D10Blob_GetBufferPointer(errors):"no diagnostic");
    if(errors)ID3D10Blob_Release(errors);if(code)ID3D10Blob_Release(code);return SUCCEEDED(hr);
}
#include "nv2a_gpu_texture_bridge.inc"
static void gpu_keep_failed_shader(const char *source,const char *stage)
{
    const char *base=getenv("RECOMP_GPU_CAPTURE");if(!base || !*base)return;
    char path[1024];int n=snprintf(path,sizeof path,"%s.failed-%s.dxbc-cache",base,stage);
    if(n<0 || n>=(int)sizeof path)return;
    FILE *input=fopen(source,"rb");if(!input)return;
    FILE *output=fopen(path,"wb");if(!output){fclose(input);return;}
    unsigned char block[4096];size_t bytes,total=0;int ok=1;
    while((bytes=fread(block,1,sizeof block,input))!=0) {
        total+=bytes;if(total>16u*1024u*1024u+32u || fwrite(block,1,bytes,output)!=bytes){ok=0;break;}
    }
    if(ferror(input))ok=0;fclose(input);if(fclose(output)!=0)ok=0;
    if(!ok)remove(path);
    else fprintf(stderr,"[GPU-CAPTURE] preserved failed %s shader: %s\n",stage,path);
}
/* vertex_mode: 0 pixel, 1 rectangle vertex, 2 conservative triangle vertex. */
static int gpu_rov_code(int vertex_mode,int simple,const char *path,uint64_t key)
{
    int vertex=vertex_mode!=0,coverage=vertex_mode==2;
    const char *source=coverage?nv_gpu_coverage_source:nv_gpu_rov_source;
    size_t source_bytes=coverage?sizeof(nv_gpu_coverage_source)-1:sizeof(nv_gpu_rov_source)-1;
    ID3D11VertexShader **vertex_out=coverage?&g.coverage_vertex_shader:&g.vertex_shader;
    FILE*f=fopen(path,"rb");uint64_t header[4];void *data=NULL;HRESULT hr=-1;
    double begin=gpu_ms();unsigned stage=g.shade_probe.compiling?8u:
        (coverage?3u:(vertex?1u:(simple?2u:(g.literal.compiling?3u+g.literal.compiling:0u))));
    ID3D11PixelShader **pixel=g.shade_probe.compiling?&g.shade_probe.shader:
        (simple?&g.simple_pixel_shader:(g.literal.compiling?&g.literal.shader[g.literal.compiling-1]:&g.pixel_shader));
    if(f) {
        if(fread(header,sizeof header,1,f)==1 && header[0]==0x51505232524F5631ull &&
           header[1]==key && header[2]==stage && header[3]>0 && header[3]<=16u*1024u*1024u) {
            data=malloc((size_t)header[3]);
            if(data && fread(data,1,(size_t)header[3],f)==header[3] && fgetc(f)==EOF) {
                if(vertex)hr=ID3D11Device_CreateVertexShader(g.device,data,(size_t)header[3],NULL,vertex_out);
                else hr=ID3D11Device_CreatePixelShader(g.device,data,(size_t)header[3],NULL,pixel);
            }
        }
        free(data);fclose(f);if(SUCCEEDED(hr)){
            fprintf(stderr,"[GPU-RASTER] native shader stage=%u cache_hit=1 setup_ms=%.3f\n",stage,gpu_ms()-begin);return 1;}
    }
    D3D_SHADER_MACRO macros[16];unsigned macro_count=0;
    if(vertex)macros[macro_count++]=(D3D_SHADER_MACRO){"NV_GPU_VERTEX","1"};
    if(simple)macros[macro_count++]=(D3D_SHADER_MACRO){"NV_GPU_SIMPLE_MATERIAL","1"};
    if(!vertex)macros[macro_count++]=(D3D_SHADER_MACRO){"NV_GPU_DIAGNOSTIC_COUNTERS",g.native_diagnostics?"1":"0"};
    if(!vertex)macros[macro_count++]=(D3D_SHADER_MACRO){"NV_GPU_VECTOR_COMBINERS",g.vector_combiner && !g.array_registers && !simple?"1":"0"};
    if(!vertex)macros[macro_count++]=(D3D_SHADER_MACRO){"NV_GPU_QUAD_SAMPLING",g.quad_sampling?"1":"0"};
    if(!vertex)macros[macro_count++]=(D3D_SHADER_MACRO){"NV_GPU_DEPTH_SEED",g.seed.enabled?"1":"0"};
    if(!vertex)macros[macro_count++]=(D3D_SHADER_MACRO){"NV_GPU_EMPTY_REJECT",g.empty_reject?"1":"0"};
    if(!vertex)macros[macro_count++]=(D3D_SHADER_MACRO){"NV_GPU_EARLY_DEPTH",g.early_depth && !g.native_diagnostics?"1":"0"};
    if(!vertex)macros[macro_count++]=(D3D_SHADER_MACRO){"NV_GPU_SKIP_UNCHANGED",g.skip_unchanged?"1":"0"};
    if(!vertex)macros[macro_count++]=(D3D_SHADER_MACRO){"NV_GPU_STAGE_LIVENESS",g.material.enabled?"1":"0"};
    static const char *literal_definitions[]={"0","1","2","3"};
    if(!vertex)macros[macro_count++]=(D3D_SHADER_MACRO){"NV_GPU_LITERAL_CLASS",literal_definitions[g.literal.compiling]};
    if(!vertex)macros[macro_count++]=(D3D_SHADER_MACRO){"NV_GPU_COMPACT_MATERIALS",g.literal.compact_compiling?"1":"0"};
    if(!vertex && g.shade_probe.compiling)macros[macro_count++]=(D3D_SHADER_MACRO){"NV_GPU_PROBE_SHADE","1"};
    if(!vertex && simple && g.local_rov)macros[macro_count++]=(D3D_SHADER_MACRO){"NV_GPU_LOCAL_ROV","1"};
    macros[macro_count++]=(D3D_SHADER_MACRO){"NV_GPU_ARRAY_REGISTERS",g.array_registers?"1":"0"};
    macros[macro_count]=(D3D_SHADER_MACRO){NULL,NULL};
    ID3DBlob *code=NULL,*errors=NULL;
    hr=D3DCompile(source,source_bytes,coverage?"qpr2_gpu_coverage":"qpr2_gpu_rov",
        macros,NULL,vertex?"vertex_main":"pixel_main",vertex?"vs_5_0":"ps_5_0",
        GPU_SHADER_FLAGS,0,&code,&errors);
    if(FAILED(hr)) {
        fprintf(stderr,"[GPU-RASTER] native %s compile failed: %s; %s\n",
            coverage?"coverage vertex":(vertex?"vertex":(simple?"simple pixel":"pixel")),errors?(char*)ID3D10Blob_GetBufferPointer(errors):"no diagnostic",coverage?"rectangle coverage retained":"compute fallback");
        if(errors)ID3D10Blob_Release(errors);if(code)ID3D10Blob_Release(code);return 0;
    }
    if(errors)ID3D10Blob_Release(errors);
    if(vertex)hr=ID3D11Device_CreateVertexShader(g.device,ID3D10Blob_GetBufferPointer(code),ID3D10Blob_GetBufferSize(code),NULL,vertex_out);
    else hr=ID3D11Device_CreatePixelShader(g.device,ID3D10Blob_GetBufferPointer(code),ID3D10Blob_GetBufferSize(code),NULL,pixel);
    if(SUCCEEDED(hr)) {
        f=fopen(path,"wb");if(f){uint64_t h[4]={0x51505232524F5631ull,key,stage,ID3D10Blob_GetBufferSize(code)};
            int ok=fwrite(h,sizeof h,1,f)==1 && fwrite(ID3D10Blob_GetBufferPointer(code),1,(size_t)h[3],f)==h[3];
            if(fclose(f)!=0)ok=0;if(!ok)remove(path);}
    }
    ID3D10Blob_Release(code);
    fprintf(stderr,"[GPU-RASTER] native shader stage=%u cache_hit=0 setup_ms=%.3f created=%u\n",stage,gpu_ms()-begin,(unsigned)SUCCEEDED(hr));
    return SUCCEEDED(hr);
}
#include "nv2a_gpu_shade_probe.inc"
static int gpu_native_general_initialize(char *pixel_path)
{
    for(unsigned attempt=0;attempt<7;attempt++){
        uint64_t key=gpu_compile_key(nv_gpu_rov_source,sizeof(nv_gpu_rov_source)-1,"pixel_main","ps_5_0",gpu_native_variant(g.array_registers?1u:0u));
        snprintf(pixel_path,128,"shader-cache/qpr2-rov-ps-%016llx.dxbc",(unsigned long long)key);
        if(gpu_rov_code(0,0,pixel_path,key))return 1;
        if(g.early_depth){g.early_depth=0;fprintf(stderr,"[GPU-EARLY-DEPTH] compile/create failed; restoring source-first shader\n");continue;}
        if(g.material.enabled){g.material.enabled=0;fprintf(stderr,"[GPU-MATERIAL-USAGE] compile/create failed; retrying full stages\n");continue;}
        if(g.empty_reject){g.empty_reject=0;fprintf(stderr,"[GPU-EMPTY-REJECT] compile/create failed; retrying V94 transactions\n");continue;}
        if(g.seed.enabled){g.seed.enabled=0;fprintf(stderr,"[GPU-DEPTH-SEED] compile/create failed; retrying without depth seed\n");continue;}
        if(g.quad_sampling){g.quad_sampling=0;fprintf(stderr,"[GPU-SAMPLING] shared-address compile/create failed; retrying legacy sampling\n");continue;}
        if(!g.vector_combiner || g.array_registers)break;
        g.vector_combiner=0;
        fprintf(stderr,"[GPU-COMBINER] vector compile/create failed; retrying scalar named pixel shader\n");
    }
    return 0;
}
static int gpu_rov_initialize(char *vertex_path,char *pixel_path)
{
    const char*enabled=getenv("RECOMP_GPU_NATIVE");
    if(enabled && atoi(enabled)<=0)return 0;
    D3D11_FEATURE_DATA_D3D11_OPTIONS2 feature;memset(&feature,0,sizeof feature);
    HRESULT hr=ID3D11Device_CheckFeatureSupport(g.device,D3D11_FEATURE_D3D11_OPTIONS2,&feature,sizeof feature);
    fprintf(stderr,"[GPU-RASTER] native ROV support=%u feature_query=%08lX\n",
        (unsigned)feature.ROVsSupported,(unsigned long)hr);
    if(FAILED(hr) || !feature.ROVsSupported)return 0;
    uint64_t vertex_key=gpu_compile_key(nv_gpu_rov_source,sizeof(nv_gpu_rov_source)-1,"vertex_main","vs_5_0",4u|(g.array_registers?1u:0u));
    snprintf(vertex_path,128,"shader-cache/qpr2-rov-vs-%016llx.dxbc",(unsigned long long)vertex_key);
    if(!gpu_rov_code(1,0,vertex_path,vertex_key) || !gpu_native_general_initialize(pixel_path))return 0;
    D3D11_RASTERIZER_DESC raster;memset(&raster,0,sizeof raster);
    raster.FillMode=D3D11_FILL_SOLID;raster.CullMode=D3D11_CULL_NONE;raster.DepthClipEnable=1;
    return SUCCEEDED(ID3D11Device_CreateRasterizerState(g.device,&raster,&g.rasterizer)) &&
           gpu_buffer_create(&g.startup_trace,8u*32u*32u*16u,16,1);
}
static int gpu_coverage_initialize(char *path)
{
    const char *enabled=getenv("RECOMP_GPU_COVERAGE");if(enabled && atoi(enabled)<=0)return 0;
    D3D11_FEATURE_DATA_D3D11_OPTIONS2 feature;memset(&feature,0,sizeof feature);
    HRESULT hr=ID3D11Device_CheckFeatureSupport(g.device,D3D11_FEATURE_D3D11_OPTIONS2,&feature,sizeof feature);
    fprintf(stderr,"[GPU-RASTER] conservative coverage tier=%u feature_query=%08lX\n",(unsigned)feature.ConservativeRasterizationTier,(unsigned long)hr);
    /* Tier 1 may discard triangles that collapse on the subpixel grid. */
    if(FAILED(hr) || feature.ConservativeRasterizationTier<D3D11_CONSERVATIVE_RASTERIZATION_TIER_2)return 0;
    ID3D11Device3 *modern=NULL;hr=ID3D11Device_QueryInterface(g.device,&IID_ID3D11Device3,(void**)&modern);if(FAILED(hr))return 0;
    D3D11_RASTERIZER_DESC2 raster;memset(&raster,0,sizeof raster);
    raster.FillMode=D3D11_FILL_SOLID;raster.CullMode=D3D11_CULL_NONE;raster.DepthClipEnable=1;
    raster.ConservativeRaster=D3D11_CONSERVATIVE_RASTERIZATION_MODE_ON;raster.ScissorEnable=1;
    hr=ID3D11Device3_CreateRasterizerState2(modern,&raster,&g.coverage_rasterizer);ID3D11Device3_Release(modern);if(FAILED(hr))return 0;
    uint64_t key=gpu_compile_key(nv_gpu_coverage_source,sizeof(nv_gpu_coverage_source)-1,"vertex_main","vs_5_0",4u|(g.array_registers?1u:0u));
    snprintf(path,128,"shader-cache/qpr2-coverage-vs-%016llx.dxbc",(unsigned long long)key);
    if(gpu_rov_code(2,0,path,key))return 1;
    ID3D11RasterizerState2_Release(g.coverage_rasterizer);g.coverage_rasterizer=NULL;return 0;
}
static int gpu_vertex_ids_initialize(void)
{
    unsigned count=NV_GPU_MAX_TRIANGLES*6u;uint32_t *ids=malloc((size_t)count*4);if(!ids)return 0;
    for(unsigned i=0;i<count;i++)ids[i]=i;
    D3D11_BUFFER_DESC desc;memset(&desc,0,sizeof desc);desc.ByteWidth=count*4;
    desc.Usage=D3D11_USAGE_IMMUTABLE;desc.BindFlags=D3D11_BIND_INDEX_BUFFER;
    D3D11_SUBRESOURCE_DATA data;memset(&data,0,sizeof data);data.pSysMem=ids;
    HRESULT hr=ID3D11Device_CreateBuffer(g.device,&desc,&data,&g.vertex_ids);free(ids);return SUCCEEDED(hr);
}
#include "nv2a_gpu_literal_host.inc"
#include "nv2a_gpu_store_host.inc"
static int gpu_initialize(void)
{
    const char *hot=getenv("RECOMP_GPU_TEXTURE_HOT");g.texture_hot_enabled=!hot || atoi(hot)>0;
    const char *reports=getenv("RECOMP_GPU_REPORT_ONLY");g.report_only_enabled=!reports || atoi(reports)>0;
    const char *micro=getenv("RECOMP_GPU_MICRO_FUSE");g.micro_fuse.enabled=!micro || atoi(micro)>0;
    if(g.init)return g.init>=2;
    g.init=1;
    const char *enabled=getenv("RECOMP_GPU_RASTER");
    if(!enabled || atoi(enabled)<=0)return 0;
    const char *defer=getenv("RECOMP_GPU_DEFER_READBACK");
    g.deferred_readback.enabled=!defer || atoi(defer)>0;
    const char *simd=getenv("RECOMP_GPU_READBACK_SIMD");
    g.readback_simd=NV_GPU_SURFACE_SSE2 && (!simd || atoi(simd)>0);
    const char *batch_readback=getenv("RECOMP_GPU_BATCH_READBACK");
    g.readback_batch.enabled=!batch_readback || atoi(batch_readback)>0;
    const char *vector=getenv("RECOMP_GPU_VECTOR_COMBINERS");g.vector_combiner=!vector || atoi(vector)>0;
    const char *quad=getenv("RECOMP_GPU_QUAD_SAMPLING");g.quad_sampling=!quad || atoi(quad)>0;
    const char *diagnostics=getenv("RECOMP_GPU_DIAGNOSTIC_COUNTERS");
    g.native_diagnostics=diagnostics && atoi(diagnostics)>0;
    {const char *e=getenv("RECOMP_GPU_COMPLETION_TOKEN");g.completion.enabled=e && atoi(e)>0;}
    {const char *e=getenv("RECOMP_GPU_FEEDBACK_SNAPSHOT");g.completion.feedback_enabled=!e || atoi(e)>0;}
    const char *local_rov=getenv("RECOMP_GPU_LOCAL_ROV");
    g.local_rov=local_rov && atoi(local_rov)>0;
    const char *reject=getenv("RECOMP_GPU_EMPTY_REJECT");g.empty_reject=!reject || atoi(reject)>0;
    /* ROVs are unavailable on an 11_0 device even when the adapter supports
     * them. Request modern levels first, with legacy-runtime fallbacks. */
    D3D_FEATURE_LEVEL requested[]={D3D_FEATURE_LEVEL_12_1,D3D_FEATURE_LEVEL_12_0,
        D3D_FEATURE_LEVEL_11_1,D3D_FEATURE_LEVEL_11_0},actual;
    HRESULT hr=D3D11CreateDevice(NULL,D3D_DRIVER_TYPE_HARDWARE,NULL,0,requested,4,
        D3D11_SDK_VERSION,&g.device,&actual,&g.context);
    if(hr==E_INVALIDARG)hr=D3D11CreateDevice(NULL,D3D_DRIVER_TYPE_HARDWARE,NULL,0,requested+2,2,
        D3D11_SDK_VERSION,&g.device,&actual,&g.context);
    if(hr==E_INVALIDARG)hr=D3D11CreateDevice(NULL,D3D_DRIVER_TYPE_HARDWARE,NULL,0,requested+3,1,
        D3D11_SDK_VERSION,&g.device,&actual,&g.context);
    if(FAILED(hr)) {
        fprintf(stderr,"[GPU-RASTER] no D3D11 hardware device hr=%08lX; software fallback\n",(unsigned long)hr);return 0;
    }
    IDXGIDevice *dxgi=NULL;IDXGIAdapter *adapter=NULL;DXGI_ADAPTER_DESC desc;
    if(SUCCEEDED(ID3D11Device_QueryInterface(g.device,&IID_IDXGIDevice,(void**)&dxgi))) {
        if(SUCCEEDED(IDXGIDevice_GetAdapter(dxgi,&adapter))) {
            if(SUCCEEDED(IDXGIAdapter_GetDesc(adapter,&desc))) {
                char name[256]={0};WideCharToMultiByte(CP_UTF8,0,desc.Description,-1,name,sizeof name,NULL,NULL);
                fprintf(stderr,"[GPU-RASTER] hardware adapter=%s feature_level=%04X dedicated_mib=%llu\n",
                    name,(unsigned)actual,(unsigned long long)(desc.DedicatedVideoMemory/(1024*1024)));
            }
            IDXGIAdapter_Release(adapter);
        }
        IDXGIDevice_Release(dxgi);
    }
    CreateDirectoryA("shader-cache",NULL); /* Cache I/O is optional; compile on a miss. */
    char cache_path[128];const char *fixed=getenv("RECOMP_GPU_FIXED");
    int array_registers=fixed && atoi(fixed)<=0;
    if(!gpu_general_initialize(array_registers,cache_path) &&
       (array_registers || !gpu_general_initialize(1,cache_path)))goto fail;
    g.color_upload=malloc(1024u*1024u*4u);g.depth_upload=malloc(1024u*1024u*4u);
    g.surface_upload=malloc(1024u*1024u*sizeof(GpuSurface));
    g.refs=malloc(NV_GPU_MAX_REFERENCES*4u);
    g.queued=malloc(NV_GPU_MAX_TRIANGLES*sizeof(NvGpuTri));
    if(!g.color_upload || !g.depth_upload || !g.surface_upload || !g.refs || !g.queued || !gpu_buffer_create(&g.stats,NV_GPU_MAX_TILES*16,4,1) ||
       !gpu_buffer_create(&g.atlas,128u*1024u*1024u,4,0))goto fail;
    /* The real hardware shader must pass an offscreen comparison before any
     * title pixels are submitted. This is ordinary fixture data, not altered
     * guest memory or runtime poisoning. */
    g.init=2;g.selftest=1;
    if(!gpu_selftest()) {
        gpu_keep_failed_shader(cache_path,g.array_registers?"compute-array":"compute-named");
        remove(cache_path);
        if(g.array_registers)goto fail;
        fprintf(stderr,"[GPU-RASTER] named-register offscreen comparison failed; retrying array compute\n");
        ID3D11ComputeShader_Release(g.shader);g.shader=NULL;
        g.active=g.resident=0;g.queued_count=g.state_count=g.queued_refs=0;
        for(unsigned i=0;i<128;i++)g.cache[i].queued=0;
        memset(&g.completed,0,sizeof g.completed);
        if(!gpu_general_initialize(1,cache_path) || !gpu_selftest())goto fail;
    }
    char simple_path[128]={0};
    if(gpu_simple_initialize(simple_path)) {
        int simple_ok=gpu_selftest();
        if(simple_ok){simple_ok=gpu_material_selftest();if(simple_ok<0)goto fail;}
        if(!simple_ok) {
            fprintf(stderr,"[GPU-RASTER] simple material offscreen comparison failed; general compute retained\n");
            remove(simple_path);ID3D11ComputeShader_Release(g.simple_shader);g.simple_shader=NULL;
            g.active=0;g.queued_count=g.state_count=g.queued_refs=0;
            for(unsigned i=0;i<128;i++)g.cache[i].queued=0;
            memset(&g.completed,0,sizeof g.completed);
            if(!gpu_selftest())goto fail;
        } else fprintf(stderr,"[GPU-RASTER] simple material offscreen comparison passed; specialized compute enabled\n");
    }
    const char *seed=getenv("RECOMP_GPU_DEPTH_SEED");g.seed.enabled=!seed || atoi(seed)>0;
    const char *seed_reuse=getenv("RECOMP_GPU_SEED_REUSE");g.seed.reuse_enabled=!seed_reuse || atoi(seed_reuse)>0;
    /* V114's live-depth-first shader increased race map waits on the RX 6900 XT.
     * Keep it opt-in until a controlled pixel-only A/B demonstrates a gain. */
    const char *early=getenv("RECOMP_GPU_EARLY_DEPTH");g.early_depth=!g.native_diagnostics && early && atoi(early)>0;
    const char *prefix=getenv("RECOMP_GPU_DEPTH_PREFIX");g.seed.prefix_enabled=!prefix || atoi(prefix)>0;
    if(g.seed.enabled){
        if(!gpu_buffer_create(&g.seed.control,16,4,0)){g.seed.enabled=0;fprintf(stderr,"[GPU-DEPTH-SEED] control allocation failed; legacy shading retained\n");}
        else {const uint32_t zero[4]={0};ID3D11DeviceContext_UpdateSubresource(g.context,(ID3D11Resource*)g.seed.control.buffer,0,NULL,zero,0,0);}
    }
    const char *usage=getenv("RECOMP_GPU_STAGE_LIVENESS");g.material.enabled=!usage || atoi(usage)>0;
    if(g.material.enabled){
        if(!gpu_buffer_create(&g.material.fallback,NV_GPU_MAX_STATES*4,4,0)){
            g.material.enabled=0;fprintf(stderr,"[GPU-MATERIAL-USAGE] fallback allocation failed; full stages retained\n");
        }else {
            for(unsigned i=0;i<NV_GPU_MAX_STATES;i++)g.material.masks[i]=15;
            ID3D11DeviceContext_UpdateSubresource(g.context,(ID3D11Resource*)g.material.fallback.buffer,0,NULL,g.material.masks,0,0);
        }
    }
    char vertex_path[128]={0},pixel_path[128]={0};
    if(gpu_rov_initialize(vertex_path,pixel_path)) {
        g.native=1;g.resident=0;g.native_batch=0;
        int native_ok=gpu_selftest();
        if(native_ok && (g.early_depth || g.material.enabled || g.empty_reject || g.seed.enabled || g.quad_sampling || (g.vector_combiner && !g.array_registers))){native_ok=gpu_material_selftest();if(native_ok<0)goto fail;}
        if(g.readback_failed)goto fail;
        while(!native_ok && (g.early_depth || g.material.enabled || g.empty_reject || g.seed.enabled || g.quad_sampling || (g.vector_combiner && !g.array_registers))){
            gpu_keep_failed_shader(pixel_path,g.early_depth?"native-early-depth-ps":g.material.enabled?"native-stage-usage-ps":g.empty_reject?"native-empty-reject-ps":g.seed.enabled?"native-depth-seed-ps":g.quad_sampling?"native-sampling-ps":"native-vector-ps");remove(pixel_path);
            ID3D11PixelShader_Release(g.pixel_shader);g.pixel_shader=NULL;
            if(g.early_depth){g.early_depth=0;fprintf(stderr,"[GPU-EARLY-DEPTH] offscreen mismatch; restoring source-first shader\n");}
            else if(g.material.enabled){g.material.enabled=0;fprintf(stderr,"[GPU-MATERIAL-USAGE] offscreen mismatch; retrying full stages\n");}
            else if(g.empty_reject){g.empty_reject=0;fprintf(stderr,"[GPU-EMPTY-REJECT] offscreen mismatch; retrying V94 transactions\n");}
            else if(g.seed.enabled){g.seed.enabled=0;fprintf(stderr,"[GPU-DEPTH-SEED] offscreen mismatch; retrying without depth seed\n");}
            else if(g.quad_sampling){g.quad_sampling=0;fprintf(stderr,"[GPU-SAMPLING] shared-address offscreen mismatch; retrying legacy sampling\n");}
            else {g.vector_combiner=0;fprintf(stderr,"[GPU-COMBINER] vector offscreen mismatch; retrying scalar named pixel shader\n");}
            g.active=g.resident=0;g.queued_count=g.state_count=g.queued_refs=0;
            for(unsigned i=0;i<128;i++)g.cache[i].queued=0;
            memset(&g.completed,0,sizeof g.completed);
            native_ok=gpu_native_general_initialize(pixel_path) && gpu_selftest();
            if(native_ok){native_ok=gpu_material_selftest();if(native_ok<0)goto fail;}
            if(g.readback_failed)goto fail;
        }
        char native_simple_path[128]={0};
        const char *native_simple=getenv("RECOMP_GPU_NATIVE_SIMPLE");
        if(native_ok && (!native_simple || atoi(native_simple)>0)) {
            uint64_t key=gpu_compile_key(nv_gpu_rov_source,sizeof(nv_gpu_rov_source)-1,"pixel_main","ps_5_0",gpu_native_variant(2u|(g.array_registers?1u:0u)));
            snprintf(native_simple_path,sizeof native_simple_path,"shader-cache/qpr2-rov-simple-%016llx.dxbc",(unsigned long long)key);
            native_ok=gpu_rov_code(0,1,native_simple_path,key) && gpu_selftest();
            if(native_ok){native_ok=gpu_material_selftest();if(native_ok<0)goto fail;}
            if(g.readback_failed)goto fail;
            if(!native_ok && g.local_rov){
                /* A scope candidate must never disable the validated simple
                 * shader or force compute. Retry the v110 global variant
                 * before weakening any existing material/rejection policy. */
                gpu_keep_failed_shader(native_simple_path,"native-local-rov-simple");remove(native_simple_path);
                if(g.simple_pixel_shader){ID3D11PixelShader_Release(g.simple_pixel_shader);g.simple_pixel_shader=NULL;}
                g.local_rov=0;g.local_rov_rejected=1;
                g.active=g.resident=0;g.queued_count=g.state_count=g.queued_refs=0;
                for(unsigned i=0;i<128;i++)g.cache[i].queued=0;
                memset(&g.completed,0,sizeof g.completed);
                key=gpu_compile_key(nv_gpu_rov_source,sizeof(nv_gpu_rov_source)-1,"pixel_main","ps_5_0",gpu_native_variant(2u|(g.array_registers?1u:0u)));
                snprintf(native_simple_path,sizeof native_simple_path,"shader-cache/qpr2-rov-simple-%016llx.dxbc",(unsigned long long)key);
                native_ok=gpu_rov_code(0,1,native_simple_path,key) && gpu_selftest();
                if(native_ok){native_ok=gpu_material_selftest();if(native_ok<0)goto fail;}
                if(g.readback_failed)goto fail;
                fprintf(stderr,"[GPU-ROV-SCOPE] local candidate failed validation; retried global shader result=%u\n",(unsigned)native_ok);
            }
            if(!native_ok && g.early_depth){
                /* The simple shader must agree too. If a driver rejects the
                 * live-depth variant, restore BOTH shaders as one pair. */
                gpu_keep_failed_shader(native_simple_path,"native-early-depth-simple");remove(native_simple_path);
                if(g.simple_pixel_shader){ID3D11PixelShader_Release(g.simple_pixel_shader);g.simple_pixel_shader=NULL;}
                ID3D11PixelShader_Release(g.pixel_shader);g.pixel_shader=NULL;
                g.early_depth=0;
                g.active=g.resident=0;g.queued_count=g.state_count=g.queued_refs=0;
                for(unsigned i=0;i<128;i++)g.cache[i].queued=0;
                memset(&g.completed,0,sizeof g.completed);
                native_ok=gpu_native_general_initialize(pixel_path) && gpu_selftest();
                if(native_ok){native_ok=gpu_material_selftest();if(native_ok<0)goto fail;}
                if(g.readback_failed)goto fail;
                if(native_ok){
                    key=gpu_compile_key(nv_gpu_rov_source,sizeof(nv_gpu_rov_source)-1,"pixel_main","ps_5_0",gpu_native_variant(2u|(g.array_registers?1u:0u)));
                    snprintf(native_simple_path,sizeof native_simple_path,"shader-cache/qpr2-rov-simple-%016llx.dxbc",(unsigned long long)key);
                    native_ok=gpu_rov_code(0,1,native_simple_path,key) && gpu_selftest();
                    if(native_ok){native_ok=gpu_material_selftest();if(native_ok<0)goto fail;}
                    if(g.readback_failed)goto fail;
                }
                fprintf(stderr,"[GPU-EARLY-DEPTH] simple candidate failed; restored source-first pair result=%u\n",(unsigned)native_ok);
            }
            while(!native_ok && (g.material.enabled || g.empty_reject)){
                gpu_keep_failed_shader(native_simple_path,g.material.enabled?"native-stage-usage-simple-ps":"native-empty-reject-simple-ps");remove(native_simple_path);
                if(g.simple_pixel_shader){ID3D11PixelShader_Release(g.simple_pixel_shader);g.simple_pixel_shader=NULL;}
                ID3D11PixelShader_Release(g.pixel_shader);g.pixel_shader=NULL;
                if(g.material.enabled){g.material.enabled=0;fprintf(stderr,"[GPU-MATERIAL-USAGE] simple candidate failed; retrying both full-stage variants\n");}
                else {g.empty_reject=0;fprintf(stderr,"[GPU-EMPTY-REJECT] simple candidate failed; retrying both V94 variants\n");}
                g.active=g.resident=0;g.queued_count=g.state_count=g.queued_refs=0;
                for(unsigned i=0;i<128;i++)g.cache[i].queued=0;
                memset(&g.completed,0,sizeof g.completed);
                native_ok=gpu_native_general_initialize(pixel_path) && gpu_selftest();
                if(native_ok){native_ok=gpu_material_selftest();if(native_ok<0)goto fail;}
                if(g.readback_failed)goto fail;
                if(native_ok){
                    key=gpu_compile_key(nv_gpu_rov_source,sizeof(nv_gpu_rov_source)-1,"pixel_main","ps_5_0",gpu_native_variant(2u|(g.array_registers?1u:0u)));
                    snprintf(native_simple_path,sizeof native_simple_path,"shader-cache/qpr2-rov-simple-%016llx.dxbc",(unsigned long long)key);
                    native_ok=gpu_rov_code(0,1,native_simple_path,key) && gpu_selftest();
                    if(native_ok){native_ok=gpu_material_selftest();if(native_ok<0)goto fail;}
                    if(g.readback_failed)goto fail;
                }
            }
            if(native_ok)fprintf(stderr,"[GPU-RASTER] native simple material comparison passed; specialized graphics pixel shader enabled\n");
            else {
                fprintf(stderr,"[GPU-RASTER] native simple material setup/comparison failed; validated compute retained\n");
                gpu_keep_failed_shader(native_simple_path,"native-simple-ps");remove(native_simple_path);
                if(g.simple_pixel_shader){ID3D11PixelShader_Release(g.simple_pixel_shader);g.simple_pixel_shader=NULL;}
            }
        }
        if(!native_ok) {
            fprintf(stderr,"[GPU-RASTER] native offscreen comparison failed; compute fallback\n");
            /* Keep the unchanged general shaders cached if only the optional
             * specialized variant failed; they already passed their gate. */
            if(!native_simple_path[0]) {
                gpu_keep_failed_shader(vertex_path,"native-vs");gpu_keep_failed_shader(pixel_path,"native-ps");
                remove(vertex_path);remove(pixel_path);
            }
            g.native=0;g.active=0;g.resident=0;
            g.queued_count=g.state_count=g.queued_refs=0;
            for(unsigned i=0;i<128;i++)g.cache[i].queued=0;
            memset(&g.completed,0,sizeof g.completed);
            if(!gpu_selftest() || gpu_material_selftest()!=1)goto fail;
        } else fprintf(stderr,"[GPU-RASTER] native ROV offscreen comparison passed; graphics pixel backend enabled\n");
    }
    if(g.native){
        const char *batch=getenv("RECOMP_GPU_BATCH_UPLOADS");
        if(!batch || atoi(batch)>0){g.native_batch=gpu_vertex_ids_initialize();if(!g.native_batch)fprintf(stderr,"[GPU-RASTER] identity-index setup failed; per-class uploads retained\n");}
        int mixed_ok=gpu_mixed_selftest();if(mixed_ok<0)goto fail;
        if(!mixed_ok && g.native_batch){
            fprintf(stderr,"[GPU-RASTER] mixed material comparison failed; per-class uploads retained\n");
            g.native_batch=0;g.active=g.resident=0;g.queued_count=g.state_count=g.queued_refs=0;
            for(unsigned i=0;i<128;i++)g.cache[i].queued=0;
            memset(&g.completed,0,sizeof g.completed);
            if(!gpu_selftest() || gpu_mixed_selftest()!=1 || gpu_material_selftest()!=1)goto fail;
        }else if(!mixed_ok)goto fail;
        else if(g.native_batch)fprintf(stderr,"[GPU-RASTER] mixed material comparison passed; shared native uploads enabled\n");
    }
    char coverage_path[128]={0};
    if(g.native && gpu_coverage_initialize(coverage_path)){
        g.coverage=1;g.resident=0;int coverage_ok=gpu_selftest();
        if(coverage_ok){coverage_ok=gpu_mixed_selftest();if(coverage_ok<0)goto fail;}
        if(coverage_ok){coverage_ok=gpu_material_selftest();if(coverage_ok<0)goto fail;}
        if(!coverage_ok){
            fprintf(stderr,"[GPU-RASTER] conservative coverage comparison failed; rectangle coverage retained\n");
            gpu_keep_failed_shader(coverage_path,"coverage-vs");remove(coverage_path);
            g.coverage=0;g.active=g.resident=0;g.queued_count=g.state_count=g.queued_refs=0;
            for(unsigned i=0;i<128;i++)g.cache[i].queued=0;
            memset(&g.completed,0,sizeof g.completed);
            if(!gpu_selftest() || gpu_material_selftest()!=1)goto fail;
        }else fprintf(stderr,"[GPU-RASTER] conservative triangle comparison passed; hardware triangle coverage enabled\n");
    }
    char report_path[128]={0};
    if(gpu_report_initialize(report_path)) {
        int report_test=gpu_report_selftest();
        if(report_test<0){fprintf(stderr,"[GPU-RASTER] visibility-reset fixture execution failed\n");goto fail;}
        if(!report_test) {
            fprintf(stderr,"[GPU-RASTER] visibility-reset comparison failed; synchronous resets retained\n");
            gpu_keep_failed_shader(report_path,"report-reset");
            remove(report_path);ID3D11ComputeShader_Release(g.report_shader);g.report_shader=NULL;
            if(!gpu_selftest())goto fail;
        } else fprintf(stderr,"[GPU-RASTER] visibility-reset comparison passed; GPU-only counter resets enabled\n");
    }
    char clear_path[128]={0};
    if(gpu_clear_initialize(clear_path)) {
        int clear_test=gpu_clear_selftest();
        if(clear_test<0){fprintf(stderr,"[GPU-RASTER] surface-clear fixture execution failed\n");goto fail;}
        if(!clear_test) {
            fprintf(stderr,"[GPU-RASTER] surface-clear comparison failed; synchronous clears retained\n");
            gpu_keep_failed_shader(clear_path,"surface-clear");remove(clear_path);
            ID3D11ComputeShader_Release(g.clear_shader);g.clear_shader=NULL;
            if(!gpu_selftest())goto fail;
        }else fprintf(stderr,"[GPU-RASTER] surface-clear comparison passed; ordered GPU clears enabled\n");
    }
    const char *uav_only=getenv("RECOMP_GPU_UAV_ONLY");
    if(g.native && (!uav_only || atoi(uav_only)>0)) {
        g.uav_only=1;g.resident=0;int ok=gpu_selftest();
        if(g.readback_failed)goto fail;
        if(ok){ok=gpu_mixed_selftest();if(ok<0)goto fail;}
        if(ok){ok=gpu_material_selftest();if(ok<0)goto fail;}
        if(ok && g.clear_shader){ok=gpu_clear_selftest();if(ok<0)goto fail;}
        if(!ok) {
            fprintf(stderr,"[GPU-RASTER] UAV-only comparison failed; scratch render target retained\n");
            g.uav_only=0;g.active=g.resident=0;g.queued_count=g.state_count=g.queued_refs=0;
            for(unsigned i=0;i<128;i++)g.cache[i].queued=0;
            memset(&g.completed,0,sizeof g.completed);
            if(!gpu_selftest() || gpu_mixed_selftest()!=1 || gpu_material_selftest()!=1)goto fail;
            if(g.clear_shader && gpu_clear_selftest()!=1)goto fail;
        }else {
            if(g.raster_view){ID3D11RenderTargetView_Release(g.raster_view);g.raster_view=NULL;}
            if(g.raster_target){ID3D11Texture2D_Release(g.raster_target);g.raster_target=NULL;}
            fprintf(stderr,"[GPU-RASTER] UAV-only comparison passed; scratch render target removed\n");
        }
    }
    const char *readback_queue=getenv("RECOMP_GPU_READBACK_QUEUE");
    if(g.native && (!readback_queue || atoi(readback_queue)>0)) {
        g.readback_queue=g.readback_queue_test=1;
        int ok=gpu_queued_readback_selftest();g.readback_queue_test=0;
        if(ok<0 || g.readback_failed)goto fail;
        if(!ok){g.readback_queue=0;fprintf(stderr,"[GPU-RASTER] queued target comparison failed; synchronous target switches retained\n");}
        else fprintf(stderr,"[GPU-RASTER] queued target comparison passed; independent target copies can overlap CPU work\n");
    }
    if(g.readback_failed)goto fail;
    {
        int geometry=gpu_raw_initialize();
        if(geometry<0 || g.readback_failed)goto fail;
    }
    if(gpu_bridge_initialize()) {
        g.bridge.testing=g.readback_queue_test=1;
        int ok=gpu_bridge_selftest();g.bridge.testing=g.readback_queue_test=0;
        if(ok<0 || g.readback_failed)goto fail;
        if(!ok) {
            g.bridge.enabled=0;
            fprintf(stderr,"[GPU-TEXTURE-BRIDGE] offscreen mismatch; CPU dependencies retained\n");
        } else fprintf(stderr,"[GPU-TEXTURE-BRIDGE] exact producer/consumer comparison passed\n");
    }
    {
        const char *resume=getenv("RECOMP_GPU_TARGET_RESUME");
        if(g.bridge.enabled && (!resume || atoi(resume)>0)) {
            const char *retire=getenv("RECOMP_GPU_RETIRE_RESUMED_PIXELS");
            g.resume.retire_pixels=!retire || atoi(retire)>0;
            g.resume.enabled=g.resume.testing=g.bridge.testing=g.readback_queue_test=1;
            int ok=gpu_resume_selftest();
            g.resume.testing=g.bridge.testing=g.readback_queue_test=0;
            if(ok<0 || g.readback_failed)goto fail;
            if(!ok){g.resume.enabled=0;fprintf(stderr,"[GPU-TARGET-RESUME] offscreen mismatch; CPU target dependencies retained\n");}
            else fprintf(stderr,"[GPU-TARGET-RESUME] exact color/depth resume comparison passed\n");
            g.resume.restores=g.resume.checks=g.resume.changed=g.resume.conflicts=g.resume.allocation_failures=g.resume.bytes=0;
        }
    }
    if(!gpu_literal_initialize() || g.readback_failed)goto fail;
    if(!gpu_store_initialize() || g.readback_failed)goto fail;
    if(g.seed.control.buffer && g.seed.control_value){
        const uint32_t zero[4]={0};ID3D11DeviceContext_UpdateSubresource(g.context,(ID3D11Resource*)g.seed.control.buffer,0,NULL,zero,0,0);g.seed.control_value=0;
    }
    g.selftest=0;g.init=3;
    const char *auto_backend=getenv("RECOMP_GPU_AUTOTUNE");
    const char *experimental=getenv("RECOMP_GPU_AUTOTUNE_EXPERIMENT");
    g.auto_enabled=g.native && experimental && atoi(experimental)>0 && (!auto_backend || atoi(auto_backend)>0);
    const char *empty_stats=getenv("RECOMP_GPU_EMPTY_STATS");
    g.skip_empty_stats=!empty_stats || atoi(empty_stats)>0;
    const char *bulk_texture=getenv("RECOMP_GPU_BULK_TEXTURE");
    g.bulk_texture=!bulk_texture || atoi(bulk_texture)>0;
    const char *stream=getenv("RECOMP_GPU_UPLOAD_STREAM"),*cache=getenv("RECOMP_GPU_UPLOAD_CACHE");
    g.upload_stream=!stream || atoi(stream)>0;g.upload_cache=!cache || atoi(cache)>0;
    const char *intern=getenv("RECOMP_GPU_STATE_INTERN");
    g.state_intern_enabled=!intern || atoi(intern)>0;
    memset(g.state_intern,0,sizeof g.state_intern);
    fprintf(stderr,"[GPU-BACKEND-POLICY] default=native experimental_autotune=%u reason=single-batch-not-representative\n",(unsigned)g.auto_enabled);
    fprintf(stderr,"[GPU-POLICY] shader_order=material-first framebuffer_copy=full-resource\n");
    {
        const char *probe=getenv("RECOMP_GPU_PROFILE_SHADE");
        g.shade_probe.enabled=g.native && probe && atoi(probe)>0;
    }
    gpu_timing_initialize();
    gpu_shade_probe_initialize();
    fprintf(stderr,"[GPU-ROV-SCOPE] simple=%s rejected=%u writer_barrier=1\n",g.native && g.local_rov && g.simple_pixel_shader?"local":"global",(unsigned)g.local_rov_rejected);
    fprintf(stderr,"[GPU-RASTER] offscreen hardware comparison passed; hardware raster enabled\n");
    return 1;
fail:
    g.init=1;g.selftest=0;g.active=g.resident=0;g.pending_count=0;g.readback_queue=0;
    g.host_color=NULL;g.host_depth=NULL;
    g.queued_count=g.state_count=g.queued_refs=0;
    for(unsigned i=0;i<128;i++)g.cache[i].queued=0;
    memset(&g.completed,0,sizeof g.completed);
    fprintf(stderr,"[GPU-RASTER] hardware setup/self-test failed; software fallback\n");return 0;
}

static uint32_t gpu_load_texel(NvGpuTexture meta,int st,int face,int x,int y)
{
    (void)st;GpuTexture *t=&g.cache[meta.cache_slot];
    if(!t->decoded)return 0xFFFFFFFFu;
    return t->decoded[((size_t)face*t->meta.height+y)*t->meta.width+x];
}
#include "nv2a_gpu_pixel.inc"
#include "nv2a_gpu_shade.inc"
static int selftest_decode(void *opaque,uint32_t face,uint32_t x,uint32_t y,uint32_t *v)
{
    *v=((uint32_t*)opaque)[y*8+x]^(face*0x00331107u);return 1;
}
/* Preserve a failed private startup case for exact replay on the next upload.
 * This uses tiny generated fixture data, not guest memory or game textures. */
static void gpu_selftest_capture(unsigned pass,const NvGpuState *first,const NvGpuState *last,
                                 const NvGpuTri *tri,const uint32_t *initial_color,
                                 const float *initial_depth)
{
    const char *base=getenv("RECOMP_GPU_CAPTURE");if(!base || !*base)return;
    char path[1024];int n=snprintf(path,sizeof path,"%s.selftest-%s-%u.bin",base,
        g.native?(g.coverage?(g.queue_simple?"coverage-simple":"coverage"):(g.queue_simple?"rov-simple":"rov")):(g.queue_simple?"simple":(g.array_registers?"compute-array":"compute-named")),pass);
    if(n<0 || n>=(int)sizeof path)return;
    gpu_capture_close("startup failure case");s_capture=fopen(path,"wb");if(!s_capture)return;
    uint32_t header[4]={0x51475235u,1,sizeof(NvGpuState),sizeof(NvGpuTri)};
    if(fwrite(header,sizeof header,1,s_capture)!=1){gpu_capture_close("write failure");return;}
    s_capture_bytes=sizeof header;s_capture_presents=0;
    size_t bytes=(size_t)first->width*first->height*4;
    if(gpu_capture_header(1,sizeof *first+bytes*2)) {
        gpu_capture_data(first,sizeof *first);gpu_capture_data(initial_color,bytes);gpu_capture_data(initial_depth,bytes);
    }
    for(unsigned st=0;st<4;st++)if(first->tex[st].valid) {
        unsigned slot=first->tex[st].cache_slot;GpuTexture *t=&g.cache[slot];
        uint32_t h[3]={slot,t->atlas_base,(uint32_t)(t->decoded_bytes/4)};
        if(gpu_capture_header(2,12+t->decoded_bytes)){gpu_capture_data(h,12);gpu_capture_data(t->decoded,t->decoded_bytes);}
    }
    NvGpuState states[2]={*first,*last};NvGpuTri triangles[8];
    for(unsigned i=0;i<8;i++){triangles[i]=tri[i];triangles[i].state_index=i<4?0:1;}
    uint32_t batch[3]={g.native?((g.coverage?4u:0u)|(pass==21 && g.native_batch?8u:0u)|(g.queue_simple?3u:1u)):(g.queue_simple?2u:0u),2,8};
    if(gpu_capture_header(3,sizeof batch+sizeof states+sizeof triangles)) {
        gpu_capture_data(batch,sizeof batch);gpu_capture_data(states,sizeof states);gpu_capture_data(triangles,sizeof triangles);
    }
    uint32_t shape[2]={first->width,first->height};
    if(gpu_capture_header(4,sizeof shape+bytes*2)) {
        gpu_capture_data(shape,sizeof shape);gpu_capture_data(g.color_upload,bytes);gpu_capture_data(g.depth_upload,bytes);
    }
    if(g.native && first->width==32 && first->height==32) {
        D3D11_MAPPED_SUBRESOURCE trace;memset(&trace,0,sizeof trace);
        ID3D11DeviceContext_CopyResource(g.context,(ID3D11Resource*)g.startup_trace.readback,(ID3D11Resource*)g.startup_trace.buffer);
        if(SUCCEEDED(ID3D11DeviceContext_Map(g.context,(ID3D11Resource*)g.startup_trace.readback,0,D3D11_MAP_READ,0,&trace))) {
            uint32_t trace_shape[3]={32,32,8};
            if(gpu_capture_header(6,sizeof trace_shape+g.startup_trace.bytes)) {
                gpu_capture_data(trace_shape,sizeof trace_shape);gpu_capture_data(trace.pData,g.startup_trace.bytes);
            }
            ID3D11DeviceContext_Unmap(g.context,(ID3D11Resource*)g.startup_trace.readback,0);
        }
    }
    fprintf(stderr,"[GPU-CAPTURE] saved failed startup comparison: %s\n",path);
    gpu_capture_close("startup comparison saved");
}
static int gpu_selftest(void)
{
    uint32_t original[32*32],actual[32*32],expected[32*32],texels[64];
    uint32_t initial_color[32*32];float initial_depth[32*32];
    float depth[32*32],reference_depth[32*32];NvGpuTri tri[8];
    NvGpuState state;NvGpuTextureSource sources[4];
    for(unsigned k=0;k<64;k++)texels[k]=0x20000000u+(k*0x030517u);
    memset(tri,0,sizeof tri);
    for(unsigned i=0;i<8;i++) {
        NvGpuTri *t=tri+i;
        float p[3][2]={{(float)(i%3),1},{31,(float)(i%5+2)},{(float)(i%7+4),31}};
        for(int k=0;k<3;k++) {
            t->v[k].pos[0]=p[k][0];t->v[k].pos[1]=p[k][1];t->v[k].pos[2]=10+(i%3)*7;
            t->v[k].pos[3]=1+(k%2)*0.5f;t->iw[k]=1/t->v[k].pos[3];
            for(int j=0;j<4;j++){t->v[k].d0[j]=(float)((i+k+j)%5)*0.2f;t->v[k].d1[j]=(float)((i+k+j)%3)*0.3f;}
            for(int st=0;st<4;st++) {
                t->stc[k][st][0]=(p[k][0]/17)*t->iw[k];t->stc[k][st][1]=(p[k][1]/19)*t->iw[k];
                t->stc[k][st][3]=t->iw[k];
            }
            t->uv[k][0]=p[k][0]*t->iw[k];t->uv[k][1]=p[k][1]*t->iw[k];t->vfog[k]=0.4f+k*0.2f;
        }
        float *a=t->v[0].pos,*b=t->v[1].pos,*c=t->v[2].pos;
        t->area=(b[0]-a[0])*(c[1]-a[1])-(b[1]-a[1])*(c[0]-a[0]);t->inv_area=1/t->area;
        t->inclusive[0]=nv2a_edge_inclusive(a,b,t->area);t->inclusive[1]=nv2a_edge_inclusive(b,c,t->area);t->inclusive[2]=nv2a_edge_inclusive(c,a,t->area);
        t->minx=t->miny=0;t->maxx=t->maxy=32;t->fogc[0]=0.1f;t->fogc[1]=0.3f;t->fogc[2]=0.7f;
    }
    for(unsigned pass=0;pass<(g.coverage?20u:16u);pass++) {
        memset(&state,0,sizeof state);memset(sources,0,sizeof sources);
        state.width=state.height=32;state.pitch=128;state.bpp=4;state.format=0x128;
        state.depth_available=state.depth_test=state.depth_mask=1;
        /* Sloped and repeated passes catch precision loss that a flat-depth
         * fixture cannot, particularly coplanar wheel/material passes. */
        for(unsigned i=0;i<8;i++)for(int k=0;k<3;k++) {
            if(pass>=6)tri[i].v[k].pos[2]=1000000.0f+(float)k*2400000.5f;
            else if(pass==5)tri[i].v[k].pos[2]=0.125f+(float)k*13.75f;
            else tri[i].v[k].pos[2]=10+(i%3)*7;
        }
        if(pass==8)for(unsigned i=0;i<4;i++)tri[i+4]=tri[i];state.depth_func=pass==2?0x202:0x203;
        if(pass==3){state.bpp=2;state.pitch=64;state.format=0x121;}
        if(pass==4){state.format=0x05050228;}
        state.zpass_enable=1;state.color_mask=0x01010101;state.blend_enable=1;
        state.blend_sfactor=0x302;state.blend_dfactor=0x303;state.blend_equation=0x8006;
        if(pass==2 || pass==7)state.control0=1u<<12;
        if(pass) {
            state.alpha_test=1;state.alpha_func=0x204;state.alpha_ref=8;
            state.rc.control=2;state.rc.stage_program=1|(1u<<5);
            state.rc.color_icw[0]=0x08040000;state.rc.color_icw[1]=0x090c0000;
            state.rc.alpha_icw[0]=0x18140000;state.rc.alpha_icw[1]=0x191c0000;
            state.rc.color_ocw[0]=state.rc.color_ocw[1]=0xc0;
            state.rc.alpha_ocw[0]=state.rc.alpha_ocw[1]=0xc0;
            for(int st=0;st<2;st++) {
                state.tex[st].width=state.tex[st].height=8;state.tex[st].valid=1;
                state.tex[st].format=0x12;state.tex[st].normalized=1;
                state.tex[st].addr_u=state.tex[st].addr_v=1;state.tex[st].filter=0x02020000;
                sources[st].raw=(const uint8_t*)texels;sources[st].bytes=sizeof texels;
                sources[st].opaque=texels;sources[st].decode=selftest_decode;
            }
            if(pass==4){state.rc.stage_program=3|(1u<<5);state.tex[0].cube=1;}
        }
        if(pass>=9 && pass<=12) {
            /* Captured ATV material: scaled texture/diffuse RGB, texture alpha,
             * fog, and vertices with non-unit diffuse alpha. All other stage
             * programs and arbitrary combiner words stay on the interpreter. */
            memset(&state.rc,0,sizeof state.rc);state.rc.control=0x11101;
            state.rc.stage_program=1;state.rc.color_icw[0]=0xC8C40000u;
            state.rc.color_ocw[0]=0x100C0u;state.rc.alpha_ocw[0]=0xC0u;
            state.rc.alpha_icw[0]=pass==10?0xD8D41010u:0xD8301010u;
            state.rc.final0=pass==10?0xCu:0x130C0300u;state.rc.final1=0x1C80u;
            state.tex[1].valid=0;state.alpha_test=pass==10;state.blend_enable=pass==10;
            for(unsigned i=0;i<8;i++)for(int k=0;k<3;k++) {
                tri[i].v[k].d0[0]=tri[i].v[k].d0[1]=tri[i].v[k].d0[2]=0.75f;
                tri[i].v[k].d0[3]=k==1?1.0f:100.0f/255.0f;
                if(pass==11)tri[i].v[k].pos[2]=6.98e29f+(float)k*8e26f;
            }
            if(pass==11)state.control0=1u<<12;
            if(pass==12) {
                state.tex[0].width=3;state.tex[0].height=5;
                for(unsigned i=0;i<8;i++)for(int k=0;k<3;k++) {
                    tri[i].stc[k][0][0]=(tri[i].v[k].pos[0]/17.0f-1.5f)*tri[i].iw[k];
                    tri[i].stc[k][0][1]=(tri[i].v[k].pos[1]/19.0f-1.5f)*tri[i].iw[k];
                }
            }
        }
        if(pass>=13 && pass<=15) {
            /* Exercise named-register lowering beyond the title's common
             * one-stage material: eight stages, per-stage constants, dot/mux,
             * blue-to-alpha, destination aliases and final SUM/EF registers. */
            state.rc.control=0x11008u|(pass==15?0x100u:0);
            for(unsigned st=0;st<8;st++) {
                uint32_t mapping[6]={0,0x10,0x30,0x08,0x18,0x20};
                uint32_t flags=mapping[st%6];
                if(pass==14)flags|=3u|0xC0u;
                if(pass==15)flags|=4u;
                state.rc.factor0[st]=0x14283C50u+st*0x08101820u;
                state.rc.factor1[st]=0xA0908070u-st*0x0507090Bu;
                state.rc.color_icw[st]=st?0x0C04390Du:0x08040000u;
                if(st==3)state.rc.color_icw[st]=0x010C020Du;
                state.rc.alpha_icw[st]=st?0x1C14301Du:0x18140000u;
                state.rc.color_ocw[st]=(flags<<12)|0xCDCu;
                state.rc.alpha_ocw[st]=((flags&0x3Cu)<<12)|0xCDCu;
            }
            state.rc.final0=pass==14?0x0F0E040Cu:(pass==15?0xCu:0x130C0300u);
            state.rc.final1=0x0E041C80u;
            state.alpha_test=0;
            state.rc.stage_program|=1u<<15;state.tex[3]=state.tex[0];sources[3]=sources[0];
            if(pass==15)state.rc.stage_program|=5u<<10;
        }
        if(pass>=16){
            static const float points[8][3][2]={
                {{1.5f,8.4999f},{30.5f,8.5001f},{30.5f,8.4999f}},
                {{17.4999f,1.5f},{17.5001f,30.5f},{17.4999f,30.5f}},
                {{1.5001f,1.5f},{30.5f,30.5001f},{30.5001f,30.5f}},
                {{10.4999f,10.4999f},{10.5001f,10.4999f},{10.5f,10.5001f}},
                {{1.5f,1.5f},{30.5f,1.5f},{30.5f,30.5f}},
                {{1.5f,1.5f},{30.5f,30.5f},{1.5f,30.5f}},
                {{0,0},{32,0},{0,32}},{{32,0},{32,32},{0,32}}};
            memset(&state.rc,0,sizeof state.rc);memset(state.tex,0,sizeof state.tex);state.alpha_test=0;state.blend_enable=1;
            for(unsigned i=0;i<8;i++){
                NvGpuTri *t=tri+i;
                for(unsigned k=0;k<3;k++){
                    unsigned v=(pass&1)?2-k:k;t->v[k].pos[0]=points[i][v][0];t->v[k].pos[1]=points[i][v][1];
                    if(pass>=18){t->v[k].pos[0]+=0.00001f;t->v[k].pos[1]-=0.00001f;}
                    t->v[k].pos[2]=10;t->v[k].pos[3]=t->iw[k]=1;
                    t->v[k].d0[0]=0.2f+(i%3)*0.2f;t->v[k].d0[1]=0.4f;t->v[k].d0[2]=0.6f;t->v[k].d0[3]=0.5f;
                }
                const float *a=t->v[0].pos,*b=t->v[1].pos,*c=t->v[2].pos;
                t->area=(b[0]-a[0])*(c[1]-a[1])-(b[1]-a[1])*(c[0]-a[0]);t->inv_area=1/t->area;
                for(unsigned k=0;k<3;k++)t->inclusive[k]=nv2a_edge_inclusive(t->v[k].pos,t->v[(k+1)%3].pos,t->area);
            }
        }
        for(int i=0;i<8;i++)tri[i].use_rc=pass!=0 && pass<16;
        for(unsigned i=0;i<32*32;i++) {
            original[i]=actual[i]=expected[i]=0xFF112233u;
            depth[i]=reference_depth[i]=pass==11?3.4e38f:(pass==2?10:(pass>=6?16777215.0f:100));
        }
        for(unsigned y=0;y<32;y++)for(unsigned x=0;x<32;x++) {
            const uint8_t *p=(const uint8_t*)actual+gpu_color_at(&state,x,y);
            unsigned at=y*32+x;
            if(state.bpp==4)memcpy(&expected[at],p,4);
            else {uint16_t c;memcpy(&c,p,2);expected[at]=0xFF000000u|((c&0xF800u)<<8)|((c&0x07E0u)<<5)|((c&31u)<<3);}
        }
        /* Partial initial occluders exercise general and simple bound rejection. */
        if(pass==12 || pass==13)for(unsigned i=0;i<32*32;i++)
            if(i%3)depth[i]=reference_depth[i]=2000000.0f;
        memcpy(initial_color,expected,sizeof initial_color);memcpy(initial_depth,reference_depth,sizeof initial_depth);
        if(g.native) {
            static const uint32_t empty_trace[8*32*32*4]={0};
            ID3D11DeviceContext_UpdateSubresource(g.context,(ID3D11Resource*)g.startup_trace.buffer,0,NULL,empty_trace,0,0);
        }
        if(nv_gpu_draw(&state,tri,4,(uint8_t*)actual,depth,32,sources)!=1)return 0;
        NvGpuState first_state=g.queued_state[g.state_count-1];
        state.color_mask^=0x00010000u;
        if(pass==8){state.depth_func=0x202;state.depth_mask=0;}
        if(nv_gpu_draw(&state,tri+4,4,(uint8_t*)actual,depth,32,sources)!=1)return 0;
        state=g.queued_state[g.state_count-1];
        NvGpuCount count={0},reference={0};
        /* bound[] contains precisely the decoded texture data used by GPU. */
        for(int y=0;y<32;y++)for(int x=0;x<32;x++)for(int i=0;i<8;i++) {
            unsigned at=(unsigned)y*32+x;NvGpuPixel p=gpu_pixel(i<4?first_state:state,tri[i],x,y,expected[at],reference_depth[at]);
            expected[at]=p.color;reference_depth[at]=p.depth;
            reference.depth_fail+=p.depth_fail;reference.pixels+=p.pixels;reference.zpass+=p.zpass;
        }
        if(nv_gpu_sync(&count)<0)return 0;
        unsigned differing=0;
        for(unsigned i=0;i<32*32;i++) {
            if(depth[i]!=reference_depth[i])++differing;
            uint32_t got;const uint8_t *p=(const uint8_t*)actual+gpu_color_at(&state,i%32,i/32);
            if(state.bpp==4)memcpy(&got,p,4);
            else {uint16_t c;memcpy(&c,p,2);got=0xFF000000u|((c&0xF800u)<<8)|((c&0x07E0u)<<5)|((c&31u)<<3);}
            for(unsigned shift=0;shift<32;shift+=8) {
                int a=(got>>shift)&255,b=(expected[i]>>shift)&255;
                if(abs(a-b)>1)++differing;
            }
        }
        gpu_reference_counts(&reference);
        if(differing || count.depth_fail!=reference.depth_fail || count.pixels!=reference.pixels || count.zpass!=reference.zpass) {
            fprintf(stderr,"[GPU-RASTER] offscreen mismatch pass=%u components=%u pixels=%llu/%llu depth_fail=%llu/%llu zpass=%u/%u\n",
                pass,differing,(unsigned long long)count.pixels,(unsigned long long)reference.pixels,
                (unsigned long long)count.depth_fail,(unsigned long long)reference.depth_fail,count.zpass,reference.zpass);
            gpu_selftest_capture(pass,&first_state,&state,tri,initial_color,initial_depth);
            return 0;
        }
    }
    g.host_color=NULL;g.host_depth=NULL;memset(&g.completed,0,sizeof g.completed);return 1;
}
static int selftest_material_decode(void *opaque,uint32_t face,uint32_t x,uint32_t y,uint32_t *v)
{
    (void)face;*v=((const uint32_t*)opaque)[y*256+x];return 1;
}
/* The small 32px comparison missed a real menu-surface failure. Exercise a
 * 256px target and texture, fractional edges, float24-scale depth, wrapping,
 * fog, and non-unit diffuse alpha without retaining any title assets. */
static int gpu_material_selftest(void)
{
    enum {side=256,pixels=side*side};
    uint32_t *storage=malloc((size_t)pixels*7*4);if(!storage)return -1;
    uint32_t *actual=storage,*expected=actual+pixels,*initial=expected+pixels;
    float *depth=(float*)(initial+pixels),*reference_depth=depth+pixels,*initial_depth=reference_depth+pixels;
    uint32_t *texels=(uint32_t*)(initial_depth+pixels);
    NvGpuState state;NvGpuTri tri[8];NvGpuTextureSource sources[4];
    memset(&state,0,sizeof state);memset(tri,0,sizeof tri);memset(sources,0,sizeof sources);
    state.width=state.height=side;state.pitch=side*4;state.bpp=4;state.format=0x128;
    state.depth_available=state.depth_test=state.depth_mask=state.zpass_enable=1;
    state.depth_func=0x203;state.control0=1u<<12;state.color_mask=0x01010101u;
    state.blend_sfactor=0x302;state.blend_dfactor=0x303;state.blend_equation=0x8006;
    state.rc.control=0x11101;state.rc.stage_program=1;state.rc.color_icw[0]=0xC8C40000u;
    state.rc.color_ocw[0]=0x100C0;state.rc.alpha_icw[0]=0xD8301010u;state.rc.alpha_ocw[0]=0xC0;
    state.rc.final0=0x130C0300u;state.rc.final1=0x1C80;
    state.tex[0].valid=state.tex[0].normalized=1;state.tex[0].width=state.tex[0].height=side;
    state.tex[0].format=0x12;state.tex[0].addr_u=state.tex[0].addr_v=1;state.tex[0].filter=0x02063F01u;
    for(unsigned i=0;i<pixels;i++) {
        actual[i]=expected[i]=initial[i]=0;depth[i]=reference_depth[i]=initial_depth[i]=3.4e38f;
        texels[i]=0xFF000000u|((16+(i%37))<<16)|((20+(i%43))<<8)|(22+(i%47));
    }
    sources[0].raw=(const uint8_t*)texels;sources[0].bytes=(size_t)pixels*4;
    sources[0].opaque=texels;sources[0].decode=selftest_material_decode;
    for(unsigned i=0;i<8;i++) {
        NvGpuTri *t=tri+i;float offset=(float)(i%4)*2.25f;
        float p[3][2]={{18.25f+offset,140.5f},{73.75f-offset,155.25f+offset},{31.5f+offset,245.75f-offset}};
        t->use_rc=1;t->minx=16;t->maxx=76;t->miny=136;t->maxy=248;
        t->fogc[0]=0.12f;t->fogc[1]=0.16f;t->fogc[2]=0.21f;
        for(unsigned k=0;k<3;k++) {
            t->v[k].pos[0]=p[k][0];t->v[k].pos[1]=p[k][1];
            t->v[k].pos[2]=7.98e29f-(float)(i/4)*6e27f-(float)k*3e27f;
            t->v[k].pos[3]=k==1?1.15f:0.85f;t->iw[k]=1/t->v[k].pos[3];
            t->v[k].d0[0]=0.75f;t->v[k].d0[1]=0.84f;t->v[k].d0[2]=0.96f;
            t->v[k].d0[3]=k==1?1.0f:100.0f/255.0f;t->vfog[k]=0.75f+(float)k*0.08f;
            t->stc[k][0][0]=(p[k][0]/64.0f-1.5f)*t->iw[k];
            t->stc[k][0][1]=(p[k][1]/97.0f-1.5f)*t->iw[k];t->stc[k][0][3]=t->iw[k];
        }
        const float *a=t->v[0].pos,*b=t->v[1].pos,*c=t->v[2].pos;
        t->area=(b[0]-a[0])*(c[1]-a[1])-(b[1]-a[1])*(c[0]-a[0]);t->inv_area=1/t->area;
        for(unsigned k=0;k<3;k++)t->inclusive[k]=nv2a_edge_inclusive(t->v[k].pos,t->v[(k+1)%3].pos,t->area);
    }
    if(g.literal.testing){
        nv_gpu_literal_fixture(&state.rc,g.literal.testing,g.literal.variant);
        state.tex[1]=state.tex[0];sources[1]=sources[0];
        for(unsigned stage=0;stage<8;stage++){
            state.rc.factor0[stage]=0x20406080u+stage*0x07132109u;
            state.rc.factor1[stage]=0xC0B0A090u-stage*0x09070503u;
        }
        state.rc.final_c0=0x789ABCDEu;state.rc.final_c1=0x13579BDFu;
        for(unsigned i=0;i<8;i++)for(unsigned v=0;v<3;v++){
            for(unsigned c=0;c<4;c++){
                tri[i].v[v].d1[c]=0.15f+0.17f*(float)((i+v+c)%5);
                tri[i].stc[v][1][c]=tri[i].stc[v][0][c];
            }
            tri[i].stc[v][1][0]+=0.37f*tri[i].iw[v];
        }
    }
    int ok=-1;
    if(nv_gpu_draw(&state,tri,4,(uint8_t*)actual,depth,side,sources)!=1)goto done;
    NvGpuState first_state=g.queued_state[g.state_count-1];
    if(nv_gpu_draw(&state,tri+4,4,(uint8_t*)actual,depth,side,sources)!=1)goto done;
    state=g.queued_state[g.state_count-1];NvGpuCount count={0},reference={0};
    for(unsigned i=0;i<8;i++)for(int y=tri[i].miny;y<tri[i].maxy;y++)for(int x=tri[i].minx;x<tri[i].maxx;x++) {
        unsigned at=(unsigned)y*side+x;NvGpuPixel p=gpu_pixel(i<4?first_state:state,tri[i],x,y,expected[at],reference_depth[at]);
        expected[at]=p.color;reference_depth[at]=p.depth;
        reference.depth_fail+=p.depth_fail;reference.pixels+=p.pixels;reference.zpass+=p.zpass;
    }
    if(nv_gpu_sync(&count)<0)goto done;
    unsigned differing=0;
    for(unsigned i=0;i<pixels;i++) {
        if(depth[i]!=reference_depth[i])++differing;
        for(unsigned shift=0;shift<32;shift+=8)if(abs((int)((actual[i]>>shift)&255)-(int)((expected[i]>>shift)&255))>1)++differing;
    }
    gpu_reference_counts(&reference);
    ok=!differing && count.pixels==reference.pixels && count.depth_fail==reference.depth_fail && count.zpass==reference.zpass;
    if(!ok) {
        fprintf(stderr,"[GPU-RASTER] 256px material mismatch components=%u pixels=%llu/%llu depth_fail=%llu/%llu zpass=%u/%u\n",
            differing,(unsigned long long)count.pixels,(unsigned long long)reference.pixels,
            (unsigned long long)count.depth_fail,(unsigned long long)reference.depth_fail,count.zpass,reference.zpass);
        gpu_selftest_capture(16,&first_state,&state,tri,initial,initial_depth);
    }
done:
    /* Only private fixture draws can be discarded here. No CPU pointer from
     * this heap may survive into another gate or publication to the guest. */
    g.active=g.resident=0;g.queued_count=g.state_count=g.queued_refs=0;
    for(unsigned i=0;i<128;i++)g.cache[i].queued=0;
    g.host_color=NULL;g.host_depth=NULL;memset(&g.completed,0,sizeof g.completed);
    free(storage);return ok;
}

#include "nv2a_gpu_store_test.inc"
#include "nv2a_gpu_texture_bridge_test.inc"
#include "nv2a_gpu_target_resume_test.inc"

/* Alternate native programs in one upload, checking indexed absolute IDs,
 * state addressing and order before exposing the context to guest code. */
static int gpu_mixed_selftest(void)
{
    uint32_t actual[1024],expected[1024],initial[1024];float depth[1024],reference_depth[1024],initial_depth[1024];
    NvGpuState state;NvGpuTri tri[8];NvGpuTextureSource sources[4];
    memset(&state,0,sizeof state);memset(tri,0,sizeof tri);memset(sources,0,sizeof sources);
    state.width=state.height=32;state.pitch=128;state.bpp=4;state.format=0x128;
    state.depth_available=state.depth_test=state.depth_mask=state.zpass_enable=1;state.depth_func=0x203;state.color_mask=0x01010101u;
    state.rc.control=2;state.rc.color_icw[0]=0x08040000;state.rc.color_icw[1]=0x090C0000;
    state.rc.alpha_icw[0]=0x18140000;state.rc.alpha_icw[1]=0x191C0000;
    state.rc.color_ocw[0]=state.rc.color_ocw[1]=state.rc.alpha_ocw[0]=state.rc.alpha_ocw[1]=0xC0;
    for(unsigned at=0;at<1024;at++){actual[at]=expected[at]=initial[at]=0xFF112233u;depth[at]=reference_depth[at]=initial_depth[at]=100;}
    if(g.native){static const uint32_t empty[8*1024*4]={0};ID3D11DeviceContext_UpdateSubresource(g.context,(ID3D11Resource*)g.startup_trace.buffer,0,NULL,empty,0,0);}
    for(unsigned i=0;i<8;i++){
        NvGpuTri *t=tri+i;t->use_rc=i&1;t->maxx=t->maxy=32;float points[3][2]={{i*2.0f,1},{31,i*2.0f+2},{i*2.0f+1,31}};
        for(unsigned k=0;k<3;k++){
            t->v[k].pos[0]=points[k][0];t->v[k].pos[1]=points[k][1];t->v[k].pos[2]=30-i*2.0f;t->v[k].pos[3]=t->iw[k]=1;
            t->v[k].d0[0]=0.2f+i*0.08f;t->v[k].d0[1]=0.6f;t->v[k].d0[2]=0.8f;t->v[k].d0[3]=0.75f;
        }
        const float *a=t->v[0].pos,*b=t->v[1].pos,*c=t->v[2].pos;
        t->area=(b[0]-a[0])*(c[1]-a[1])-(b[1]-a[1])*(c[0]-a[0]);t->inv_area=1/t->area;
        for(unsigned k=0;k<3;k++)t->inclusive[k]=nv2a_edge_inclusive(t->v[k].pos,t->v[(k+1)%3].pos,t->area);
    }
    int ok=-1;NvGpuState first_state=state,last_state=state;
    for(unsigned i=0;i<8;i++){
        if(nv_gpu_draw(&state,tri+i,1,(uint8_t*)actual,depth,32,sources)!=1)goto done;
        last_state=g.queued_state[g.state_count-1];if(!i)first_state=last_state;
    }
    NvGpuCount reference={0},count={0};
    for(unsigned i=0;i<8;i++)for(int y=0;y<32;y++)for(int x=0;x<32;x++){
        unsigned at=(unsigned)y*32+x;NvGpuPixel p=gpu_pixel(state,tri[i],x,y,expected[at],reference_depth[at]);
        expected[at]=p.color;reference_depth[at]=p.depth;reference.depth_fail+=p.depth_fail;reference.pixels+=p.pixels;reference.zpass+=p.zpass;
    }
    if(nv_gpu_sync(&count)<0)goto done;
    unsigned differing=0;
    for(unsigned at=0;at<1024;at++){
        if(depth[at]!=reference_depth[at])++differing;
        for(unsigned shift=0;shift<32;shift+=8)if(abs((int)((actual[at]>>shift)&255)-(int)((expected[at]>>shift)&255))>1)++differing;
    }
    gpu_reference_counts(&reference);
    ok=!differing && count.depth_fail==reference.depth_fail && count.pixels==reference.pixels && count.zpass==reference.zpass;
    if(!ok){fprintf(stderr,"[GPU-RASTER] mixed upload mismatch components=%u pixels=%llu/%llu depth_fail=%llu/%llu zpass=%u/%u\n",differing,(unsigned long long)count.pixels,(unsigned long long)reference.pixels,(unsigned long long)count.depth_fail,(unsigned long long)reference.depth_fail,count.zpass,reference.zpass);gpu_selftest_capture(21,&first_state,&last_state,tri,initial,initial_depth);}
done:
    g.active=g.resident=0;g.queued_count=g.state_count=g.queued_refs=0;for(unsigned i=0;i<128;i++)g.cache[i].queued=0;
    g.host_color=NULL;g.host_depth=NULL;memset(&g.completed,0,sizeof g.completed);return ok;
}

static int gpu_report_selftest(void)
{
    NvGpuState s;NvGpuTri t;NvGpuTextureSource sources[4];
    memset(&s,0,sizeof s);memset(&t,0,sizeof t);memset(sources,0,sizeof sources);
    s.width=s.height=16;s.pitch=64;s.bpp=4;s.format=0x128;
    s.depth_available=s.depth_test=s.depth_mask=s.zpass_enable=1;s.depth_func=0x203;
    s.color_mask=0x01010101u;s.blend_enable=1;s.blend_sfactor=0x302;s.blend_dfactor=0x303;s.blend_equation=0x8006;
    for(int k=0;k<3;k++) {
        t.v[k].pos[0]=k==1?16:0;t.v[k].pos[1]=k==2?16:0;t.v[k].pos[2]=10;t.iw[k]=1;
        for(int j=0;j<4;j++)t.v[k].d0[j]=(float)(j+2)*0.15f;
    }
    t.area=256;t.inv_area=1.0f/256;t.maxx=t.maxy=16;
    for(int k=0;k<3;k++)t.inclusive[k]=nv2a_edge_inclusive(t.v[k].pos,t.v[(k+1)%3].pos,t.area);
    uint32_t actual[256],expected[256];float depth[256],expected_depth[256];
    for(unsigned at=0;at<256;at++){actual[at]=expected[at]=0xFF112233u;depth[at]=expected_depth[at]=100;}
    NvGpuTri behind=t;for(unsigned k=0;k<3;k++)behind.v[k].pos[2]=20;
    NvGpuCount reference={0},count={0};
    for(unsigned draw=0;draw<3;draw++)for(int y=0;y<16;y++)for(int x=0;x<16;x++) {
        unsigned at=(unsigned)y*16+x;NvGpuPixel p=gpu_pixel(s,draw==1?behind:t,x,y,expected[at],expected_depth[at]);
        expected[at]=p.color;expected_depth[at]=p.depth;
        reference.pixels+=p.pixels;reference.depth_fail+=p.depth_fail;
        if(draw==2)reference.zpass+=p.zpass;
        if((p.pixel_max&0xFFFFFFu)>(reference.pixel_max&0xFFFFFFu))reference.pixel_max=p.pixel_max;
    }
    if(nv_gpu_draw(&s,&t,1,(uint8_t*)actual,depth,16,sources)!=1 ||
       nv_gpu_draw(&s,&behind,1,(uint8_t*)actual,depth,16,sources)!=1 || nv_gpu_reset_report()!=1)return -1;
    /* A reset must submit the first draw without copying its pixels to CPU. */
    if(actual[1]!=0xFF112233u || depth[1]!=100)return -1;
    if(nv_gpu_draw(&s,&t,1,(uint8_t*)actual,depth,16,sources)!=1 || nv_gpu_sync(&count)<0)return -1;
    gpu_reference_counts(&reference);
    int ok=count.pixels==reference.pixels && count.depth_fail==reference.depth_fail && count.zpass==reference.zpass;
    for(unsigned shift=0;shift<32;shift+=8)
        if(abs((int)((count.pixel_max>>shift)&255)-(int)((reference.pixel_max>>shift)&255))>1)ok=0;
    for(unsigned at=0;at<256;at++) {
        if(depth[at]!=expected_depth[at])ok=0;
        for(unsigned shift=0;shift<32;shift+=8)
            if(abs((int)((actual[at]>>shift)&255)-(int)((expected[at]>>shift)&255))>1)ok=0;
    }
    if(!ok)fprintf(stderr,"[GPU-RASTER] visibility-reset mismatch pixels=%llu/%llu depth_fail=%llu/%llu zpass=%u/%u pixel_max=%08X/%08X\n",
        (unsigned long long)count.pixels,(unsigned long long)reference.pixels,
        (unsigned long long)count.depth_fail,(unsigned long long)reference.depth_fail,
        count.zpass,reference.zpass,count.pixel_max,reference.pixel_max);
    g.host_color=NULL;g.host_depth=NULL;g.resident=0;memset(&g.completed,0,sizeof g.completed);return ok;
}
static int gpu_clear_selftest(void)
{
    NvGpuState s;NvGpuTri t;NvGpuTextureSource sources[4];
    memset(&s,0,sizeof s);memset(&t,0,sizeof t);memset(sources,0,sizeof sources);
    s.width=32;s.height=17;s.format=0x128;
    s.depth_available=s.depth_test=s.depth_mask=s.zpass_enable=1;s.depth_func=0x203;
    s.color_mask=0x01010101u;s.blend_enable=1;s.blend_sfactor=0x302;s.blend_dfactor=0x303;s.blend_equation=0x8006;
    t.maxx=32;t.maxy=17;t.area=544;t.inv_area=1.0f/544;
    for(unsigned k=0;k<3;k++) {
        t.v[k].pos[0]=k==1?32:0;t.v[k].pos[1]=k==2?17:0;t.v[k].pos[2]=10;t.iw[k]=1;
        for(unsigned j=0;j<4;j++)t.v[k].d0[j]=(float)(j+2)*0.15f;
    }
    for(unsigned k=0;k<3;k++)t.inclusive[k]=nv2a_edge_inclusive(t.v[k].pos,t.v[(k+1)%3].pos,t.area);
    int ok=1;
    for(unsigned mode=0;mode<2;mode++) {
        uint8_t actual[17*144];uint32_t expected[17*32];float depth[17*40],expected_depth[17*32];
        memset(actual,0xA5,sizeof actual);for(unsigned i=0;i<17*40;i++)depth[i]=-123;
        s.bpp=mode?2:4;s.pitch=36*s.bpp;g.resident=0;
        for(unsigned y=0;y<17;y++)for(unsigned x=0;x<32;x++) {
            uint32_t value=0xFF102030u;uint16_t packed=0x1106;
            memcpy(actual+y*s.pitch+x*s.bpp,mode?(void*)&packed:(void*)&value,s.bpp);
            expected[y*32+x]=value;depth[y*40+x]=expected_depth[y*32+x]=100;
        }
        NvGpuCount reference={0},count={0};
        NvGpuTri behind=t;for(unsigned k=0;k<3;k++)behind.v[k].pos[2]=20;
        for(unsigned draw=0;draw<3;draw++) {
            NvGpuTri primitive=draw==1?behind:t;
            if(nv_gpu_draw(&s,&primitive,1,actual,depth,40,sources)!=1){ok=-1;goto clear_done;}
            for(unsigned y=0;y<17;y++)for(unsigned x=0;x<32;x++) {
                unsigned at=y*32+x;NvGpuPixel p=gpu_pixel(s,primitive,x,y,expected[at],expected_depth[at]);
                expected[at]=p.color;expected_depth[at]=p.depth;
                reference.pixels+=p.pixels;reference.depth_fail+=p.depth_fail;reference.zpass+=p.zpass;
                if((p.pixel_max&0xFFFFFFu)>(reference.pixel_max&0xFFFFFFu))reference.pixel_max=p.pixel_max;
            }
            if(draw==0) {
                if(nv_gpu_clear_color(&s,actual,3,2,27,16,0x4D89ABCDu)!=1){ok=-1;goto clear_done;}
                for(unsigned y=2;y<16;y++)for(unsigned x=3;x<27;x++)expected[y*32+x]=mode?0xFF88A8C8u:0x4D89ABCDu;
            }else if(draw==1) {
                if(nv_gpu_clear_depth(depth,40,37.5f)!=1){ok=-1;goto clear_done;}
                for(unsigned i=0;i<17*32;i++)expected_depth[i]=37.5f;
            }
            /* All operations remain pending until the real completion below. */
            if(g.selftest && g.readbacks){ok=-1;goto clear_done;}
        }
        if(nv_gpu_sync(&count)<0){ok=-1;goto clear_done;}
        for(unsigned y=0;y<17;y++)for(unsigned x=0;x<36;x++) {
            if(x<32) {
                uint32_t value=0;const uint8_t *p=actual+y*s.pitch+x*s.bpp;
                if(mode){uint16_t packed;memcpy(&packed,p,2);value=0xFF000000u|((packed&0xF800u)<<8)|((packed&0x07E0u)<<5)|((packed&31u)<<3);}
                else memcpy(&value,p,4);
                if(depth[y*40+x]!=expected_depth[y*32+x])ok=0;
                for(unsigned shift=0;shift<32;shift+=8)if(abs((int)((value>>shift)&255)-(int)((expected[y*32+x]>>shift)&255))>1)ok=0;
            }else for(unsigned k=0;k<s.bpp;k++)if(actual[y*s.pitch+x*s.bpp+k]!=0xA5)ok=0;
        }
        for(unsigned y=0;y<17;y++)for(unsigned x=32;x<40;x++)if(depth[y*40+x]!=-123)ok=0;
        gpu_reference_counts(&reference);
        if(count.pixels!=reference.pixels || count.depth_fail!=reference.depth_fail || count.zpass!=reference.zpass)ok=0;
        for(unsigned shift=0;shift<32;shift+=8)if(abs((int)((count.pixel_max>>shift)&255)-(int)((reference.pixel_max>>shift)&255))>1)ok=0;
        if(!ok)fprintf(stderr,"[GPU-RASTER] surface-clear mismatch bpp=%u pixels=%llu/%llu depth_fail=%llu/%llu zpass=%u/%u\n",s.bpp,
            (unsigned long long)count.pixels,(unsigned long long)reference.pixels,(unsigned long long)count.depth_fail,(unsigned long long)reference.depth_fail,count.zpass,reference.zpass);
    }
clear_done:
    g.active=g.resident=0;g.queued_count=g.state_count=g.queued_refs=0;for(unsigned i=0;i<128;i++)g.cache[i].queued=0;
    g.host_color=NULL;g.host_depth=NULL;memset(&g.completed,0,sizeof g.completed);return ok;
}
/* Replay a suitable real batch into private host mirrors. GPU state,
 * textures, triangles and arithmetic are unchanged. The actual accepted batch
 * is subsequently executed once on its original backend and guest target.
 * Only a byte-exact color/depth and visibility match permits a later switch. */
static int gpu_backend_benchmark(void)
{
    const unsigned count=g.queued_count,states=g.state_count,refs=g.queued_refs;
    const int native=g.native,simple=g.queue_simple;
    const unsigned left=g.dirty_left,right=g.dirty_right,top=g.dirty_top,bottom=g.dirty_bottom;
    NvGpuState target=g.target;uint8_t *host=g.host_color;float *depth=g.host_depth;unsigned stride=g.depth_stride;
    NvGpuCount completed=g.completed;
    size_t color_bytes=gpu_target_color_bytes(&target),depth_bytes=depth?gpu_target_depth_bytes(&target,stride):0;
    uint8_t *scratch[2]={NULL,NULL};float *zscratch[2]={NULL,NULL};
    int ok=1,equal=1;uint32_t zpass[2]={0};double best[2]={1e30,1e30};
    g.auto_done=1;g.auto_busy=1;
    for(unsigned mode=0;mode<2;mode++) {
        scratch[mode]=malloc(color_bytes);if(depth_bytes)zscratch[mode]=malloc(depth_bytes);
        if(!scratch[mode] || (depth_bytes && !zscratch[mode])){ok=0;goto restore;}
    }
    g.selftest=1;
    for(unsigned round=0;round<2 && ok>0;round++)for(unsigned mode=0;mode<2;mode++) {
        /* Alternating order avoids always charging a cold first upload to
         * the same backend. Keep the better of two completed measurements. */
        unsigned backend=round?(1u-mode):mode;
        g.native=backend==0;
        memcpy(scratch[backend],host,color_bytes);
        if(depth_bytes)memcpy(zscratch[backend],depth,depth_bytes);
        g.active=g.resident=0;g.queued_count=g.state_count=g.queued_refs=0;
        memset(&g.completed,0,sizeof g.completed);
        double begin=gpu_ms();
        if(gpu_target(&target,scratch[backend],zscratch[backend],stride)!=1){ok=-1;break;}
        g.queued_count=count;g.state_count=states;g.queued_refs=refs;g.queue_simple=simple;
        if(!g.native) {
            g.queue_simple=g.simple_shader!=NULL;
            for(unsigned i=0;i<count;i++)if(!g.queued_kind[i]){g.queue_simple=0;break;}
        }
        g.dirty_left=left;g.dirty_right=right;g.dirty_top=top;g.dirty_bottom=bottom;
        if(!gpu_readback_current("backend-benchmark")){ok=-1;break;}
        double elapsed=gpu_ms()-begin;if(elapsed<best[backend])best[backend]=elapsed;
        zpass[backend]=g.completed.zpass;
        if(round==0 && backend==1) {
            if(memcmp(scratch[0],scratch[1],color_bytes) ||
               (depth_bytes && memcmp(zscratch[0],zscratch[1],depth_bytes)) || zpass[0]!=zpass[1])equal=0;
        }
    }
    if(ok>0 && (memcmp(scratch[0],scratch[1],color_bytes) ||
       (depth_bytes && memcmp(zscratch[0],zscratch[1],depth_bytes)) || zpass[0]!=zpass[1]))equal=0;
restore:
    /* Even on failure, no private destination may remain reachable. The
     * caller stops accepted rendering after a real device/readback failure. */
    g.native=native;g.active=g.resident=0;g.queued_count=g.state_count=g.queued_refs=0;
    g.pending_count=0;g.host_color=NULL;g.host_depth=NULL;g.selftest=0;g.completed=completed;
    if(ok>=0 && gpu_target(&target,host,depth,stride)!=1)ok=-1;
    g.queued_count=count;g.state_count=states;g.queued_refs=refs;g.queue_simple=simple;
    g.dirty_left=left;g.dirty_right=right;g.dirty_top=top;g.dirty_bottom=bottom;
    for(unsigned i=0;i<128;i++)g.cache[i].queued=0;
    for(unsigned i=0;i<states;i++)for(unsigned st=0;st<4;st++)
        if(g.queued_state[i].tex[st].valid)g.cache[g.queued_state[i].tex[st].cache_slot].queued=1;
    for(unsigned mode=0;mode<2;mode++){free(scratch[mode]);free(zscratch[mode]);}
    g.auto_busy=0;
    if(ok>0) {
        g.auto_native_ms=best[0];g.auto_compute_ms=best[1];
        g.auto_choose_compute=equal && best[1]<best[0]*0.85;
        fprintf(stderr,"[GPU-BACKEND-BENCH] triangles=%u states=%u target=%ux%u native_ms=%.3f compute_ms=%.3f exact_output=%u choose=%s\n",
            count,states,target.width,target.height,best[0],best[1],(unsigned)equal,g.auto_choose_compute?"compute":"native");
    }else if(!ok)fprintf(stderr,"[GPU-BACKEND-BENCH] allocation unavailable; native rendering retained\n");
    return ok<0?-1:1;
}
static int gpu_queued_readback_selftest(void)
{
    uint32_t color[6][16*16],expected[6][16*16];float depth[6][16*16],expected_depth[6][16*16];
    NvGpuCount reference={0},count={0};NvGpuTextureSource sources[4]={0};NvGpuState state={0};NvGpuTri tri={0};
    state.width=state.height=16;state.pitch=64;state.bpp=4;state.format=0x128;state.color_mask=0x01010101;
    state.depth_available=state.depth_test=state.depth_mask=state.zpass_enable=1;state.depth_func=0x203;
    tri.area=256;tri.inv_area=1.0f/256;tri.maxx=tri.maxy=16;
    const float xy[3][2]={{0,0},{16,0},{0,16}};
    for(unsigned k=0;k<3;k++) {
        tri.v[k].pos[0]=xy[k][0];tri.v[k].pos[1]=xy[k][1];tri.v[k].pos[2]=20;tri.v[k].pos[3]=tri.iw[k]=1;
        tri.v[k].d0[3]=1;
    }
    for(unsigned k=0;k<3;k++)tri.inclusive[k]=nv2a_edge_inclusive(tri.v[k].pos,tri.v[(k+1)%3].pos,tri.area);
    g.active=g.resident=0;memset(&g.completed,0,sizeof g.completed);
    int ok=1;
    for(unsigned i=0;i<6;i++) {
        for(unsigned at=0;at<256;at++){color[i][at]=expected[i][at]=0xFF112233u+i;depth[i][at]=expected_depth[i][at]=100;}
        for(unsigned k=0;k<3;k++){tri.v[k].d0[0]=(i+1)/8.0f;tri.v[k].d0[1]=0.5f;tri.v[k].d0[2]=0.25f;}
        for(unsigned y=0;y<16;y++)for(unsigned x=0;x<16;x++) {
            unsigned at=y*16+x;NvGpuPixel p=gpu_pixel(state,tri,(int)x,(int)y,expected[i][at],expected_depth[i][at]);
            expected[i][at]=p.color;expected_depth[i][at]=p.depth;
            reference.depth_fail+=p.depth_fail;reference.pixels+=p.pixels;reference.zpass+=p.zpass;
            if((p.pixel_max&0xFFFFFF)>(reference.pixel_max&0xFFFFFF))reference.pixel_max=p.pixel_max;
        }
        if(nv_gpu_draw(&state,&tri,1,(uint8_t*)color[i],depth[i],16,sources)!=1){ok=-1;goto done;}
        if(i && i<5 && (g.pending_count!=i || color[0][0]!=0xFF112233u)){ok=0;goto done;}
    }
    if(g.pending_count!=1){ok=0;goto done;}
    if(nv_gpu_sync(&count)<0){ok=-1;goto done;}
    gpu_reference_counts(&reference);
    if(memcmp(depth,expected_depth,sizeof depth) || count.depth_fail!=reference.depth_fail ||
       count.pixels!=reference.pixels || count.zpass!=reference.zpass)ok=0;
    for(unsigned shift=0;shift<32;shift+=8)
        if(abs((int)((count.pixel_max>>shift)&255)-(int)((reference.pixel_max>>shift)&255))>1)ok=0;
    for(unsigned i=0;i<6*256;i++)for(unsigned shift=0;shift<32;shift+=8)
        if(abs((int)((((uint32_t*)color)[i]>>shift)&255)-(int)((((uint32_t*)expected)[i]>>shift)&255))>1)ok=0;
done:
    /* All private stack destinations must complete before they leave scope. */
    if(!g.readback_failed && nv_gpu_sync(&count)<0)ok=-1;
    g.pending_count=0;g.active=g.resident=0;g.host_color=NULL;g.host_depth=NULL;
    memset(&g.completed,0,sizeof g.completed);return ok;
}
#include "nv2a_gpu_geometry_impl.inc"
void nv_gpu_report(void)
{
    if(InterlockedCompareExchange(&s_startup,0,0)!=2)return;
    gpu_raw_report();gpu_timing_report();
    fprintf(stderr,"[GPU-COMPLETION] enabled=%d waits=%llu marker_wait_ms=%.3f full_surface_pixels_deferred=%llu guest_access_drains=%llu feedback_enabled=%d feedback_snapshots=%llu feedback_fallbacks=%llu\n",
        g.completion.enabled,(unsigned long long)g.completion.waits,g.completion.wait_ms,
        (unsigned long long)g.completion.avoided_pixels,(unsigned long long)g.completion.access_drains,
        g.completion.feedback_enabled,(unsigned long long)g.completion.feedback_snapshots,
        (unsigned long long)g.completion.feedback_fallbacks);
    g.completion.waits=g.completion.avoided_pixels=g.completion.access_drains=0;
    g.completion.feedback_snapshots=g.completion.feedback_fallbacks=0;g.completion.wait_ms=0;
    for(unsigned shape=0;shape<4;shape++){
        const char *name=shape==0?"640x480":shape==1?"256x256":shape==2?"128x128":"other";
        fprintf(stderr,"[GPU-TARGET-SHAPE] size=%s binds=%llu queued=%llu readbacks=%llu pixels=%llu map_wait_ms=%.3f empty_skips=%llu\n",
            name,(unsigned long long)g.target_shape[shape].binds,
            (unsigned long long)g.target_shape[shape].queued,
            (unsigned long long)g.target_shape[shape].readbacks,
            (unsigned long long)g.target_shape[shape].pixels,
            g.target_shape[shape].map_wait_ms,
            (unsigned long long)g.target_shape[shape].empty_skips);
    }
    memset(g.target_shape,0,sizeof g.target_shape);
    fprintf(stderr,"[GPU-COMBINER] vector_writes=%u registers=%s\n",(unsigned)(g.native && !g.array_registers && g.vector_combiner),g.array_registers?"array":"named");
    fprintf(stderr,"[GPU-SAMPLING] shared_addresses=%u\n",(unsigned)(g.native && g.quad_sampling));
    gpu_literal_report();
    fprintf(stderr,"[GPU-MICRO-FUSE] enabled=%u fused_draws=%llu draws_avoided=%llu generic_triangles=%llu\n",
        (unsigned)(g.native && g.native_batch && g.micro_fuse.enabled),
        (unsigned long long)g.micro_fuse.fused_draws,
        (unsigned long long)g.micro_fuse.runs_saved,
        (unsigned long long)g.micro_fuse.triangles);
    g.micro_fuse.fused_draws=g.micro_fuse.runs_saved=g.micro_fuse.triangles=0;
    fprintf(stderr,"[GPU-STORE] enabled=%u ready_mask=%u rejected=%u compare=bit-exact writer_barrier=1\n",(unsigned)(g.native && g.skip_unchanged),g.store_ready,g.store_rejected);
    gpu_material_report();
    fprintf(stderr,"[GPU-EMPTY-REJECT] enabled=%u quiet=%u eligible_batches=%llu eligible_triangles=%llu\n",
        (unsigned)(g.native && g.empty_reject),(unsigned)!g.native_diagnostics,
        (unsigned long long)g.empty_reject_batches,(unsigned long long)g.empty_reject_triangles);
    g.empty_reject_batches=g.empty_reject_triangles=0;
    fprintf(stderr,"[GPU-DEPTH-SEED] enabled=%u prefix=%u batches=%llu copies=%llu bytes=%llu unsafe=%llu cold=%llu small=%llu allocation_failures=%llu partial_batches=%llu partial_triangles=%llu\n",
        (unsigned)(g.native && g.seed.enabled),(unsigned)g.seed.prefix_enabled,(unsigned long long)g.seed.batches,(unsigned long long)g.seed.copies,(unsigned long long)g.seed.bytes,
        (unsigned long long)g.seed.unsafe,(unsigned long long)g.seed.cold,(unsigned long long)g.seed.small_batches,(unsigned long long)g.seed.allocation_failures,
        (unsigned long long)g.seed.partial_batches,(unsigned long long)g.seed.partial_triangles);
    g.seed.batches=g.seed.copies=g.seed.bytes=g.seed.unsafe=g.seed.cold=g.seed.small_batches=g.seed.allocation_failures=0;
    g.seed.partial_batches=g.seed.partial_triangles=0;
    fprintf(stderr,"[GPU-SEED-REUSE] enabled=%u snapshots_reused=%llu copy_bytes_avoided=%llu\n",
        (unsigned)(g.native && g.seed.enabled && g.seed.reuse_enabled),
        (unsigned long long)g.seed.reuses,(unsigned long long)g.seed.reused_bytes);
    g.seed.reuses=g.seed.reused_bytes=0;
    fprintf(stderr,"[GPU-FLUSH-CPU] batches=%llu upload_ms=%.3f raw_submit_ms=%.3f raster_submit_ms=%.3f total_ms=%.3f\n",
        (unsigned long long)g.flush_time.batches,g.flush_time.upload_ms,g.flush_time.raw_ms,g.flush_time.raster_ms,g.flush_time.total_ms);
    memset(&g.flush_time,0,sizeof g.flush_time);
    fprintf(stderr,"[GPU-DEFER-READBACK] enabled=%d deferred=%llu issued=%llu avoided=%llu avoided_bytes=%llu enqueue_ms=%.3f\n",
        g.deferred_readback.enabled,(unsigned long long)g.deferred_readback.deferred,
        (unsigned long long)g.deferred_readback.issued,(unsigned long long)g.deferred_readback.avoided,
        (unsigned long long)g.deferred_readback.avoided_bytes,g.deferred_readback.enqueue_ms);
    g.deferred_readback.deferred=g.deferred_readback.issued=g.deferred_readback.avoided=g.deferred_readback.avoided_bytes=0;
    g.deferred_readback.enqueue_ms=0;
    fprintf(stderr,"[GPU-TARGET-RETIRE] enabled=%d retired=%llu surface_map_bytes_avoided=%llu counter_reads=%llu no_counter_reads=%llu\n",
        g.resume.enabled && g.resume.retire_pixels,(unsigned long long)g.resume.retired,
        (unsigned long long)g.resume.retired_bytes,(unsigned long long)g.resume.counter_reads,
        (unsigned long long)g.resume.empty_retired);
    g.resume.retired=g.resume.retired_bytes=g.resume.counter_reads=g.resume.empty_retired=0;
    fprintf(stderr,"[GPU-TARGET-RESUME] enabled=%d restores=%llu checked=%llu host_changed=%llu conflicts=%llu allocation_failures=%llu gpu_bytes=%llu\n",
        g.resume.enabled,(unsigned long long)g.resume.restores,(unsigned long long)g.resume.checks,
        (unsigned long long)g.resume.changed,(unsigned long long)g.resume.conflicts,
        (unsigned long long)g.resume.allocation_failures,(unsigned long long)g.resume.bytes);
    g.resume.restores=g.resume.checks=g.resume.changed=g.resume.conflicts=g.resume.allocation_failures=g.resume.bytes=0;
    fprintf(stderr,"[GPU-TEXTURE-BRIDGE] enabled=%d snapshots=%llu packs=%llu hits=%llu gpu_texel_bytes=%llu candidates=%llu rejected=%llu\n",
        g.bridge.enabled,(unsigned long long)g.bridge.snapshots,(unsigned long long)g.bridge.packs,
        (unsigned long long)g.bridge.hits,(unsigned long long)g.bridge.bytes,
        (unsigned long long)g.bridge.eligible,(unsigned long long)g.bridge.misses);
    g.bridge.snapshots=g.bridge.packs=g.bridge.hits=g.bridge.bytes=g.bridge.eligible=g.bridge.misses=0;
    fprintf(stderr,"[GPU-CPU-WRITE] disjoint_clears=%llu\n",(unsigned long long)g.disjoint_writes);
    g.disjoint_writes=0;
    fprintf(stderr,"[GPU-BACKEND-AUTO] enabled=%u tested=%u selected=%s native_ms=%.3f compute_ms=%.3f\n",
        (unsigned)g.auto_enabled,(unsigned)g.auto_done,g.native?"native":"compute",g.auto_native_ms,g.auto_compute_ms);
    fprintf(stderr,"[GPU-RASTER] draws=%llu dispatches=%llu triangles=%llu tile_groups=%llu texture_uploads=%llu readbacks=%llu fallback_groups=%llu submit_ms=%.3f readback_ms=%.3f texture_ms=%.3f target_ms=%.3f map_ms=%.3f copy_ms=%.3f cache_slots=128 backend=%s registers=%s simple_dispatches=%llu simple_triangles=%llu color_uploads=%llu depth_uploads=%llu color_reuses=%llu depth_reuses=%llu report_resets=%llu\n",
        (unsigned long long)g.draws,(unsigned long long)g.dispatches,(unsigned long long)g.tris,(unsigned long long)g.groups,
        (unsigned long long)g.uploads,(unsigned long long)g.readbacks,(unsigned long long)g.fallbacks,g.submit_ms,g.readback_ms,g.texture_ms,g.target_ms,g.map_ms,g.copy_ms,g.native?"native-rov":"compute",g.array_registers?"array":"named",
        (unsigned long long)g.simple_dispatches,(unsigned long long)g.simple_tris,
        (unsigned long long)g.color_uploads,(unsigned long long)g.depth_uploads,(unsigned long long)g.color_reuses,(unsigned long long)g.depth_reuses,(unsigned long long)g.report_resets);
    fprintf(stderr,"[GPU-READBACK-BATCH] enabled=%d completions=%llu older_targets=%llu current_copy_bytes=%llu\n",
        g.readback_batch.enabled,(unsigned long long)g.readback_batch.batches,
        (unsigned long long)g.readback_batch.older_targets,(unsigned long long)g.readback_batch.current_bytes);
    g.readback_batch.batches=g.readback_batch.older_targets=g.readback_batch.current_bytes=0;
    fprintf(stderr,"[GPU-REPORT-ONLY] enabled=%u reports=%llu fallback=%llu counter_bytes=%llu wait_ms=%.3f\n",
        (unsigned)g.report_only_enabled,(unsigned long long)g.report_only,(unsigned long long)g.report_fallback,
        (unsigned long long)g.report_bytes,g.report_wait_ms);
    g.report_only=g.report_fallback=g.report_bytes=0;g.report_wait_ms=0;
    fprintf(stderr,"[GPU-READBACK-TIME] simd=%u current=%llu queued=%llu current_enqueue_ms=%.3f map_wait_ms=%.3f canonical_unpack_ms=%.3f publish_ms=%.3f counters_ms=%.3f unmap_ms=%.3f capture_ms=%.3f unpacked_pixels=%llu simd_rows=%llu scalar_rows=%llu\n",
        (unsigned)g.readback_simd,(unsigned long long)g.readback_time.current,(unsigned long long)g.readback_time.queued,
        g.readback_time.enqueue_ms,g.readback_time.map_wait_ms,g.readback_time.unpack_ms,g.readback_time.publish_ms,
        g.readback_time.counters_ms,g.readback_time.unmap_ms,g.readback_time.capture_ms,
        (unsigned long long)g.readback_time.pixels,(unsigned long long)g.readback_time.simd_rows,(unsigned long long)g.readback_time.scalar_rows);
    fprintf(stderr,"[GPU-READBACK-ROWS] copies=%llu partial=%llu empty=%llu bytes=%llu bytes_avoided=%llu\n",
        (unsigned long long)g.readback_time.surface_copies,(unsigned long long)g.readback_time.surface_partial,
        (unsigned long long)g.readback_time.surface_empty,(unsigned long long)g.readback_time.surface_bytes,
        (unsigned long long)g.readback_time.surface_bytes_avoided);
    memset(&g.readback_time,0,sizeof g.readback_time);
    fprintf(stderr,"[GPU-READBACK-QUEUE] enabled=%u copies=%llu peak=%llu dependency_drains=%llu capacity_drains=%llu submit_ms=%.3f pending=%u\n",
        (unsigned)g.readback_queue,(unsigned long long)g.queued_readbacks,(unsigned long long)g.pending_peak,
        (unsigned long long)g.pending_dependencies,(unsigned long long)g.pending_capacity,g.pending_submit_ms,g.pending_count);
    g.queued_readbacks=g.pending_peak=g.pending_dependencies=g.pending_capacity=0;g.pending_submit_ms=0;
    fprintf(stderr,"[GPU-EMPTY-STATS] enabled=%u skipped_readbacks=%llu initialized_targets=%llu\n",
        (unsigned)g.skip_empty_stats,(unsigned long long)g.empty_stats_skips,(unsigned long long)g.stats_initializations);
    g.empty_stats_skips=g.stats_initializations=0;
    size_t input_bytes=g.triangles.bytes+g.state.bytes+g.references.bytes+g.tiles.bytes;
    size_t shadow_bytes=g.triangles.upload_capacity+g.state.upload_capacity+g.references.upload_capacity+g.tiles.upload_capacity;
    for(unsigned i=0;i<NV_GPU_UPLOAD_SLOTS-1;i++) {
        GpuUploadSlot *slot=&g.upload_spare[i];
        input_bytes+=slot->triangles.bytes+slot->state.bytes+slot->references.bytes+slot->tiles.bytes;
        shadow_bytes+=slot->triangles.upload_capacity+slot->state.upload_capacity+slot->references.upload_capacity+slot->tiles.upload_capacity;
    }
    fprintf(stderr,"[GPU-UPLOAD] stream=%u slots=%u cache=%u writes=%llu reuses=%llu geometry_reuses=%llu bytes=%llu compared_bytes=%llu rotations=%llu gpu_capacity=%llu host_capacity=%llu\n",
        (unsigned)g.upload_stream,g.upload_stream?NV_GPU_UPLOAD_SLOTS:1u,(unsigned)g.upload_cache,
        (unsigned long long)g.upload_writes,(unsigned long long)g.upload_reuses,(unsigned long long)g.geometry_reuses,(unsigned long long)g.upload_bytes,
        (unsigned long long)g.upload_compared,(unsigned long long)g.upload_rotations,(unsigned long long)input_bytes,(unsigned long long)shadow_bytes);
    g.upload_writes=g.upload_reuses=g.upload_bytes=g.upload_compared=g.upload_rotations=g.geometry_reuses=0;
    fprintf(stderr,"[GPU-STATE-INTERN] enabled=%u hits=%llu misses=%llu state_bytes_avoided=%llu exact_verification=1\n",
        (unsigned)g.state_intern_enabled,(unsigned long long)g.state_intern_hits,
        (unsigned long long)g.state_intern_misses,(unsigned long long)g.state_intern_bytes_saved);
    g.state_intern_hits=g.state_intern_misses=g.state_intern_bytes_saved=0;
    fprintf(stderr,"[GPU-COUNTERS] diagnostic=%s guest_visibility=enabled\n",g.native && !g.native_diagnostics?"disabled":"enabled");
    fprintf(stderr,"[GPU-EARLY-DEPTH] enabled=%u shader_order=%s\n",(unsigned)(g.native && g.early_depth),g.native && g.early_depth?"live-depth-before-source":"source-before-ROV");
    fprintf(stderr,"[GPU-COVERAGE] mode=%s upload_batches=%llu shared_uploads=%u\n",g.native?(g.coverage?"conservative-triangles":"rectangles"):"compute-tiles",(unsigned long long)g.queue_uploads,(unsigned)(g.native && g.native_batch));g.queue_uploads=0;
    for(unsigned i=0;i<20;i++)if(g.sync_reason[i].count){fprintf(stderr,"[GPU-SYNC] reason=%s readbacks=%llu wait_copy_ms=%.3f\n",g.sync_reason[i].name,(unsigned long long)g.sync_reason[i].count,g.sync_reason[i].ms);g.sync_reason[i].count=0;g.sync_reason[i].ms=0;}
    fprintf(stderr,"[GPU-CLEAR] commands=%llu submit_ms=%.3f enabled=%u\n",(unsigned long long)g.clear_commands,g.clear_ms,(unsigned)(g.clear_shader!=NULL));
    fprintf(stderr,"[GPU-CLEAR-SKIP] inactive=%llu unavailable=%llu color_target=%llu layout=%llu depth_target=%llu bounds=%llu\n",
        (unsigned long long)g.clear_skips[0],(unsigned long long)g.clear_skips[1],(unsigned long long)g.clear_skips[2],
        (unsigned long long)g.clear_skips[3],(unsigned long long)g.clear_skips[4],(unsigned long long)g.clear_skips[5]);
    memset(g.clear_skips,0,sizeof g.clear_skips);
    size_t bytes=g.color.bytes+g.depth.bytes+g.surface.bytes;
    for(unsigned i=0;i<3;i++)bytes+=g.target_spare[i].color.bytes+g.target_spare[i].depth.bytes+g.target_spare[i].surface.bytes;
    fprintf(stderr,"[GPU-TARGET] binds=%llu allocations=%llu pool_hits=%llu allocation_ms=%.3f pooled_bytes=%llu uav_only=%u\n",
        (unsigned long long)g.target_binds,(unsigned long long)g.target_creates,(unsigned long long)g.target_pool_hits,g.target_create_ms,(unsigned long long)bytes*2,(unsigned)g.uav_only);
    g.target_binds=g.target_creates=g.target_pool_hits=0;g.target_create_ms=0;
    g.clear_commands=0;g.clear_ms=0;
    fprintf(stderr,"[GPU-TEXTURE] hits=%llu updates=%llu decode_ms=%.3f atlas_ms=%.3f hot=%u hot_hits=%llu index_hits=%llu full_scans=%llu\n",
        (unsigned long long)g.texture_hits,(unsigned long long)g.texture_updates,g.decode_ms,g.atlas_ms,
        (unsigned)g.texture_hot_enabled,(unsigned long long)g.texture_hot_hits,
        (unsigned long long)g.texture_index_hits,(unsigned long long)g.texture_full_scans);
    fprintf(stderr,"[GPU-TEXTURE-BULK] enabled=%u images=%llu texels=%llu scalar_images=%llu\n",
        (unsigned)g.bulk_texture,(unsigned long long)g.bulk_images,(unsigned long long)g.bulk_texels,(unsigned long long)g.scalar_images);
    g.bulk_images=g.bulk_texels=g.scalar_images=0;
    g.texture_hits=g.texture_updates=g.texture_hot_hits=g.texture_index_hits=g.texture_full_scans=0;g.decode_ms=g.atlas_ms=0;
    g.color_uploads=g.depth_uploads=g.color_reuses=g.depth_reuses=g.report_resets=0;
    g.draws=g.dispatches=g.simple_dispatches=g.simple_tris=g.tris=g.groups=g.uploads=g.readbacks=g.fallbacks=0;g.submit_ms=g.readback_ms=g.texture_ms=g.target_ms=g.map_ms=g.copy_ms=0;
}
#else
int nv_gpu_available(void){return 0;}
int nv_gpu_geometry_available(void){return 0;}
int nv_gpu_draw_raw(const NvGpuState*s,const NvRawInput*i,uint8_t*c,float*d,unsigned stride,const NvGpuTextureSource*t)
{(void)s;(void)i;(void)c;(void)d;(void)stride;(void)t;return 0;}
int nv_gpu_draw(const NvGpuState *s,const NvGpuTri *t,unsigned n,uint8_t *c,float *d,unsigned stride,const NvGpuTextureSource sources[4])
{(void)s;(void)t;(void)n;(void)c;(void)d;(void)stride;(void)sources;return 0;}
int nv_gpu_reserve_raw(const NvGpuState *s,unsigned n,int use_rc,uint8_t *c,float *d,unsigned stride,const NvGpuTextureSource sources[4])
{(void)s;(void)n;(void)use_rc;(void)c;(void)d;(void)stride;(void)sources;return 0;}
int nv_gpu_submit(void){return 1;}
int nv_gpu_has_pending_work(void){return 0;}
int nv_gpu_guest_access(uint64_t a,uint64_t n){(void)a;(void)n;return 1;}
int nv_gpu_guest_read(uint64_t a,uint64_t n){(void)a;(void)n;return 1;}
int nv_gpu_sync(NvGpuCount *count){memset(count,0,sizeof *count);return 1;}
int nv_gpu_sync_named(NvGpuCount *count,const char *reason){(void)reason;return nv_gpu_sync(count);}
int nv_gpu_sync_completion(NvGpuCount *count){(void)count;return 0;}
int nv_gpu_sync_report(NvGpuCount *count){(void)count;return 0;}
int nv_gpu_reset_report(void){return 0;}
int nv_gpu_clear_color(const NvGpuState*s,uint8_t*c,unsigned l,unsigned t,unsigned r,unsigned b,uint32_t v)
{(void)s;(void)c;(void)l;(void)t;(void)r;(void)b;(void)v;return 0;}
int nv_gpu_clear_depth(float*d,unsigned stride,float v){(void)d;(void)stride;(void)v;return 0;}
int nv_gpu_cpu_write_disjoint(const void*a,size_t bytes){(void)a;(void)bytes;return 0;}
void nv_gpu_report(void){}
void nv_gpu_capture_request(void){}
void nv_gpu_capture_present(const uint32_t*p,unsigned w,unsigned h){(void)p;(void)w;(void)h;}
#endif
