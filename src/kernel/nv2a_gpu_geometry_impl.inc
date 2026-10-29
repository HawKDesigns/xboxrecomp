/* Raw geometry stays on GPU between transform/preparation and rasterization.
 * Only private startup comparisons map geometry. Runtime guest fences retain
 * the ordinary full pixel/depth publication path. */
static uint32_t nv_raw_data(uint32_t at){uint32_t v;memcpy(&v,raw.ref_bytes+(size_t)at*4,4);return v;}
static NvRawAttr nv_raw_attr(uint32_t draw,uint32_t at){return raw.ref_attrs[(raw.ref_atlas?raw.ref_draws[draw].p.reserved:0)+at];}
static float nv_raw_asfloat(uint32_t v){float f;memcpy(&f,&v,4);return f;}
static float nv_raw_constant(uint32_t draw,int c,int k){return raw.ref_draws[draw].constants[c][k];}
static NvRawParams nv_raw_params(uint32_t draw){return raw.ref_draws[draw].p;}
static uint32_t nv_raw_program(uint32_t at){return raw.ref_words[at];}
#include "nv2a_gpu_vsh.inc"
#include "nv2a_gpu_prepare.inc"
static int raw_fast_reserve=-1;
static int gpu_raw_immutable(GpuBuffer *out,const void *data,unsigned bytes,unsigned stride)
{
    D3D11_BUFFER_DESC d={0};D3D11_SUBRESOURCE_DATA init={0};
    d.ByteWidth=bytes;d.Usage=D3D11_USAGE_IMMUTABLE;d.BindFlags=D3D11_BIND_SHADER_RESOURCE;
    d.MiscFlags=D3D11_RESOURCE_MISC_BUFFER_STRUCTURED;d.StructureByteStride=stride;init.pSysMem=data;
    GpuBuffer b={0};b.bytes=bytes;b.stride=stride;
    if(FAILED(ID3D11Device_CreateBuffer(g.device,&d,&init,&b.buffer)))return 0;
    D3D11_SHADER_RESOURCE_VIEW_DESC view={0};view.Format=DXGI_FORMAT_UNKNOWN;view.ViewDimension=D3D11_SRV_DIMENSION_BUFFER;view.Buffer.NumElements=bytes/stride;
    if(FAILED(ID3D11Device_CreateShaderResourceView(g.device,(ID3D11Resource*)b.buffer,&view,&b.srv))){gpu_buffer_release(&b);return 0;}
    gpu_buffer_release(out);*out=b;return 1;
}
/* Resident ranges are immutable until their last queued consumer submits.
 * Reuse is ordered by the single D3D immediate context; no CPU maps live VRAM. */
static void gpu_raw_range_free(unsigned first,unsigned bytes)
{
    unsigned at=0;while(at<raw.free_count && raw.free_ranges[at].first<first)++at;
    memmove(raw.free_ranges+at+1,raw.free_ranges+at,(raw.free_count-at)*sizeof raw.free_ranges[0]);
    raw.free_ranges[at].first=first;raw.free_ranges[at].bytes=bytes;++raw.free_count;
    if(at && raw.free_ranges[at-1].first+raw.free_ranges[at-1].bytes==first){
        raw.free_ranges[at-1].bytes+=bytes;memmove(raw.free_ranges+at,raw.free_ranges+at+1,(raw.free_count-at-1)*sizeof raw.free_ranges[0]);--raw.free_count;--at;
    }
    if(at+1<raw.free_count && raw.free_ranges[at].first+raw.free_ranges[at].bytes==raw.free_ranges[at+1].first){
        raw.free_ranges[at].bytes+=raw.free_ranges[at+1].bytes;memmove(raw.free_ranges+at+1,raw.free_ranges+at+2,(raw.free_count-at-2)*sizeof raw.free_ranges[0]);--raw.free_count;
    }
}
static int gpu_raw_range_alloc(unsigned bytes,unsigned *first)
{
    for(unsigned i=0;i<raw.free_count;i++)if(raw.free_ranges[i].bytes>=bytes){
        *first=raw.free_ranges[i].first;raw.free_ranges[i].first+=bytes;raw.free_ranges[i].bytes-=bytes;
        if(!raw.free_ranges[i].bytes){memmove(raw.free_ranges+i,raw.free_ranges+i+1,(raw.free_count-i-1)*sizeof raw.free_ranges[0]);--raw.free_count;}return 1;
    }
    return 0;
}
/* O(1) recency maintenance; probationary content cannot displace reused
 * resident data until its own eligible queue has been exhausted. */
static void gpu_raw_cache_configure(void)
{
    if(raw.cache_limit)return;
    raw.cache_limit=NV_RAW_MESHES;
    const char *slots=getenv("RECOMP_GPU_MESH_CACHE_SLOTS");
    if(slots && *slots){char *end=NULL;unsigned long n=strtoul(slots,&end,10);
        if(end && !*end && n>=64 && n<=NV_RAW_MESHES)raw.cache_limit=(unsigned)n;}
    const char *segmented=getenv("RECOMP_GPU_MESH_CACHE_2Q");
    raw.cache_segmented=!segmented || atoi(segmented)>0;
    const char *balance=getenv("RECOMP_GPU_MESH_CACHE_BALANCE");
    raw.cache_balance=raw.cache_segmented && (!balance || atoi(balance)>0);
    const char *word_hash=getenv("RECOMP_GPU_MESH_WORD_HASH");raw.word_hash=!word_hash || atoi(word_hash)>0;
    const char *profile=getenv("RECOMP_GPU_GEOMETRY_PROFILE");raw.profile_enabled=!profile || atoi(profile)>0;
    const char *points=getenv("RECOMP_GPU_POINT_BOUNDS");raw.point_bounds_enabled=!points || atoi(points)>0;
    const char *combine=getenv("RECOMP_GPU_STREAM_UNION");raw.stream_union=!combine || atoi(combine)>0;
}
static void gpu_raw_cache_initialize(void)
{
    if(raw.cache_initialized)return;gpu_raw_cache_configure();raw.cache_initialized=1;raw.free_slot_count=raw.cache_limit;
    for(unsigned i=0;i<raw.cache_limit;i++)raw.free_slots[i]=raw.cache_limit-1-i;
}
static void gpu_raw_lru_remove(NvRawMesh *m)
{
    unsigned *head=m->cache_hot?&raw.lru_hot_head:&raw.lru_head;
    unsigned *tail=m->cache_hot?&raw.lru_hot_tail:&raw.lru_tail;
    if(m->lru_prev)raw.mesh[m->lru_prev-1].lru_next=m->lru_next;else *head=m->lru_next;
    if(m->lru_next)raw.mesh[m->lru_next-1].lru_prev=m->lru_prev;else *tail=m->lru_prev;
    m->lru_prev=m->lru_next=0;
}
static void gpu_raw_lru_append(NvRawMesh *m)
{
    unsigned *head=m->cache_hot?&raw.lru_hot_head:&raw.lru_head;
    unsigned *tail=m->cache_hot?&raw.lru_hot_tail:&raw.lru_tail;
    unsigned link=(unsigned)(m-raw.mesh)+1;m->lru_prev=*tail;m->lru_next=0;
    if(*tail)raw.mesh[*tail-1].lru_next=link;else *head=link;*tail=link;
}
static size_t gpu_raw_mesh_host_cost(const NvRawMesh *m)
{
    return m->byte_count+(size_t)m->count*4+(size_t)m->triangles*12;
}
static void gpu_raw_cache_demote(NvRawMesh *m,int aged,int byte_budget)
{
    gpu_raw_lru_remove(m);m->cache_hot=0;--raw.hot_entries;
    raw.hot_host_bytes-=gpu_raw_mesh_host_cost(m);raw.hot_atlas_bytes-=m->atlas_bytes;
    if(raw.cache_balance){
        /* Expired/over-budget data is eligible before the current working
         * set. Demotion changes policy only, never residency or pinning. */
        unsigned link=(unsigned)(m-raw.mesh)+1;m->lru_prev=0;m->lru_next=raw.lru_head;
        if(raw.lru_head)raw.mesh[raw.lru_head-1].lru_prev=link;else raw.lru_tail=link;
        raw.lru_head=link;
    }else gpu_raw_lru_append(m); /* Original V99 policy for comparison. */
    if(!g.selftest){++raw.cache_demotions;if(aged)++raw.cache_age_demotions;if(byte_budget)++raw.cache_byte_demotions;}
}
static void gpu_raw_cache_age(void)
{
    if(!raw.cache_balance)return;
    /* Protected recency is ordered by exact cache touches. No entry scan is
     * needed; remove only an expired prefix, at most once per admission. */
    while(raw.lru_hot_head){
        NvRawMesh *old=&raw.mesh[raw.lru_hot_head-1];
        if(raw.cache_frame-old->last_frame<64u)break;
        gpu_raw_cache_demote(old,1,0);
    }
}
static void gpu_raw_cache_touch(NvRawMesh *m)
{
    gpu_raw_lru_remove(m);
    /* A resident must have been seen in a distinct frame; first GPU use of
     * a previously CPU-only candidate cannot promote via same-frame hits. */
    uint64_t seen=raw.cache_balance?m->last_frame:m->first_frame;
    if(raw.cache_segmented && !m->cache_hot && m->resident && seen!=raw.cache_frame){
        m->cache_hot=1;++raw.hot_entries;raw.hot_host_bytes+=gpu_raw_mesh_host_cost(m);raw.hot_atlas_bytes+=m->atlas_bytes;
        if(!g.selftest)++raw.cache_promotions;
    }
    m->last_frame=raw.cache_frame;gpu_raw_lru_append(m);
    /* Count limits alone allowed old protected data to occupy almost every
     * payload byte. Reserve one quarter for probation, independent of size. */
    while(raw.hot_entries>raw.cache_limit*3u/4u ||
          (raw.cache_balance && raw.hot_host_bytes>NV_RAW_HOST_LIMIT*3u/4u)){
        int bytes=raw.cache_balance && raw.hot_host_bytes>NV_RAW_HOST_LIMIT*3u/4u;
        gpu_raw_cache_demote(&raw.mesh[raw.lru_hot_head-1],0,bytes);
    }
    if(!g.selftest){if(m->cache_hot)++raw.cache_hot_hits;else ++raw.cache_cold_hits;}
}
static NvRawMesh *gpu_raw_victim(int resident,const NvRawMesh *avoid)
{
    for(unsigned queue=0;queue<2;queue++)
        for(unsigned link=queue?raw.lru_hot_head:raw.lru_head;link;link=raw.mesh[link-1].lru_next){
            NvRawMesh *m=&raw.mesh[link-1];if(!m->pinned && m!=avoid && (!resident || m->resident))return m;
        }
    return NULL;
}
static void gpu_raw_mesh_release(NvRawMesh *m)
{
    if(m->bytes){
        gpu_raw_lru_remove(m);raw.free_slots[raw.free_slot_count++]=(unsigned)(m-raw.mesh);--raw.entries;
        if(m->cache_hot){--raw.hot_entries;raw.hot_host_bytes-=gpu_raw_mesh_host_cost(m);raw.hot_atlas_bytes-=m->atlas_bytes;if(!g.selftest)++raw.cache_hot_evictions;}
        unsigned *link=&raw.buckets[m->key&(NV_RAW_HASH-1)];
        while(*link && &raw.mesh[*link-1]!=m)link=&raw.mesh[*link-1].hash_next;
        if(*link)*link=m->hash_next;
        raw.host_bytes-=m->byte_count+(size_t)m->count*4+(size_t)m->triangles*12;
        if(!g.selftest)++raw.evictions;
    }
    if(m->resident){gpu_raw_range_free(m->atlas_begin,m->atlas_bytes);raw.resident_bytes-=m->atlas_bytes;--raw.resident_entries;}
    free(m->bytes);free(m->original);free(m->indices);memset(m,0,sizeof *m);
}
#include "nv2a_gpu_stream_union.inc"
#include "nv2a_gpu_mesh_word_hash.inc"
static uint64_t gpu_raw_hash_bytes(uint64_t h,const void *data,size_t bytes)
{
    if(raw.word_hash)return gpu_raw_word_hash(h,data,bytes);
    const uint8_t *p=data;for(size_t i=0;i<bytes;i++)h=(h^p[i])*1099511628211ull;return h;
}
static uint64_t gpu_raw_key(const NvRawInput *in,const NvRawStreamPlan *plan)
{
    uint32_t meta[4]={in->count,in->vertices,in->primitive,in->inputs};
    uint64_t h=gpu_raw_hash_bytes(14695981039346656037ull,meta,sizeof meta);
    h=gpu_raw_hash_bytes(h,in->indices,(size_t)in->count*4);
    for(unsigned i=0;i<16;i++){
        const NvRawStream *v=&in->stream[i];uint64_t info[4]={v->bytes,v->stride,v->type,v->size};
        h=gpu_raw_hash_bytes(h,info,sizeof info);
        if(!raw.stream_union)h=gpu_raw_hash_bytes(h,v->data,v->bytes);
    }
    if(raw.stream_union){
        h=gpu_raw_hash_bytes(h,plan->offset,sizeof plan->offset);
        for(unsigned i=0;i<plan->count;i++){
            h=gpu_raw_hash_bytes(h,&plan->spans[i].bytes,sizeof plan->spans[i].bytes);
            h=gpu_raw_hash_bytes(h,(const void*)plan->spans[i].first,plan->spans[i].bytes);
        }
    }
    return h;
}
static int gpu_raw_mesh_equal(const NvRawMesh *m,const NvRawInput *in)
{
    if(!m->bytes || m->count!=in->count || m->vertices!=in->vertices || m->primitive!=in->primitive || m->inputs!=in->inputs ||
       memcmp(m->original,in->indices,(size_t)in->count*4))return 0;
    for(unsigned j=0;j<16;j++){
        const NvRawStream *s=&in->stream[j];const NvRawAttr *a=&m->attrs[j];
        if(m->lengths[j]!=s->bytes || a->stride!=s->stride || a->type!=s->type || a->size!=s->size)return 0;
        if(s->bytes && memcmp(m->bytes+a->offset,s->data,s->bytes))return 0;
    }
    return 1;
}
static unsigned gpu_raw_triangle_count(unsigned prim,unsigned n)
{
    if(prim==5)return n/3;if(prim==6 || prim==7 || prim==10)return n>2?n-2:0;
    if(prim==8)return (n/4)*2;if(prim==9)return n>3?((n-2)/2)*2:0;return 0;
}
static NvRawMesh *gpu_raw_mesh(const NvRawInput *in)
{
    gpu_raw_cache_initialize();gpu_raw_cache_age();unsigned slot;
    NvRawStreamPlan plan;if(!gpu_raw_stream_plan(in,&plan,raw.stream_union))return NULL;
    if(!g.selftest){++raw.stream_bytes.lookups;raw.stream_bytes.overlapped+=plan.unique<plan.logical;
        raw.stream_bytes.logical+=plan.logical;raw.stream_bytes.unique+=plan.unique;}
    uint64_t key=gpu_raw_key(in,&plan);
    /* Hashes choose candidates only. Exact layout/index/full-byte equality
     * remains required, including when a guest address is reused. */
    for(unsigned link=raw.buckets[key&(NV_RAW_HASH-1)];link;link=raw.mesh[link-1].hash_next){
        NvRawMesh *m=&raw.mesh[link-1];
        if(m->key==key && gpu_raw_mesh_equal(m,in)){++raw.tick;gpu_raw_cache_touch(m);if(!g.selftest)++raw.hits;return m;}
    }
    unsigned tris=gpu_raw_triangle_count(in->primitive,in->count);if(!tris)return NULL;
    size_t bytes=plan.bytes;
    size_t need=bytes+(size_t)in->count*4+(size_t)tris*12;if(need>NV_RAW_HOST_LIMIT || bytes>UINT32_MAX)return NULL;
    while(raw.host_bytes+need>NV_RAW_HOST_LIMIT || !raw.free_slot_count){
        NvRawMesh *victim=gpu_raw_victim(0,NULL);
        if(victim){if(!g.selftest){if(!raw.free_slot_count)++raw.cache_slot_evictions;else ++raw.cache_host_evictions;}gpu_raw_mesh_release(victim);}
        else if(!gpu_flush()){g.readback_failed=1;return NULL;}
    }
    slot=raw.free_slots[raw.free_slot_count-1];
    NvRawMesh next={0};next.bytes=calloc(1,bytes);next.original=malloc((size_t)in->count*4);next.indices=malloc((size_t)tris*12);
    if(!next.bytes || !next.original || !next.indices){free(next.bytes);free(next.original);free(next.indices);return NULL;}
    next.byte_count=(unsigned)bytes;next.count=in->count;next.vertices=in->vertices;next.triangles=tris;next.primitive=in->primitive;next.inputs=in->inputs;
    size_t legacy_bytes=0;
    for(unsigned j=0;j<16;j++){
        const NvRawStream *s=&in->stream[j];next.attrs[j]=(NvRawAttr){plan.offset[j],s->stride,s->type,s->size};
        next.lengths[j]=s->bytes;next.addresses[j]=s->data;legacy_bytes+=(s->bytes+11u)&~(size_t)3u;
    }
    for(unsigned j=0;j<plan.count;j++)memcpy(next.bytes+plan.spans[j].offset,(const void*)plan.spans[j].first,plan.spans[j].bytes);
    if(!g.selftest){raw.stream_bytes.copy_legacy+=legacy_bytes;raw.stream_bytes.copied+=bytes;}
    memcpy(next.original,in->indices,(size_t)in->count*4);
    unsigned out=0;
    for(unsigned t=0;t<tris;t++){
        unsigned a,b,c;
        if(in->primitive==5){a=t*3;b=a+1;c=a+2;}
        else if(in->primitive==6){a=t+(t&1);b=t+1-(t&1);c=t+2;}
        else if(in->primitive==7 || in->primitive==10){a=0;b=t+1;c=t+2;}
        else if(in->primitive==8){a=(t/2)*4;b=a+((t&1)?2:1);c=a+((t&1)?3:2);}
        else {a=(t/2)*2;b=a+((t&1)?3:1);c=a+((t&1)?2:3);}
        next.indices[out++]=in->indices[a];next.indices[out++]=in->indices[b];next.indices[out++]=in->indices[c];
    }
    raw.ref_bytes=next.bytes;raw.ref_attrs=next.attrs;
    for(unsigned v=0;v<in->vertices;v++){
        NvRawVec p=nv_raw_attribute(0,v,0);
        for(unsigned k=0;k<4;k++){
            if(!isfinite(p.v[k])){free(next.bytes);free(next.original);free(next.indices);return NULL;}
            if(!v || p.v[k]<next.lo[k])next.lo[k]=p.v[k];if(!v || p.v[k]>next.hi[k])next.hi[k]=p.v[k];
        }
    }
    --raw.free_slot_count;++raw.entries;raw.mesh[slot]=next;raw.host_bytes+=need;++raw.tick;raw.mesh[slot].last_frame=raw.mesh[slot].first_frame=raw.cache_frame;gpu_raw_lru_append(&raw.mesh[slot]);
    raw.mesh[slot].key=key;raw.mesh[slot].hash_next=raw.buckets[key&(NV_RAW_HASH-1)];raw.buckets[key&(NV_RAW_HASH-1)]=slot+1;
    if(!g.selftest)++raw.misses;return &raw.mesh[slot];
}
/* Interval arithmetic covers all positions in the cached object box before
 * running any vertex program. Outward rounding and a full-pixel guard retain
 * uncertain/near-plane/boundary draws on the exact CPU clipper. */
typedef struct {double lo,hi;} RawInterval;
static RawInterval raw_interval(double a,double b){RawInterval r={a,b};return r;}
static RawInterval raw_interval_mul(RawInterval a,RawInterval b)
{
    double p[4]={a.lo*b.lo,a.lo*b.hi,a.hi*b.lo,a.hi*b.hi},lo=p[0],hi=p[0];
    for(int i=1;i<4;i++){if(p[i]<lo)lo=p[i];if(p[i]>hi)hi=p[i];}
    return raw_interval(nextafterf((float)lo,-INFINITY),nextafterf((float)hi,INFINITY));
}
static RawInterval raw_interval_add(RawInterval a,RawInterval b)
{return raw_interval(nextafterf((float)(a.lo+b.lo),-INFINITY),nextafterf((float)(a.hi+b.hi),INFINITY));}
static int gpu_raw_box_bounds(const NvRawMesh *m,const NvRawInput *in)
{
    RawInterval clip[4];
    for(int row=0;row<4;row++){
        if(in->position_kind==2)clip[row]=raw_interval(m->lo[row],m->hi[row]);
        else{
            clip[row]=raw_interval_mul(raw_interval(m->lo[0],m->hi[0]),raw_interval(in->constants[96+row][0],in->constants[96+row][0]));
            for(int j=1;j<4;j++)clip[row]=raw_interval_add(clip[row],raw_interval_mul(raw_interval(m->lo[j],m->hi[j]),raw_interval(in->constants[96+row][j],in->constants[96+row][j])));
        }
        if(!isfinite(clip[row].lo) || !isfinite(clip[row].hi))return 0;
    }
    if(clip[3].hi<0.001)return -1;
    if(clip[3].lo<=0.01 || clip[3].hi>1e12)return 0;
    RawInterval reciprocal=raw_interval(1.0/clip[3].hi,1.0/clip[3].lo);
    /* Reject reciprocal-clamp/denormal/cancellation regimes, which retain
     * the original exact interpreter/clipper rather than guessing. */
    if(reciprocal.lo<5.42101e-20 || reciprocal.hi>1.884467e19)return 0;
    for(int j=0;j<4;j++){reciprocal.lo=nextafterf((float)reciprocal.lo,-INFINITY);reciprocal.hi=nextafterf((float)reciprocal.hi,INFINITY);}
    RawInterval screen[3];
    for(int j=0;j<3;j++){
        float scale=in->constants[58][j],offset=in->constants[59][j];if(!isfinite(scale) || !isfinite(offset))return 0;
        screen[j]=raw_interval_add(raw_interval_mul(raw_interval_mul(clip[j],raw_interval(scale,scale)),reciprocal),raw_interval(offset,offset));
        if(!isfinite(screen[j].lo) || !isfinite(screen[j].hi) || fabs(screen[j].lo)>1e8 || fabs(screen[j].hi)>1e8)return 0;
    }
    if(screen[0].hi<(double)in->params.left-1 || screen[0].lo>(double)in->params.right+1 ||
       screen[1].hi<(double)in->params.top-1 || screen[1].lo>(double)in->params.bottom+1)return -1;
    if(in->zclip && (screen[2].hi<(double)in->zmin-1 || screen[2].lo>(double)in->zmax+1))return -1;
    if(screen[0].lo<(double)in->params.left+1 || screen[0].hi>(double)in->params.right-1 ||
       screen[1].lo<(double)in->params.top+1 || screen[1].hi>(double)in->params.bottom-1)return 0;
    if(in->zclip && (screen[2].lo<(double)in->zmin+1e-3 || screen[2].hi>(double)in->zmax-1e-3))return 0;
    return 1;
}
/* An object AABB loses correlation between coordinate extrema. Per-vertex
 * proof uses a wide float roundoff envelope around double dot products. The
 * 1e-5 relative envelope exceeds the worst first-order float error of four
 * products, three additions, reciprocal, scale and offset by over an order
 * of magnitude. Cancellation uses the sum of ABSOLUTE products, never the
 * small final dot. This avoids dozens of nextafterf calls per vertex while
 * retaining a strictly inward accept / outward reject margin. */
static int gpu_raw_point_bounds(const NvRawMesh *m,const NvRawInput *in)
{
    if(!raw.point_bounds_enabled || !m->vertices || !m->bytes || m->vertices>256u){
        if(!g.selftest && raw.point_bounds_enabled && m->vertices>256u)++raw.point_large;
        return 0;
    }
    if(!g.selftest)++raw.point_attempted;
    raw.ref_bytes=m->bytes;raw.ref_attrs=m->attrs;
    int inside=1,all_left=1,all_right=1,all_top=1,all_bottom=1;
    int all_near=in->zclip,all_far=in->zclip;
    for(unsigned v=0;v<m->vertices;v++){
        NvRawVec p=nv_raw_attribute(0,v,0);double lo[4],hi[4];
        for(int row=0;row<4;row++){
            if(in->position_kind==2)lo[row]=hi[row]=p.v[row];
            else {
                double sum=0,absolute=0;
                for(int j=0;j<4;j++){
                    double term=(double)p.v[j]*in->constants[96+row][j];
                    sum+=term;absolute+=fabs(term);
                }
                double error=absolute*1e-5+1e-30;
                lo[row]=sum-error;hi[row]=sum+error;
            }
            if(!isfinite(lo[row]) || !isfinite(hi[row]) ||
               fabs(lo[row])>1e30 || fabs(hi[row])>1e30)goto uncertain;
        }
        if(lo[3]<=0.01 || hi[3]>1e12)goto uncertain;
        double reciprocal_lo=1.0/hi[3],reciprocal_hi=1.0/lo[3];
        if(reciprocal_lo<5.42101e-20 || reciprocal_hi>1.884467e19)goto uncertain;
        for(int j=0;j<3;j++){
            float scale=in->constants[58][j],offset=in->constants[59][j];
            if(!isfinite(scale) || !isfinite(offset))goto uncertain;
            double a=lo[j]*(double)scale*reciprocal_lo,b=lo[j]*(double)scale*reciprocal_hi;
            double c=hi[j]*(double)scale*reciprocal_lo,d=hi[j]*(double)scale*reciprocal_hi;
            double minimum=fmin(fmin(a,b),fmin(c,d)),maximum=fmax(fmax(a,b),fmax(c,d));
            double error=1e-5*(fabs(offset)+fmax(fabs(minimum),fabs(maximum))+1.0);
            double screen_lo=minimum+offset-error,screen_hi=maximum+offset+error;
            if(!isfinite(screen_lo) || !isfinite(screen_hi) ||
               fabs(screen_lo)>1e8 || fabs(screen_hi)>1e8)goto uncertain;
            if(j==0){
                inside&=screen_lo>=(double)in->params.left+1 && screen_hi<=(double)in->params.right-1;
                all_left&=screen_hi<(double)in->params.left-1;
                all_right&=screen_lo>(double)in->params.right+1;
            }else if(j==1){
                inside&=screen_lo>=(double)in->params.top+1 && screen_hi<=(double)in->params.bottom-1;
                all_top&=screen_hi<(double)in->params.top-1;
                all_bottom&=screen_lo>(double)in->params.bottom+1;
            }else if(in->zclip){
                inside&=screen_lo>=(double)in->zmin+1e-3 && screen_hi<=(double)in->zmax-1e-3;
                all_near&=screen_hi<(double)in->zmin-1;
                all_far&=screen_lo>(double)in->zmax+1;
            }
        }
    }
    if(all_left || all_right || all_top || all_bottom || all_near || all_far){
        if(!g.selftest)++raw.point_outside;
        return -1;
    }
    if(inside){if(!g.selftest)++raw.point_inside;return 1;}
uncertain:
    if(!g.selftest)++raw.point_unsafe;
    return 0;
}
static int gpu_raw_bounds(const NvRawMesh *m,const NvRawInput *in)
{
    int boxed=gpu_raw_box_bounds(m,in);
    return boxed?boxed:gpu_raw_point_bounds(m,in);
}
static int gpu_raw_resident(NvRawMesh *m)
{
    if(m->resident)return 1;
    unsigned bytes=m->byte_count+m->triangles*12,first;
    if(bytes>NV_RAW_ATLAS_LIMIT)return 0;
    while(!gpu_raw_range_alloc(bytes,&first)){
        NvRawMesh *victim=gpu_raw_victim(1,m);
        if(victim){if(!g.selftest)++raw.cache_atlas_evictions;gpu_raw_mesh_release(victim);}
        else {if(!raw.count)return 0;if(!gpu_flush()){g.readback_failed=1;return 0;}}
    }
    NvRawAttr meta[NV_RAW_META];memcpy(meta,m->attrs,sizeof m->attrs);
    for(unsigned i=0;i<16;i++)meta[i].offset+=first;
    memset(meta+16,0,sizeof meta[16]);meta[16].offset=(first+m->byte_count)/4;
    D3D11_BOX data={first,0,0,first+m->byte_count,1,1},indices={first+m->byte_count,0,0,first+bytes,1,1};
    unsigned slot=(unsigned)(m-raw.mesh);D3D11_BOX attrs={slot*sizeof meta,0,0,(slot+1)*sizeof meta,1,1};
    ID3D11DeviceContext_UpdateSubresource(g.context,(ID3D11Resource*)raw.atlas.buffer,0,&data,m->bytes,0,0);
    ID3D11DeviceContext_UpdateSubresource(g.context,(ID3D11Resource*)raw.atlas.buffer,0,&indices,m->indices,0,0);
    ID3D11DeviceContext_UpdateSubresource(g.context,(ID3D11Resource*)raw.metadata.buffer,0,&attrs,meta,0,0);
    m->resident=1;m->atlas_begin=first;m->atlas_bytes=bytes;raw.resident_bytes+=bytes;++raw.resident_entries;
    if(!g.selftest){
        ++raw.uploads;raw.uploaded_bytes+=bytes+sizeof meta;
        size_t legacy=0;for(unsigned j=0;j<16;j++)legacy+=(m->lengths[j]+11u)&~(size_t)3u;
        raw.stream_bytes.upload_legacy+=legacy+m->triangles*12+sizeof meta;
        raw.stream_bytes.uploaded+=bytes+sizeof meta;
    }
    return 1;
}
/* Sampling wrappers preserve the original operations and return values. */
#include "nv2a_gpu_geometry_profile.inc"
int nv_gpu_geometry_available(void)
{
    return (InterlockedCompareExchange(&s_startup,0,0)==2 || gpu_startup_owned()) && raw.ready && g.native && g.coverage && g.native_batch &&
        (!g.selftest || raw.testing) && !g.auto_enabled && !s_capture && !InterlockedCompareExchange(&s_capture_requested,0,0);
}
int nv_gpu_draw_raw(const NvGpuState *state,const NvRawInput *in,uint8_t *color,float *depth,unsigned stride,const NvGpuTextureSource texture[4])
{
    if(!nv_gpu_geometry_available() || !in->count || in->count>4096 || !in->vertices || in->vertices>4096 ||
       in->program_id>=raw.program_count || (in->position_kind!=1 && in->position_kind!=2))return 0;
    for(unsigned i=0;i<in->count;i++)if(in->indices[i]>=in->vertices)return 0;
    for(unsigned i=0;i<16;i++){
        const NvRawStream *v=&in->stream[i];
        if(!v->size && !v->bytes)continue;
        if(!v->data || !v->stride || !v->size || v->size>4 ||
           (v->type!=0 && v->type!=1 && v->type!=2 && v->type!=4 && v->type!=5 && v->type!=6))return 0;
        unsigned element=(v->type==0 || v->type==6)?4:v->size*(v->type==2?4:(v->type==1 || v->type==5?2:1));
        if((uint64_t)(in->vertices-1)*v->stride+element>v->bytes)return 0;
    }
    NvRawMesh *m=gpu_raw_profile_mesh(in);if(!m)return g.readback_failed?-1:0;
    int bounds=gpu_raw_profile_bounds(m,in);
    if(bounds<0 && !in->bounds_reject_enabled)return 0;
    if(bounds<0){if(!g.selftest)++raw.bounds_rejected;return 2;}
    if(!bounds){if(!g.selftest)++raw.boundary_fallbacks;return 0;}
    if(!gpu_raw_profile_resident(m))return g.readback_failed?-1:0;
    if(raw.count==NV_RAW_DRAWS || raw.vertex_count+m->vertices>NV_RAW_VERTICES)
        if(!gpu_flush())return -1;
    if(raw_fast_reserve<0){const char *e=getenv("RECOMP_GPU_RAW_FAST_RESERVE");raw_fast_reserve=!e || atoi(e)>0;}
    int accepted;
    if(raw_fast_reserve)accepted=nv_gpu_reserve_raw(state,m->triangles,in->params.use_rc,color,depth,stride,texture);
    else {
        static NvGpuTri placeholders[NV_GPU_MAX_TRIANGLES];
        for(unsigned i=0;i<m->triangles;i++){
            memset(&placeholders[i],0,sizeof placeholders[i]);placeholders[i].maxx=placeholders[i].maxy=1;
            placeholders[i].use_rc=in->params.use_rc;placeholders[i].textured=in->params.textured;
        }
        accepted=nv_gpu_draw(state,placeholders,m->triangles,color,depth,stride,texture);
    }
    if(accepted!=1)return accepted;
    if(raw_fast_reserve && !g.selftest){++raw.reserved_draws;raw.reserved_triangles+=m->triangles;}
    unsigned at=raw.count++,base=g.queued_count-m->triangles;
    NvRawDraw *draw=&raw.draws[at];memcpy(draw->constants,in->constants,sizeof draw->constants);draw->p=in->params;
    draw->p.program=raw.offsets[in->program_id];unsigned instructions;nv2a_vsh_known_program(in->program_id,&instructions);draw->p.count=instructions;
    draw->p.state=g.queued[base].state_index;draw->p.vertex_base=raw.vertex_count;draw->p.reserved=(unsigned)(m-raw.mesh)*NV_RAW_META;
    raw.vertex_count+=m->vertices;raw.meshes[at]=(unsigned)(m-raw.mesh);raw.starts[at]=base;m->pinned=1;
    for(unsigned i=0;i<m->triangles;i++)raw.slots[base+i]=1;
    if(in->params.left<g.dirty_left)g.dirty_left=in->params.left;if(in->params.right>g.dirty_right)g.dirty_right=in->params.right;
    if(in->params.top<g.dirty_top)g.dirty_top=in->params.top;if(in->params.bottom>g.dirty_bottom)g.dirty_bottom=in->params.bottom;
    if(!g.selftest){++raw.draws_queued;raw.vertices_queued+=m->vertices;raw.triangles_queued+=m->triangles;}
    return 1;
}
static int gpu_raw_upload(GpuBuffer *triangles)
{
    unsigned bytes=g.queued_count*sizeof(NvGpuTri);
    if(!triangles->uav || triangles->bytes<bytes){
        unsigned capacity=1;while(capacity<g.queued_count)capacity*=2;
        GpuBuffer next={0};if(!gpu_buffer_create(&next,capacity*sizeof(NvGpuTri),sizeof(NvGpuTri),2))return 0;
        gpu_buffer_release(triangles);*triangles=next;
    }
    triangles->upload_valid=0;
    double begin=gpu_ms();unsigned cpu=0,runs=0,writes=0,padding=0;
    for(unsigned i=0;i<g.queued_count;i++)if(!raw.slots[i]){++cpu;if(!i || raw.slots[i-1])++runs;}
    /* GPU preparation overwrites EVERY raw slot before rasterization. Thus
     * a CPU upload may bridge short raw-only gaps without altering output.
     * Limit each gap to 64 records and all extra writes to min(CPU,1024)
     * records per batch. Leading/trailing raw spans are never uploaded. */
    unsigned budget=raw.upload_coalesce?(cpu<1024u?cpu:1024u):0;
    for(unsigned i=0;i<g.queued_count;){
        if(raw.slots[i]){++i;continue;}
        unsigned first=i;while(i<g.queued_count && !raw.slots[i])++i;
        unsigned end=i;
        while(i<g.queued_count && budget){
            unsigned next=i;while(next<g.queued_count && raw.slots[next])++next;
            unsigned gap=next-end;
            if(next==g.queued_count || gap>64u || gap>budget)break;
            budget-=gap;padding+=gap;i=next;
            while(i<g.queued_count && !raw.slots[i])++i;
            end=i;
        }
        D3D11_BOX box={first*sizeof(NvGpuTri),0,0,end*sizeof(NvGpuTri),1,1};
        ID3D11DeviceContext_UpdateSubresource(g.context,(ID3D11Resource*)triangles->buffer,0,&box,g.queued+first,0,0);++writes;
        if(!g.selftest){++g.upload_writes;g.upload_bytes+=box.right-box.left;}
    }
    if(!g.selftest){
        ++raw.mixed_upload.batches;raw.mixed_upload.runs+=runs;raw.mixed_upload.writes+=writes;
        raw.mixed_upload.cpu_bytes+=(uint64_t)cpu*sizeof(NvGpuTri);
        raw.mixed_upload.padding_bytes+=(uint64_t)padding*sizeof(NvGpuTri);raw.mixed_upload.ms+=gpu_ms()-begin;
    }
    return 1;
}
static unsigned gpu_raw_jobs(unsigned stage,unsigned *work)
{
    unsigned jobs=0;
    for(unsigned d=0;d<raw.count;d++){
        NvRawMesh *m=&raw.mesh[raw.meshes[d]];unsigned count=stage?m->triangles:m->vertices;
        for(unsigned j=0;j<count;j++)raw.job_upload[jobs++]=(NvRawJob){d,j,stage?raw.starts[d]+j:raw.draws[d].p.vertex_base+j,0};
    }
    *work=jobs;unsigned padded=(jobs+63u)&~63u;
    for(unsigned j=jobs;j<padded;j++)raw.job_upload[j]=(NvRawJob){UINT32_MAX,0,0,0};
    return padded;
}
static int gpu_raw_dispatch(GpuBuffer *triangles,GpuBuffer *states)
{
    (void)states;if(!raw.count)return 1;
    double begin=gpu_ms();unsigned work[2]={0},padded[2]={0};
    NvRawUploadSlot *input=NULL;
    if(raw.input_stream){
        input=&raw.input[raw.input_cursor%NV_RAW_UPLOAD_SLOTS];
        int ok=gpu_upload(&input->draws,raw.draws,raw.count*sizeof(NvRawDraw),4);
        for(unsigned stage=0;stage<2 && ok;stage++){
            padded[stage]=gpu_raw_jobs(stage,&work[stage]);
            ok=gpu_upload(&input->jobs[stage],raw.job_upload,padded[stage]*sizeof(NvRawJob),sizeof(NvRawJob));
        }
        if(!ok){
            /* No compute command has used this input set. Rebuild the exact
             * legacy input path before dispatching any accepted geometry. */
            raw.input_stream=0;input=NULL;if(!g.selftest)++raw.input_failures;
            fprintf(stderr,"[GPU-RAW-UPLOAD] streaming allocation failed; legacy ordered uploads retained\n");
        }else {++raw.input_cursor;if(!g.selftest)++raw.input_rotations;}
    }
    if(!input && !gpu_upload(&raw.draw_buffer,raw.draws,raw.count*sizeof(NvRawDraw),4))return 0;
    if(!g.selftest)raw.input_ms+=gpu_ms()-begin;
    /* All meshes share resident atlas/metadata bindings. Dispatch once for
     * vertices and once for preparation, irrespective of unique mesh count.
     * Streaming prepares BOTH independent job buffers before either dispatch:
     * no CPU upload rewrites a just-submitted transform input between stages.
     * Ring wrap retains ordinary immediate-context UpdateSubresource ordering. */
    for(unsigned stage=0;stage<2;stage++){
        GpuBuffer *jobs=input?&input->jobs[stage]:&raw.jobs;
        if(!input){
            begin=gpu_ms();padded[stage]=gpu_raw_jobs(stage,&work[stage]);
            if(!gpu_upload(jobs,raw.job_upload,padded[stage]*sizeof(NvRawJob),sizeof(NvRawJob)))return 0;
            if(!g.selftest){raw.input_ms+=gpu_ms()-begin;if(stage)++raw.input_rewrites;}
        }
        ID3D11ShaderResourceView *srv[5]={stage?raw.vertices.srv:raw.atlas.srv,raw.metadata.srv,stage?raw.atlas.srv:raw.programs.srv,input?input->draws.srv:raw.draw_buffer.srv,jobs->srv};
        ID3D11UnorderedAccessView *uav[3]={stage?triangles->uav:raw.vertices.uav,NULL,NULL};
        ID3D11DeviceContext_CSSetShader(g.context,stage?raw.prepare:raw.transform,NULL,0);
        ID3D11DeviceContext_CSSetShaderResources(g.context,0,5,srv);ID3D11DeviceContext_CSSetUnorderedAccessViews(g.context,0,3,uav,NULL);
        gpu_timing_phase_begin(stage?NV_GPU_TIME_PREPARE:NV_GPU_TIME_VERTEX,work[stage]);
        ID3D11DeviceContext_Dispatch(g.context,padded[stage]/64,1,1);
        gpu_timing_phase_end(stage?NV_GPU_TIME_PREPARE:NV_GPU_TIME_VERTEX);
        memset(srv,0,sizeof srv);memset(uav,0,sizeof uav);
        ID3D11DeviceContext_CSSetShaderResources(g.context,0,5,srv);ID3D11DeviceContext_CSSetUnorderedAccessViews(g.context,0,3,uav,NULL);
        if(!g.selftest)++raw.dispatches;
    }
    if(!g.selftest){++raw.batches;++raw.input_batches;if(raw.count>raw.batch_peak)raw.batch_peak=raw.count;}
    return 1;
}
static void gpu_raw_reset(void)
{
    for(unsigned d=0;d<raw.count;d++)raw.mesh[raw.meshes[d]].pinned=0;
    raw.count=raw.vertex_count=0;memset(raw.slots,0,sizeof raw.slots);
}
static int gpu_raw_shader(unsigned transform,ID3D11ComputeShader **out)
{
    uint64_t key=gpu_compile_key(nv_gpu_geometry_source,sizeof nv_gpu_geometry_source-1,"main","cs_5_0",transform?1u:0u);
    char path[128];snprintf(path,sizeof path,"shader-cache/qpr2-geometry-%016llx.dxbc",(unsigned long long)key);
    if(gpu_shader_load(path,key,out))return 1;
    D3D_SHADER_MACRO macros[2]={{"NV_RAW_TRANSFORM",transform?"1":"0"},{NULL,NULL}};
    ID3DBlob *code=NULL,*errors=NULL;
    HRESULT hr=D3DCompile(nv_gpu_geometry_source,sizeof nv_gpu_geometry_source-1,"qpr2_gpu_geometry",macros,NULL,"main","cs_5_0",GPU_SHADER_FLAGS,0,&code,&errors);
    if(FAILED(hr)){fprintf(stderr,"[GPU-GEOMETRY] %s compile failed: %s\n",transform?"transform":"prepare",errors?(char*)ID3D10Blob_GetBufferPointer(errors):"no diagnostic");if(errors)ID3D10Blob_Release(errors);if(code)ID3D10Blob_Release(code);return 0;}
    if(errors)ID3D10Blob_Release(errors);
    int ok=SUCCEEDED(ID3D11Device_CreateComputeShader(g.device,ID3D10Blob_GetBufferPointer(code),ID3D10Blob_GetBufferSize(code),NULL,out));
    if(ok)gpu_shader_save(path,key,code);ID3D10Blob_Release(code);return ok;
}
static int gpu_raw_selftest(void);
static int gpu_raw_initialize(void)
{
    const char *e=getenv("RECOMP_GPU_GEOMETRY");if(!e || atoi(e)<=0 || !g.native || !g.coverage || !g.native_batch)return 0;
    const char *stream=getenv("RECOMP_GPU_RAW_UPLOAD_STREAM");raw.input_stream=!stream || atoi(stream)>0;
    const char *coalesce=getenv("RECOMP_GPU_MIXED_UPLOAD_COALESCE");raw.upload_coalesce=!coalesce || atoi(coalesce)>0;
    gpu_raw_cache_configure();
    raw.program_count=nv2a_vsh_known_count();if(!raw.program_count || raw.program_count>256)return 0;
    unsigned words=0;
    for(unsigned i=0;i<raw.program_count;i++){unsigned n;nv2a_vsh_known_program(i,&n);raw.offsets[i]=words;words+=n*4;}
    raw.words=malloc((size_t)words*4);raw.job_upload=malloc((NV_RAW_VERTICES+64)*sizeof(NvRawJob));if(!raw.words || !raw.job_upload)goto disable;
    for(unsigned i=0;i<raw.program_count;i++){unsigned n;const uint32_t (*p)[4]=nv2a_vsh_known_program(i,&n);memcpy(raw.words+raw.offsets[i],p,(size_t)n*16);}
    raw.ref_words=raw.words;
    if(sizeof(NvRawDraw)!=792u*4 || sizeof(NvRawParams)!=96u ||
       !gpu_raw_immutable(&raw.programs,raw.words,words*4,4) ||
       !gpu_buffer_create(&raw.atlas,NV_RAW_ATLAS_LIMIT,4,0) ||
       !gpu_buffer_create(&raw.metadata,raw.cache_limit*NV_RAW_META*sizeof(NvRawAttr),sizeof(NvRawAttr),0) ||
       !gpu_buffer_create(&raw.vertices,NV_RAW_VERTICES*sizeof(Nv2aVshOutput),sizeof(Nv2aVshOutput),2) ||
       !gpu_raw_shader(1,&raw.transform) || !gpu_raw_shader(0,&raw.prepare))goto disable;
    raw.free_count=1;raw.free_ranges[0].first=0;raw.free_ranges[0].bytes=NV_RAW_ATLAS_LIMIT;
    raw.ready=raw.testing=1;
    {int checked=gpu_raw_selftest();
        if(checked<0 || g.readback_failed){raw.testing=raw.ready=0;return -1;}
        if(!checked && raw.stream_union){
            /* Only private startup consumers exist here. Revert the storage
             * representation as one policy before exposing geometry to guests. */
            gpu_raw_reset();for(unsigned i=0;i<NV_RAW_MESHES;i++)gpu_raw_mesh_release(raw.mesh+i);
            g.active=g.resident=0;g.queued_count=g.state_count=g.queued_refs=0;g.pending_count=0;
            for(unsigned i=0;i<128;i++)g.cache[i].queued=0;
            g.host_color=NULL;g.host_depth=NULL;memset(&g.completed,0,sizeof g.completed);
            raw.stream_union=0;raw.stream_union_rejected=1;
            fprintf(stderr,"[GPU-STREAM-UNION] startup comparison failed; retrying separate immutable streams\n");
            checked=gpu_raw_selftest();
        }
        raw.testing=0;if(checked<0 || g.readback_failed){raw.ready=0;return -1;}if(!checked)goto disable;
    }
    fprintf(stderr,"[GPU-GEOMETRY] raw vertex/lighting and triangle preparation offscreen comparisons passed; programs=%u resident_mesh_slots=%u atlas_mib=%u cross_mesh_batching=1\n",raw.program_count,raw.cache_limit,NV_RAW_ATLAS_LIMIT/(1024u*1024u));return 1;
disable:
    raw.ready=raw.testing=0;gpu_raw_reset();
    fprintf(stderr,"[GPU-GEOMETRY] unavailable or comparison failed; CPU vertex/clip/preparation path retained\n");return 0;
}
static void gpu_raw_report(void)
{
    size_t gpu_bytes=raw.draw_buffer.bytes+raw.jobs.bytes,host_bytes=raw.draw_buffer.upload_capacity+raw.jobs.upload_capacity;
    for(unsigned i=0;i<NV_RAW_UPLOAD_SLOTS;i++){
        NvRawUploadSlot *s=&raw.input[i];gpu_bytes+=s->draws.bytes;host_bytes+=s->draws.upload_capacity;
        for(unsigned j=0;j<2;j++){gpu_bytes+=s->jobs[j].bytes;host_bytes+=s->jobs[j].upload_capacity;}
    }
    fprintf(stderr,"[GPU-MIXED-UPLOAD] coalesce=%u batches=%llu original_runs=%llu writes=%llu avoided_calls=%llu cpu_bytes=%llu padding_bytes=%llu upload_ms=%.3f max_gap_records=64 max_padding_records=1024\n",
        (unsigned)raw.upload_coalesce,(unsigned long long)raw.mixed_upload.batches,(unsigned long long)raw.mixed_upload.runs,
        (unsigned long long)raw.mixed_upload.writes,(unsigned long long)(raw.mixed_upload.runs-raw.mixed_upload.writes),
        (unsigned long long)raw.mixed_upload.cpu_bytes,(unsigned long long)raw.mixed_upload.padding_bytes,raw.mixed_upload.ms);
    memset(&raw.mixed_upload,0,sizeof raw.mixed_upload);
    fprintf(stderr,"[GPU-RAW-UPLOAD] stream=%u slots=%u batches=%llu rotations=%llu single_job_rewrites=%llu allocation_failures=%llu input_ms=%.3f gpu_capacity=%llu host_capacity=%llu\n",
        (unsigned)raw.input_stream,raw.input_stream?NV_RAW_UPLOAD_SLOTS:1u,(unsigned long long)raw.input_batches,
        (unsigned long long)raw.input_rotations,(unsigned long long)raw.input_rewrites,(unsigned long long)raw.input_failures,
        raw.input_ms,(unsigned long long)gpu_bytes,(unsigned long long)host_bytes);
    raw.input_batches=raw.input_rotations=raw.input_rewrites=raw.input_failures=0;raw.input_ms=0;
    fprintf(stderr,"[GPU-RAW-RESERVE] enabled=%u draws=%llu triangles=%llu placeholder_bytes_avoided=%llu\n",
        (unsigned)(raw_fast_reserve>0),(unsigned long long)raw.reserved_draws,
        (unsigned long long)raw.reserved_triangles,
        (unsigned long long)(raw.reserved_triangles*sizeof(NvGpuTri)*2u));
    raw.reserved_triangles=raw.reserved_draws=0;
    fprintf(stderr,"[GPU-MESH-HASH] word_hash=%u exact_byte_verification=1\n",raw.word_hash);
    gpu_raw_profile_report();
    fprintf(stderr,"[GPU-GEOMETRY] enabled=%u draws=%llu vertices=%llu candidate_triangles=%llu dispatches=%llu mesh_hits=%llu mesh_misses=%llu mesh_uploads=%llu mesh_upload_bytes=%llu bounds_rejected=%llu boundary_fallbacks=%llu host_bytes=%llu batches=%llu batch_draw_peak=%llu mesh_evictions=%llu resident_bytes=%llu mesh_slots=%u cached_meshes=%u resident_meshes=%u\n",
        (unsigned)raw.ready,(unsigned long long)raw.draws_queued,(unsigned long long)raw.vertices_queued,(unsigned long long)raw.triangles_queued,(unsigned long long)raw.dispatches,
        (unsigned long long)raw.hits,(unsigned long long)raw.misses,(unsigned long long)raw.uploads,(unsigned long long)raw.uploaded_bytes,
        (unsigned long long)raw.bounds_rejected,(unsigned long long)raw.boundary_fallbacks,(unsigned long long)raw.host_bytes,(unsigned long long)raw.batches,(unsigned long long)raw.batch_peak,(unsigned long long)raw.evictions,(unsigned long long)raw.resident_bytes,raw.cache_limit,raw.entries,raw.resident_entries);
    fprintf(stderr,"[GPU-POINT-BOUNDS] enabled=%u attempted=%llu inside=%llu outside=%llu large=%llu uncertain=%llu max_vertices=256\n",
        (unsigned)raw.point_bounds_enabled,(unsigned long long)raw.point_attempted,(unsigned long long)raw.point_inside,
        (unsigned long long)raw.point_outside,(unsigned long long)raw.point_large,(unsigned long long)raw.point_unsafe);
    raw.point_attempted=raw.point_inside=raw.point_outside=raw.point_large=raw.point_unsafe=0;
    fprintf(stderr,"[GPU-STREAM-UNION] enabled=%u rejected=%u lookups=%llu overlapped_lookups=%llu logical_stream_bytes=%llu hashed_stream_bytes=%llu copy_legacy_bytes=%llu copied_bytes=%llu upload_legacy_bytes=%llu uploaded_bytes=%llu\n",
        (unsigned)(raw.ready && raw.stream_union),raw.stream_union_rejected,(unsigned long long)raw.stream_bytes.lookups,(unsigned long long)raw.stream_bytes.overlapped,
        (unsigned long long)raw.stream_bytes.logical,(unsigned long long)raw.stream_bytes.unique,
        (unsigned long long)raw.stream_bytes.copy_legacy,(unsigned long long)raw.stream_bytes.copied,
        (unsigned long long)raw.stream_bytes.upload_legacy,(unsigned long long)raw.stream_bytes.uploaded);
    memset(&raw.stream_bytes,0,sizeof raw.stream_bytes);
    fprintf(stderr,"[GPU-MESH-CACHE] segmented=%u limit=%u probation=%u protected=%u promotions=%llu demotions=%llu probation_hits=%llu protected_hits=%llu protected_evictions=%llu slot_evictions=%llu host_evictions=%llu atlas_evictions=%llu host_limit_mib=%u atlas_limit_mib=%u metadata_gpu_bytes=%u metadata_cpu_bytes=%llu\n",
        raw.cache_segmented,raw.cache_limit,raw.entries-raw.hot_entries,raw.hot_entries,
        (unsigned long long)raw.cache_promotions,(unsigned long long)raw.cache_demotions,
        (unsigned long long)raw.cache_cold_hits,(unsigned long long)raw.cache_hot_hits,
        (unsigned long long)raw.cache_hot_evictions,(unsigned long long)raw.cache_slot_evictions,
        (unsigned long long)raw.cache_host_evictions,(unsigned long long)raw.cache_atlas_evictions,
        NV_RAW_HOST_LIMIT/(1024u*1024u),NV_RAW_ATLAS_LIMIT/(1024u*1024u),raw.metadata.bytes,(unsigned long long)(sizeof raw.mesh+sizeof raw.buckets+sizeof raw.free_slots+sizeof raw.free_ranges));
    fprintf(stderr,"[GPU-MESH-BALANCE] enabled=%u protected_host_bytes=%llu protected_gpu_bytes=%llu probation_host_bytes=%llu probation_gpu_bytes=%llu protected_budget_bytes=%u max_idle_frames=%u age_demotions=%llu byte_demotions=%llu oldest_protected_frames=%llu\n",
        raw.cache_balance,(unsigned long long)raw.hot_host_bytes,(unsigned long long)raw.hot_atlas_bytes,
        (unsigned long long)(raw.host_bytes-raw.hot_host_bytes),(unsigned long long)(raw.resident_bytes-raw.hot_atlas_bytes),
        raw.cache_balance?NV_RAW_HOST_LIMIT*3u/4u:NV_RAW_HOST_LIMIT,raw.cache_balance?64u:0u,
        (unsigned long long)raw.cache_age_demotions,(unsigned long long)raw.cache_byte_demotions,
        (unsigned long long)(raw.lru_hot_head?raw.cache_frame-raw.mesh[raw.lru_hot_head-1].last_frame:0));
    raw.cache_age_demotions=raw.cache_byte_demotions=0;
    raw.cache_promotions=raw.cache_demotions=raw.cache_cold_hits=raw.cache_hot_hits=raw.cache_hot_evictions=0;
    raw.cache_slot_evictions=raw.cache_host_evictions=raw.cache_atlas_evictions=0;
    raw.batches=raw.batch_peak=raw.evictions=0;
    raw.draws_queued=raw.vertices_queued=raw.triangles_queued=raw.dispatches=raw.hits=raw.misses=raw.uploads=raw.uploaded_bytes=raw.bounds_rejected=raw.boundary_fallbacks=0;
}
/* A pure reference consumes private constants/raw bytes. It never uploads to
 * or executes the guest's global interpreter on the background startup thread. */
static int raw_close(float a,float b)
{
    if(a==b || (isnan(a)&&isnan(b)))return 1;
    return isfinite(a)&&isfinite(b)&&fabsf(a-b)<=0.0002f*fmaxf(1.0f,fabsf(a));
}
static int gpu_raw_selftest(void)
{
    float data[3][16][4],constants[192][4];uint32_t indices[3]={0,1,2};
    for(unsigned v=0;v<3;v++)for(unsigned a=0;a<16;a++)for(unsigned k=0;k<4;k++)data[v][a][k]=0.2f+(float)(a+k+v)*0.025f;
    const float positions[3][4]={{2,2,10,1},{27,3,10,1},{4,27,10,1}};
    for(unsigned v=0;v<3;v++)memcpy(data[v][0],positions[v],16);
    for(unsigned c=0;c<192;c++)for(unsigned k=0;k<4;k++)constants[c][k]=0.1f+(float)((c+k)%7)*0.01f;
    for(unsigned row=0;row<4;row++)for(unsigned k=0;k<4;k++)constants[96+row][k]=row==k?1.0f:0;
    const float scale[4]={1,1,1,1},offset[4]={0},fog[4]={0,1,0,1},c93[4]={1,2,3,4},c94[4]={2,.25f,.5f,0},c95[4]={1,1,8,.5f};
    memcpy(constants[58],scale,16);memcpy(constants[59],offset,16);memcpy(constants[186],fog,16);
    memcpy(constants[189],c93,16);memcpy(constants[190],c94,16);memcpy(constants[191],c95,16);
    NvRawInput input={0};input.indices=indices;input.count=input.vertices=3;input.primitive=5;input.inputs=0xffff;input.constants=constants;input.position_kind=1;
    input.params.right=input.params.bottom=32;input.params.su=input.params.sv=1;
    for(unsigned a=0;a<16;a++)input.stream[a]=(NvRawStream){(const uint8_t*)data+16*a,sizeof data[0]*2+16,sizeof data[0],2,4};
    NvRawMesh *mesh=gpu_raw_mesh(&input);if(!mesh || !gpu_raw_mesh_equal(mesh,&input) || !gpu_raw_resident(mesh))return 0;
    GpuBuffer triangles={0},states={0};ID3D11Buffer *stage_v=NULL,*stage_t=NULL;unsigned cap_v=0,cap_t=0;
    D3D11_MAPPED_SUBRESOURCE mapped_v={0},mapped_t={0};int ok=0;
    if(!gpu_buffer_create(&triangles,256*sizeof(NvGpuTri),sizeof(NvGpuTri),2))goto done;
    NvGpuState dummy={0};if(!gpu_upload(&states,&dummy,sizeof dummy,sizeof dummy))goto done;
    raw.count=raw.program_count;raw.vertex_count=raw.count*3;mesh->pinned=1;
    for(unsigned d=0;d<raw.count;d++){
        NvRawDraw *out=&raw.draws[d];memcpy(out->constants,constants,sizeof constants);out->p=input.params;
        out->p.program=raw.offsets[d];unsigned count;nv2a_vsh_known_program(d,&count);out->p.count=count;out->p.vertex_base=d*3;out->p.reserved=(unsigned)(mesh-raw.mesh)*NV_RAW_META;
        raw.meshes[d]=(unsigned)(mesh-raw.mesh);raw.starts[d]=d;
    }
    if(!gpu_raw_dispatch(&triangles,&states) ||
       !gpu_staging_buffer(&stage_v,&cap_v,raw.vertices.bytes) || !gpu_staging_buffer(&stage_t,&cap_t,triangles.bytes))goto done;
    ID3D11DeviceContext_CopyResource(g.context,(ID3D11Resource*)stage_v,(ID3D11Resource*)raw.vertices.buffer);
    ID3D11DeviceContext_CopyResource(g.context,(ID3D11Resource*)stage_t,(ID3D11Resource*)triangles.buffer);
    if(FAILED(ID3D11DeviceContext_Map(g.context,(ID3D11Resource*)stage_v,0,D3D11_MAP_READ,0,&mapped_v)) ||
       FAILED(ID3D11DeviceContext_Map(g.context,(ID3D11Resource*)stage_t,0,D3D11_MAP_READ,0,&mapped_t))){g.readback_failed=1;ok=-1;goto done;}
    raw.ref_bytes=mesh->bytes;raw.ref_attrs=mesh->attrs;raw.ref_draws=raw.draws;raw.ref_words=raw.words;
    for(unsigned d=0;d<raw.count;d++){
        Nv2aVshOutput reference[3];
        for(unsigned v=0;v<3;v++){
            reference[v]=nv_raw_vertex(d,v);
            const float *a=(const float*)&reference[v],*b=(const float*)((const Nv2aVshOutput*)mapped_v.pData+d*3+v);
            for(unsigned k=0;k<sizeof(Nv2aVshOutput)/4;k++)if(!raw_close(a[k],b[k])){
                fprintf(stderr,"[GPU-GEOMETRY] vertex comparison failed program=%u vertex=%u component=%u cpu=%.9g gpu=%.9g\n",d,v,k,a[k],b[k]);goto done;
            }
        }
        NvGpuTri reference_tri=nv_raw_prepare(raw.draws[d].p,reference[0],reference[1],reference[2]);
        const NvGpuTri *actual=(const NvGpuTri*)mapped_t.pData+d;
        if(reference_tri.minx!=actual->minx || reference_tri.maxx!=actual->maxx || reference_tri.miny!=actual->miny || reference_tri.maxy!=actual->maxy ||
           memcmp(reference_tri.inclusive,actual->inclusive,sizeof actual->inclusive))goto done;
        const float *a=(const float*)&reference_tri,*b=(const float*)actual;
        for(unsigned k=0;k<sizeof(NvGpuTri)/4;k++)if(!raw_close(a[k],b[k]))goto done;
    }
    ok=1;
done:
    if(mapped_v.pData)ID3D11DeviceContext_Unmap(g.context,(ID3D11Resource*)stage_v,0);
    if(mapped_t.pData)ID3D11DeviceContext_Unmap(g.context,(ID3D11Resource*)stage_t,0);
    if(stage_v)ID3D11Buffer_Release(stage_v);if(stage_t)ID3D11Buffer_Release(stage_t);
    gpu_buffer_release(&triangles);gpu_buffer_release(&states);gpu_raw_reset();
    if(ok!=1)return ok;
    /* Then pass generated geometry through the real ordered pixel backend. */
    uint32_t actual[1024],expected[1024];float depth[1024],expected_depth[1024];
    for(unsigned i=0;i<1024;i++){actual[i]=expected[i]=0xFF102030u;depth[i]=expected_depth[i]=100;}
    NvGpuState state={0};state.width=state.height=32;state.pitch=128;state.bpp=4;state.format=0x128;
    state.color_mask=0x01010101;state.depth_available=state.depth_test=state.depth_mask=state.zpass_enable=1;state.depth_func=0x203;
    state.blend_enable=1;state.blend_sfactor=0x302;state.blend_dfactor=0x303;state.blend_equation=0x8006;
    NvGpuTextureSource textures[4]={0};NvGpuCount reference_count={0},count={0};
    g.active=g.resident=0;memset(&g.completed,0,sizeof g.completed);
    input.program_id=0;
    for(unsigned draw=0;draw<4;draw++){
        constants[101][0]=0.2f+(float)draw*0.15f;constants[101][1]=0.6f;constants[101][2]=0.4f;constants[101][3]=0.75f;
        NvRawDraw cpu={0};memcpy(cpu.constants,constants,sizeof constants);cpu.p=input.params;cpu.p.program=raw.offsets[0];
        unsigned instructions;nv2a_vsh_known_program(0,&instructions);cpu.p.count=instructions;
        data[1][0][0]=positions[1][0]-(float)draw;
        NvRawMesh *cpu_mesh=gpu_raw_mesh(&input);if(!cpu_mesh){ok=0;goto pixel_done;}
        raw.ref_draws=&cpu;raw.ref_bytes=cpu_mesh->bytes;raw.ref_attrs=cpu_mesh->attrs;
        NvGpuTri prepared=nv_raw_prepare(cpu.p,nv_raw_vertex(0,0),nv_raw_vertex(0,1),nv_raw_vertex(0,2));
        /* Alternate distinct CPU/raw/CPU contributions to exercise padding
         * across raw slots through the actual upload + preparation path. */
        NvGpuTri cpu_tri=prepared;
        for(unsigned v=0;v<3;v++){cpu_tri.v[v].d0[0]=0.8f;cpu_tri.v[v].d0[1]=0.1f;cpu_tri.v[v].d0[2]=0.3f;cpu_tri.v[v].d0[3]=0.5f;}
        for(unsigned step=0;step<3;step++){
            NvGpuTri reference=step==1?prepared:cpu_tri;
            for(unsigned y=0;y<32;y++)for(unsigned x=0;x<32;x++){
                unsigned at=y*32+x;NvGpuPixel p=gpu_pixel(state,reference,(int)x,(int)y,expected[at],expected_depth[at]);
                expected[at]=p.color;expected_depth[at]=p.depth;reference_count.depth_fail+=p.depth_fail;reference_count.pixels+=p.pixels;reference_count.zpass+=p.zpass;
                if((p.pixel_max&0xFFFFFFu)>(reference_count.pixel_max&0xFFFFFFu))reference_count.pixel_max=p.pixel_max;
            }
            int accepted=step==1?nv_gpu_draw_raw(&state,&input,(uint8_t*)actual,depth,32,textures):
                nv_gpu_draw(&state,&cpu_tri,1,(uint8_t*)actual,depth,32,textures);
            if(accepted!=1){ok=0;goto pixel_done;}
        }
    }
    if(nv_gpu_sync(&count)<0){ok=-1;goto pixel_done;}
    gpu_reference_counts(&reference_count);
    if(count.zpass!=reference_count.zpass || count.pixels!=reference_count.pixels || count.depth_fail!=reference_count.depth_fail)ok=0;
    for(unsigned i=0;i<1024;i++){
        if(depth[i]!=expected_depth[i])ok=0;
        for(unsigned shift=0;shift<32;shift+=8)if(abs((int)((actual[i]>>shift)&255)-(int)((expected[i]>>shift)&255))>1)ok=0;
    }
pixel_done:
    if(!g.readback_failed && nv_gpu_sync(&count)<0)ok=-1;
    if(ok==1 && raw.point_bounds_enabled){
        /* Correlated vertices are inside even though their independent x/y
         * extrema produce an uncertain box. A real boundary vertex must
         * still use the CPU clipper. */
        float positions[3][4]={{2,2,10,1},{27,26,10,1},{4,5,10,1}};
        float proof_constants[192][4]={{0}};
        proof_constants[96][0]=1;proof_constants[96][1]=-1;proof_constants[96][3]=16;
        proof_constants[97][1]=1;proof_constants[98][2]=1;proof_constants[99][3]=1;
        proof_constants[58][0]=proof_constants[58][1]=proof_constants[58][2]=1;
        NvRawInput proof={0};proof.indices=indices;proof.count=proof.vertices=3;
        proof.primitive=5;proof.position_kind=1;proof.inputs=1;proof.constants=proof_constants;
        proof.params.right=proof.params.bottom=32;
        proof.stream[0]=(NvRawStream){(const uint8_t*)positions,sizeof positions,sizeof positions[0],2,4};
        NvRawMesh *candidate=gpu_raw_mesh(&proof);
        if(!candidate || gpu_raw_box_bounds(candidate,&proof)!=0 || gpu_raw_bounds(candidate,&proof)!=1)ok=0;
        positions[2][0]=33;
        candidate=gpu_raw_mesh(&proof);
        if(!candidate || gpu_raw_box_bounds(candidate,&proof)!=0 || gpu_raw_bounds(candidate,&proof)!=0)ok=0;
        positions[2][0]=4;proof_constants[96][3]=-20;
        candidate=gpu_raw_mesh(&proof);
        if(!candidate || gpu_raw_box_bounds(candidate,&proof)!=0 || gpu_raw_bounds(candidate,&proof)!=-1)ok=0;
    }
    gpu_raw_reset();g.active=g.resident=0;g.host_color=NULL;g.host_depth=NULL;g.target_written=0;g.pending_count=0;
    memset(&g.completed,0,sizeof g.completed);
    return ok;
}
