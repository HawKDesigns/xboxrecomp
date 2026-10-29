/* Private renderer state; included after GpuBuffer is defined. */
#define NV_RAW_MESHES 32768u
#define NV_RAW_HOST_LIMIT (64u*1024u*1024u)
#define NV_RAW_HASH 65536u
#define NV_RAW_ATLAS_LIMIT (64u*1024u*1024u)
#define NV_RAW_META 17u
#define NV_RAW_DRAWS 512u
#define NV_RAW_VERTICES 32768u
#define NV_RAW_UPLOAD_SLOTS 4u
typedef struct {GpuBuffer draws,jobs[2];} NvRawUploadSlot;
typedef struct {
    uint8_t *bytes;
    uint32_t *original,*indices;
    unsigned byte_count,count,vertices,triangles,primitive,inputs,pinned;
    size_t lengths[16];const uint8_t *addresses[16];
    NvRawAttr attrs[16];
    float lo[4],hi[4];
    uint64_t last_frame,first_frame;unsigned cache_hot;
    unsigned resident,atlas_begin,atlas_bytes,hash_next,lru_prev,lru_next;uint64_t key;
} NvRawMesh;
typedef struct {
    uint32_t rng;
    uint64_t calls[3],samples[3];
    double ms[3],peak_ms[3];
} NvRawProfile;
static struct {
    int ready,testing;
    ID3D11ComputeShader *transform,*prepare;
    GpuBuffer programs,draw_buffer,jobs,vertices,atlas,metadata;
    NvRawUploadSlot input[NV_RAW_UPLOAD_SLOTS];
    unsigned input_cursor;int input_stream;
    int upload_coalesce;
    struct {uint64_t batches,runs,writes,cpu_bytes,padding_bytes;double ms;} mixed_upload;
    uint64_t input_batches,input_rotations,input_rewrites,input_failures;
    double input_ms;
    uint64_t reserved_triangles,reserved_draws;
    struct {unsigned first,bytes;} free_ranges[NV_RAW_MESHES+2];unsigned free_count;
    unsigned free_slots[NV_RAW_MESHES],free_slot_count,lru_head,lru_tail,cache_initialized,entries,resident_entries;
    unsigned buckets[NV_RAW_HASH];int ref_atlas;
    unsigned cache_limit,cache_segmented,cache_balance,lru_hot_head,lru_hot_tail,hot_entries;
    size_t hot_host_bytes,hot_atlas_bytes;
    uint64_t cache_age_demotions,cache_byte_demotions;
    uint64_t cache_frame,cache_promotions,cache_demotions,cache_cold_hits,cache_hot_hits,cache_hot_evictions;
    uint64_t cache_slot_evictions,cache_host_evictions,cache_atlas_evictions;
    unsigned stream_union,stream_union_rejected;
    unsigned word_hash;
    unsigned profile_enabled;NvRawProfile profile[3];
    int point_bounds_enabled;
    uint64_t point_attempted,point_inside,point_outside,point_large,point_unsafe;
    struct {uint64_t lookups,overlapped,logical,unique,copy_legacy,copied,upload_legacy,uploaded;} stream_bytes;
    size_t resident_bytes;
    uint32_t *words;unsigned offsets[256],program_count;
    NvRawMesh mesh[NV_RAW_MESHES];
    NvRawDraw draws[NV_RAW_DRAWS];
    unsigned meshes[NV_RAW_DRAWS],starts[NV_RAW_DRAWS],count,vertex_count;
    uint8_t slots[NV_GPU_MAX_TRIANGLES];
    NvRawJob *job_upload;
    const uint32_t *ref_words;const uint8_t *ref_bytes;const NvRawAttr *ref_attrs;const NvRawDraw *ref_draws;
    size_t host_bytes;
    uint64_t tick,hits,misses,uploads,uploaded_bytes,draws_queued,vertices_queued,triangles_queued,bounds_rejected,boundary_fallbacks,dispatches,batches,batch_peak,evictions;
} raw;
static int gpu_raw_initialize(void);
static int gpu_raw_upload(GpuBuffer*);
static int gpu_raw_dispatch(GpuBuffer*,GpuBuffer*);
static void gpu_raw_reset(void);
static void gpu_raw_report(void);
