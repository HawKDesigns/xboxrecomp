#ifndef NV2A_GPU_RASTER_H
#define NV2A_GPU_RASTER_H
#include <stddef.h>
#include "nv2a_gpu_types.h"
#include "nv2a_gpu_raw_types.h"
#define NV_GPU_TILE_SIZE 16
#define NV_GPU_MAX_TRIANGLES 8192
#define NV_GPU_MAX_STATES 1024
#define NV_GPU_MAX_TILES 4096
#define NV_GPU_MAX_REFERENCES (512u * NV_GPU_MAX_TILES)
typedef struct {
    uint64_t depth_fail, pixels;
    uint32_t zpass, pixel_max;
} NvGpuCount;
typedef struct {
    const uint8_t *raw, *palette;
    size_t bytes, palette_bytes;
    void *opaque;
    int (*decode)(void *, uint32_t, uint32_t, uint32_t, uint32_t *);
    /* Wrapper proof: 1=contiguous linear ARGB8888, 2=Morton ARGB8888.
     * Zero retains the arbitrary CPU decoder, including custom test decoders. */
    uint32_t gpu_argb_layout;
    /* Optional wrapper-owned whole-image conversion from the immutable raw
     * cache snapshot. Zero return leaves the legacy per-texel decoder active. */
    int (*decode_image)(void *, const uint8_t *, size_t, uint32_t *, size_t);
} NvGpuTextureSource;
/* Every tile has one owner; indices within a tile remain in draw order. */
int nv_gpu_bin(const NvGpuTri *tri, unsigned n, uint32_t width, uint32_t height,
               NvGpuTile *tiles, unsigned *tile_count, uint32_t *refs,
               unsigned capacity, unsigned *ref_count);
int nv_gpu_available(void);
/* 1: queued on hardware, 0: no submission (caller may use software).
 * CPU-prepared triangles; raw GPU geometry uses the separate entry point below. */
int nv_gpu_draw(const NvGpuState *state, const NvGpuTri *tri, unsigned n,
                uint8_t *color, float *depth, unsigned depth_stride,
                const NvGpuTextureSource texture[4]);
/* Reserve ordered raw triangle slots. GPU preparation overwrites every slot
 * before rasterization; only material/state indices are populated on CPU. */
int nv_gpu_reserve_raw(const NvGpuState *state, unsigned triangles, int use_rc,
                       uint8_t *color, float *depth, unsigned depth_stride,
                       const NvGpuTextureSource texture[4]);
typedef struct {const uint8_t *data;size_t bytes;uint32_t stride,type,size;} NvRawStream;
typedef struct {
    NvRawStream stream[16];
    const uint32_t *indices;
    unsigned count,vertices,primitive,inputs,program_id,position_kind;
    const float (*constants)[4];
    float zmin,zmax;unsigned zclip,bounds_reject_enabled;
    NvRawParams params;
} NvRawInput;
/* 1 queued, 2 rejected by conservative object bounds, 0 CPU fallback, -1
 * accepted work failed. Raw bytes/indices are snapshotted before return. */
int nv_gpu_geometry_available(void);
int nv_gpu_draw_raw(const NvGpuState*,const NvRawInput*,uint8_t*,float*,unsigned,const NvGpuTextureSource[4]);
/* Complete queued writes before CPU accesses or presentation. Guest
 * completion signals can use the token path when it is enabled. */
/* Submit immutable queued draws without mapping pixels or publishing fences.
 * Negative means accepted work could not be submitted. */
int nv_gpu_submit(void);
int nv_gpu_has_pending_work(void);
/* Complete queued target copies before any CPU-side command, vertex, texture
 * or framebuffer consumer touches overlapping guest memory. */
int nv_gpu_guest_access(uint64_t address,uint64_t bytes);
/* Actual CPU completion-chain read, rather than binding a GPU destination.
 * Includes the current active target as well as queued target snapshots. */
int nv_gpu_guest_read(uint64_t address,uint64_t bytes);
int nv_gpu_sync(NvGpuCount *count);
int nv_gpu_sync_named(NvGpuCount *count,const char *reason);
/* Complete GPU commands for a guest semaphore while retaining pixel mirrors
 * and visibility counters until a real CPU access or report. */
int nv_gpu_sync_completion(NvGpuCount *count);
/* Report-only completion keeps the current color/depth resident. Returns 0
 * when the caller must use the ordinary full completion path. */
int nv_gpu_sync_report(NvGpuCount *count);
/* Discard previous visibility counts in command order without consuming pixels.
 * Returns zero when unsupported so the caller may use the synchronous path. */
int nv_gpu_reset_report(void);
/* Ordered clears of an already active native target. Zero retains CPU clear;
 * negative means an accepted draw could not be submitted. CPU mirrors may be
 * cleared too, but rendered writes still complete at the usual sync boundary. */
int nv_gpu_clear_color(const NvGpuState *surface,uint8_t *color,
                       unsigned left,unsigned top,unsigned right,unsigned bottom,uint32_t value);
int nv_gpu_clear_depth(float *depth,unsigned stride,float value);
/* A CPU write needs no readback only when its complete byte range is disjoint
 * from BOTH active color and depth mirrors. Zero requires ordinary sync. */
int nv_gpu_cpu_write_disjoint(const void *address,size_t bytes);
void nv_gpu_report(void);
void nv_gpu_capture_request(void);
void nv_gpu_capture_present(const uint32_t *pixels,unsigned width,unsigned height);
#endif
