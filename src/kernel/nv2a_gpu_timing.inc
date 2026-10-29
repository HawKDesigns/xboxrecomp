/* Sparse GPU timeline samples. Optional diagnostics never wait, Map, Flush or
 * participate in guest completion. Legacy mode selects one scope per 8
 * presents; spread mode selects 1/64 eligible scopes across three classes. */
enum {NV_GPU_TIME_VERTEX,NV_GPU_TIME_PREPARE,NV_GPU_TIME_PIXELS,NV_GPU_TIME_COPY,NV_GPU_TIME_KINDS};
#ifndef NV_GPU_NO_TIMESTAMPS
static double gpu_ms(void);
#define NV_GPU_MATERIAL_KINDS 5u
#define NV_GPU_TIME_QUERY_COUNT (1+NV_GPU_TIME_KINDS*2+2+2)
#define NV_GPU_TIME_SLOTS 16u
#define NV_GPU_TIME_FRAMES 8u
#define NV_GPU_SPREAD_RATE 64u
#define NV_GPU_SPREAD_CLASSES 3u
typedef struct {uint64_t samples,work,min_work,max_work;double ms,max_ms;} GpuSpreadPhase;
typedef struct {
    ID3D11Query *query[NV_GPU_TIME_QUERY_COUNT];
    unsigned material_done,material_kind,material_runs,material_pick,material_at,shade_done;uint64_t material_work;
    unsigned closed,mask,category;uint64_t work[NV_GPU_TIME_KINDS];
} GpuTimeSlot;
static struct {
    int enabled,spread,materials,shade;unsigned active,want_copy;
    uint32_t material_random;
    struct {uint64_t draws,triangles,samples,sample_triangles,run_weights;double ms,max_ms,weighted_ms;} material[NV_GPU_MATERIAL_KINDS];
    struct {uint64_t matched,triangles;double full_ms,source_ms;} shade_result;
    uint32_t random[NV_GPU_SPREAD_CLASSES];
    struct {uint64_t batches,selected,dropped;} population[NV_GPU_SPREAD_CLASSES];
    GpuSpreadPhase by_class[NV_GPU_SPREAD_CLASSES][NV_GPU_TIME_KINDS];
    uint64_t frame,next_frame,dropped,invalid;
    GpuTimeSlot slot[NV_GPU_TIME_SLOTS];
    struct {uint64_t samples,work;double ms,max_ms;} phase[NV_GPU_TIME_KINDS];
} gpu_time;
static void gpu_timing_release(void)
{
    gpu_time.enabled=0;gpu_time.active=0;
    for(unsigned i=0;i<NV_GPU_TIME_SLOTS;i++){
        for(unsigned j=0;j<NV_GPU_TIME_QUERY_COUNT;j++)if(gpu_time.slot[i].query[j]){
            ID3D11Query_Release(gpu_time.slot[i].query[j]);gpu_time.slot[i].query[j]=NULL;
        }
        gpu_time.slot[i].closed=0;
    }
}
static void gpu_timing_initialize(void)
{
    gpu_timing_release();memset(&gpu_time,0,sizeof gpu_time);
    const char *e=getenv("RECOMP_GPU_PROFILE");if(!e || atoi(e)<=0)return;
    const char *spread=getenv("RECOMP_GPU_PROFILE_SPREAD");gpu_time.spread=spread && atoi(spread)>0;
    const char *materials=getenv("RECOMP_GPU_PROFILE_MATERIALS");gpu_time.materials=materials && atoi(materials)>0;
    const char *shade=getenv("RECOMP_GPU_PROFILE_SHADE");gpu_time.shade=gpu_time.materials && g.native && shade && atoi(shade)>0;
    gpu_time.material_random=0x105a7b31u;
    for(unsigned c=0;c<NV_GPU_SPREAD_CLASSES;c++)gpu_time.random[c]=0x91A37F25u^(0x9E3779B9u*(c+1));
    for(unsigned i=0;i<NV_GPU_TIME_SLOTS;i++)for(unsigned j=0;j<1+NV_GPU_TIME_KINDS*2+(gpu_time.materials?2u:0u)+(gpu_time.shade?2u:0u);j++){
        D3D11_QUERY_DESC desc={0};desc.Query=j?D3D11_QUERY_TIMESTAMP:D3D11_QUERY_TIMESTAMP_DISJOINT;
        if(FAILED(ID3D11Device_CreateQuery(g.device,&desc,&gpu_time.slot[i].query[j]))){
            gpu_timing_release();fprintf(stderr,"[GPU-PROFILE] timestamp allocation unavailable; rendering unchanged\n");return;
        }
    }
    gpu_time.enabled=1;
}
static void gpu_timing_poll(void)
{
    if(!gpu_time.enabled)return;
    for(unsigned i=0;i<NV_GPU_TIME_SLOTS;i++)if(gpu_time.slot[i].closed){
        GpuTimeSlot *s=&gpu_time.slot[i];D3D11_QUERY_DATA_TIMESTAMP_DISJOINT disjoint={0};
        HRESULT hr=ID3D11DeviceContext_GetData(g.context,(ID3D11Asynchronous*)s->query[0],&disjoint,sizeof disjoint,D3D11_ASYNC_GETDATA_DONOTFLUSH);
        if(FAILED(hr)){gpu_timing_release();fprintf(stderr,"[GPU-PROFILE] query failed; diagnostics disabled, rendering unchanged\n");return;}
        if(hr!=0)continue;
        if(disjoint.Disjoint || !disjoint.Frequency){++gpu_time.invalid;s->closed=0;continue;}
        uint64_t ticks[NV_GPU_TIME_KINDS][2]={{0}};int ready=1;
        for(unsigned k=0;k<NV_GPU_TIME_KINDS && ready;k++)if(s->mask&(1u<<k))for(unsigned j=0;j<2;j++){
            hr=ID3D11DeviceContext_GetData(g.context,(ID3D11Asynchronous*)s->query[1+k*2+j],&ticks[k][j],sizeof ticks[k][j],D3D11_ASYNC_GETDATA_DONOTFLUSH);
            if(FAILED(hr)){gpu_timing_release();fprintf(stderr,"[GPU-PROFILE] timestamp read failed; diagnostics disabled, rendering unchanged\n");return;}
            if(hr!=0){ready=0;break;}
        }
        uint64_t material_ticks[2]={0};
        if(ready && s->material_done)for(unsigned j=0;j<2;j++){
            hr=ID3D11DeviceContext_GetData(g.context,(ID3D11Asynchronous*)s->query[1+NV_GPU_TIME_KINDS*2+j],&material_ticks[j],sizeof material_ticks[j],D3D11_ASYNC_GETDATA_DONOTFLUSH);
            if(FAILED(hr)){gpu_timing_release();fprintf(stderr,"[GPU-PROFILE] material query failed; diagnostics disabled\n");return;}
            if(hr!=0){ready=0;break;}
        }
        uint64_t shade_ticks[2]={0};
        if(ready && s->shade_done)for(unsigned j=0;j<2;j++){
            hr=ID3D11DeviceContext_GetData(g.context,(ID3D11Asynchronous*)s->query[1+NV_GPU_TIME_KINDS*2+2+j],
                &shade_ticks[j],sizeof shade_ticks[j],D3D11_ASYNC_GETDATA_DONOTFLUSH);
            if(FAILED(hr)){gpu_timing_release();fprintf(stderr,"[GPU-SHADE-PROBE] query failed; diagnostics disabled\n");return;}
            if(hr!=0){ready=0;break;}
        }
        if(!ready)continue;
        if(s->shade_done && s->material_done && s->material_kind==1 &&
           material_ticks[1]>=material_ticks[0] && shade_ticks[1]>=shade_ticks[0]){
            double scale=1000.0/(double)disjoint.Frequency;
            ++gpu_time.shade_result.matched;gpu_time.shade_result.triangles+=s->material_work;
            gpu_time.shade_result.full_ms+=(material_ticks[1]-material_ticks[0])*scale;
            gpu_time.shade_result.source_ms+=(shade_ticks[1]-shade_ticks[0])*scale;
        }
        if(s->material_done){
            if(material_ticks[1]<material_ticks[0])++gpu_time.invalid;
            else{
                unsigned c=s->material_kind;double ms=(double)(material_ticks[1]-material_ticks[0])*1000.0/(double)disjoint.Frequency;
                ++gpu_time.material[c].samples;gpu_time.material[c].sample_triangles+=s->material_work;
                gpu_time.material[c].ms+=ms;gpu_time.material[c].weighted_ms+=ms*s->material_runs;
                gpu_time.material[c].run_weights+=s->material_runs;
                if(ms>gpu_time.material[c].max_ms)gpu_time.material[c].max_ms=ms;
            }
        }
        for(unsigned k=0;k<NV_GPU_TIME_KINDS;k++)if(s->mask&(1u<<k)){
            if(ticks[k][1]<ticks[k][0]){++gpu_time.invalid;continue;}
            double ms=(double)(ticks[k][1]-ticks[k][0])*1000.0/(double)disjoint.Frequency;
            ++gpu_time.phase[k].samples;gpu_time.phase[k].work+=s->work[k];gpu_time.phase[k].ms+=ms;
            if(ms>gpu_time.phase[k].max_ms)gpu_time.phase[k].max_ms=ms;
            if(gpu_time.spread){
                GpuSpreadPhase *p=&gpu_time.by_class[s->category][k];
                if(!p->samples || s->work[k]<p->min_work)p->min_work=s->work[k];
                if(!p->samples || s->work[k]>p->max_work)p->max_work=s->work[k];
                ++p->samples;p->work+=s->work[k];p->ms+=ms;if(ms>p->max_ms)p->max_ms=ms;
            }
        }
        s->closed=0;
    }
}
static unsigned gpu_timing_batch_begin(int copy)
{
    if(!gpu_time.enabled || g.selftest || g.auto_busy || gpu_time.active)return 0;
    unsigned category=copy?2u:(raw.count?1u:0u);
    if(gpu_time.spread){
        ++gpu_time.population[category].batches;
        /* Independent deterministic sequences avoid locking onto a recurring
         * first pass or a fixed every-N batch position in each frame. No
         * gameplay random state is read or changed. Selection is size-agnostic. */
        uint32_t x=gpu_time.random[category];x^=x<<13;x^=x>>17;x^=x<<5;gpu_time.random[category]=x;
        if((x&(NV_GPU_SPREAD_RATE-1u))!=0)return 0;
        ++gpu_time.population[category].selected;
    }else{
        if(gpu_time.frame<gpu_time.next_frame || (unsigned)copy!=gpu_time.want_copy)return 0;
        gpu_time.next_frame=gpu_time.frame+NV_GPU_TIME_FRAMES;
    }
    gpu_timing_poll();if(!gpu_time.enabled)return 0;
    for(unsigned i=0;i<NV_GPU_TIME_SLOTS;i++)if(!gpu_time.slot[i].closed){
        GpuTimeSlot *s=&gpu_time.slot[i];s->mask=0;s->category=category;s->material_done=s->material_runs=s->material_pick=s->material_at=s->shade_done=0;memset(s->work,0,sizeof s->work);gpu_time.active=i+1;
        ID3D11DeviceContext_Begin(g.context,(ID3D11Asynchronous*)s->query[0]);return i+1;
    }
    ++gpu_time.dropped;if(gpu_time.spread)++gpu_time.population[category].dropped;return 0;
}
static void gpu_timing_phase_begin(unsigned kind,uint64_t work)
{
    if(!gpu_time.active)return;GpuTimeSlot *s=&gpu_time.slot[gpu_time.active-1];s->work[kind]=work;
    ID3D11DeviceContext_End(g.context,(ID3D11Asynchronous*)s->query[1+kind*2]);
}
static void gpu_timing_phase_end(unsigned kind)
{
    if(!gpu_time.active)return;GpuTimeSlot *s=&gpu_time.slot[gpu_time.active-1];s->mask|=1u<<kind;
    ID3D11DeviceContext_End(g.context,(ID3D11Asynchronous*)s->query[2+kind*2]);
}
static void gpu_timing_batch_end(unsigned token)
{
    if(!token || token!=gpu_time.active)return;
    GpuTimeSlot *s=&gpu_time.slot[token-1];ID3D11DeviceContext_End(g.context,(ID3D11Asynchronous*)s->query[0]);
    s->closed=1;gpu_time.active=0;if(!gpu_time.spread)gpu_time.want_copy^=1;
}
/* At most one draw range per already-sampled batch: two timestamps inside
 * its existing disjoint scope. Selection is by range, not triangle count.
 * Repeated material ranges are never timed as one enclosing interval. */
static int gpu_timing_material_wants(void)
{return gpu_time.enabled && gpu_time.materials && gpu_time.active && !g.selftest;}
static void gpu_timing_material_select(unsigned runs)
{
    if(!gpu_timing_material_wants() || !runs)return;
    uint32_t x=gpu_time.material_random;x^=x<<13;x^=x>>17;x^=x<<5;gpu_time.material_random=x;
    GpuTimeSlot *s=&gpu_time.slot[gpu_time.active-1];s->material_runs=runs;s->material_pick=x%runs;s->material_at=0;
}
static unsigned gpu_timing_material_begin(unsigned kind,unsigned triangles)
{
    if(!gpu_time.enabled || !gpu_time.materials || g.selftest || g.auto_busy)return 0;
    ++gpu_time.material[kind].draws;gpu_time.material[kind].triangles+=triangles;
    if(!gpu_time.active)return 0;
    GpuTimeSlot *s=&gpu_time.slot[gpu_time.active-1];
    if(!s->material_runs || s->material_at++!=s->material_pick)return 0;
    s->material_kind=kind;s->material_work=triangles;
    ID3D11DeviceContext_End(g.context,(ID3D11Asynchronous*)s->query[1+NV_GPU_TIME_KINDS*2]);return gpu_time.active;
}
static void gpu_timing_material_end(unsigned token)
{
    if(!token || token!=gpu_time.active)return;
    GpuTimeSlot *s=&gpu_time.slot[token-1];
    ID3D11DeviceContext_End(g.context,(ID3D11Asynchronous*)s->query[2+NV_GPU_TIME_KINDS*2]);s->material_done=1;
}
static void gpu_timing_shade_begin(void)
{
    if(gpu_time.active && gpu_time.shade)
        ID3D11DeviceContext_End(g.context,(ID3D11Asynchronous*)gpu_time.slot[gpu_time.active-1].query[1+NV_GPU_TIME_KINDS*2+2]);
}
static void gpu_timing_shade_end(void)
{
    if(gpu_time.active && gpu_time.shade){
        GpuTimeSlot *s=&gpu_time.slot[gpu_time.active-1];
        ID3D11DeviceContext_End(g.context,(ID3D11Asynchronous*)s->query[2+NV_GPU_TIME_KINDS*2+2]);s->shade_done=1;
    }
}
static void gpu_timing_frame(void)
{
    if(InterlockedCompareExchange(&s_startup,0,0)==2)++gpu_time.frame;
}
static void gpu_timing_report(void)
{
    gpu_timing_poll();unsigned pending=0;for(unsigned i=0;i<NV_GPU_TIME_SLOTS;i++)pending+=gpu_time.slot[i].closed!=0;
    fprintf(stderr,"[GPU-PROFILE] enabled=%u interval_frames=%u slots=%u pending=%u dropped=%llu invalid=%llu spread=%u\n",
        (unsigned)gpu_time.enabled,gpu_time.spread?0u:NV_GPU_TIME_FRAMES,NV_GPU_TIME_SLOTS,pending,(unsigned long long)gpu_time.dropped,(unsigned long long)gpu_time.invalid,(unsigned)gpu_time.spread);
    const char *names[NV_GPU_TIME_KINDS]={"vertex","prepare","pixels","copy"};
    for(unsigned k=0;k<NV_GPU_TIME_KINDS;k++)if(gpu_time.phase[k].samples){
        fprintf(stderr,"[GPU-PASS-TIME] phase=%s samples=%llu gpu_ms=%.6f avg_ms=%.6f max_ms=%.6f work=%llu\n",names[k],
            (unsigned long long)gpu_time.phase[k].samples,gpu_time.phase[k].ms,gpu_time.phase[k].ms/(double)gpu_time.phase[k].samples,
            gpu_time.phase[k].max_ms,(unsigned long long)gpu_time.phase[k].work);
    }
    fprintf(stderr,"[GPU-SPREAD] enabled=%u inverse_probability=%u classes=%u\n",(unsigned)(gpu_time.enabled && gpu_time.spread),NV_GPU_SPREAD_RATE,NV_GPU_SPREAD_CLASSES);
    if(gpu_time.spread){
        const char *classes[NV_GPU_SPREAD_CLASSES]={"prepared-render","raw-mixed-render","instrumented-copy"};
        for(unsigned c=0;c<NV_GPU_SPREAD_CLASSES;c++){
            fprintf(stderr,"[GPU-SPREAD-POP] class=%s eligible=%llu selected=%llu dropped=%llu\n",classes[c],
                (unsigned long long)gpu_time.population[c].batches,(unsigned long long)gpu_time.population[c].selected,(unsigned long long)gpu_time.population[c].dropped);
            for(unsigned k=0;k<NV_GPU_TIME_KINDS;k++){
                GpuSpreadPhase *p=&gpu_time.by_class[c][k];if(!p->samples)continue;
                fprintf(stderr,"[GPU-SPREAD-TIME] class=%s phase=%s samples=%llu gpu_ms=%.6f avg_ms=%.6f max_ms=%.6f work=%llu min_work=%llu max_work=%llu\n",
                    classes[c],names[k],(unsigned long long)p->samples,p->ms,p->ms/(double)p->samples,p->max_ms,
                    (unsigned long long)p->work,(unsigned long long)p->min_work,(unsigned long long)p->max_work);
            }
        }
    }
    fprintf(stderr,"[GPU-MATERIAL-PROFILE] enabled=%u frame=%llu host_ms=%.3f sampled_ranges_per_batch=1 extra_timestamps_per_sample=2\n",(unsigned)(gpu_time.enabled && gpu_time.materials),(unsigned long long)gpu_time.frame,gpu_ms());
    const char *material_names[NV_GPU_MATERIAL_KINDS]={"generic","simple","literal1","literal2","literal3"};
    for(unsigned c=0;c<NV_GPU_MATERIAL_KINDS;c++)fprintf(stderr,"[GPU-MATERIAL-TIME] class=%s draws=%llu triangles=%llu samples=%llu sampled_triangles=%llu gpu_ms=%.6f max_ms=%.6f range_weighted_ms=%.6f range_weights=%llu\n",
        material_names[c],(unsigned long long)gpu_time.material[c].draws,(unsigned long long)gpu_time.material[c].triangles,
        (unsigned long long)gpu_time.material[c].samples,(unsigned long long)gpu_time.material[c].sample_triangles,
        gpu_time.material[c].ms,gpu_time.material[c].max_ms,gpu_time.material[c].weighted_ms,(unsigned long long)gpu_time.material[c].run_weights);
    fprintf(stderr,"[GPU-SHADE-PROBE] enabled=%u ready=%u matched=%llu triangles=%llu full_ms=%.6f source_only_ms=%.6f replays=%llu scratch_bytes=%llu skips=%llu host_ms=%.3f\n",
        (unsigned)gpu_time.shade,(unsigned)g.shade_probe.ready,
        (unsigned long long)gpu_time.shade_result.matched,(unsigned long long)gpu_time.shade_result.triangles,
        gpu_time.shade_result.full_ms,gpu_time.shade_result.source_ms,
        (unsigned long long)g.shade_probe.replays,(unsigned long long)g.shade_probe.scratch_bytes,
        (unsigned long long)g.shade_probe.skips,gpu_ms());
    memset(&gpu_time.shade_result,0,sizeof gpu_time.shade_result);
    g.shade_probe.replays=g.shade_probe.scratch_bytes=g.shade_probe.skips=0;
    memset(gpu_time.material,0,sizeof gpu_time.material);
    /* Pending queries and PRNG state survive reports. Their results can land
     * in the next window; populations and completed timings are separate. */
    memset(gpu_time.population,0,sizeof gpu_time.population);memset(gpu_time.by_class,0,sizeof gpu_time.by_class);
    memset(gpu_time.phase,0,sizeof gpu_time.phase);gpu_time.dropped=gpu_time.invalid=0;
}
#else
static int gpu_timing_material_wants(void){return 0;}
static void gpu_timing_material_select(unsigned runs){(void)runs;}
static unsigned gpu_timing_material_begin(unsigned kind,unsigned triangles){(void)kind;(void)triangles;return 0;}
static void gpu_timing_material_end(unsigned token){(void)token;}
static void gpu_timing_shade_begin(void){}
static void gpu_timing_shade_end(void){}
static void gpu_timing_initialize(void){}
static unsigned gpu_timing_batch_begin(int copy){(void)copy;return 0;}
static void gpu_timing_phase_begin(unsigned kind,uint64_t work){(void)kind;(void)work;}
static void gpu_timing_phase_end(unsigned kind){(void)kind;}
static void gpu_timing_batch_end(unsigned token){(void)token;}
static void gpu_timing_frame(void){}
static void gpu_timing_report(void){}
#endif
