/* CPU-side metadata and bounded material summaries. No asset/object identity
 * is inferred from a shader signature. Masks are private per-upload inputs. */
static void gpu_material_record(const NvGpuState *state,unsigned triangles)
{
    uint32_t words[36],hash=2166136261u;
    memcpy(words,state->rc.color_icw,32);memcpy(words+8,state->rc.alpha_icw,32);
    memcpy(words+16,state->rc.color_ocw,32);memcpy(words+24,state->rc.alpha_ocw,32);
    words[32]=state->rc.final0;words[33]=state->rc.final1;words[34]=state->rc.control;words[35]=state->rc.stage_program;
    for(unsigned i=0;i<36;i++)hash=(hash^words[i])*16777619u;
    unsigned slot;for(slot=0;slot<g.material.program_count;slot++)
        if(g.material.program[slot].hash==hash && !memcmp(g.material.program[slot].words,words,sizeof words))break;
    if(slot==g.material.program_count){
        if(slot==32){++g.material.untracked_states;return;}
        ++g.material.program_count;g.material.program[slot].hash=hash;
        memcpy(g.material.program[slot].words,words,sizeof words);
        fprintf(stderr,"[GPU-MATERIAL-PROGRAM] id=%u control=%08X stages=%08X final0=%08X final1=%08X",slot,words[34],words[35],words[32],words[33]);
        static const char *names[4]={"rgb_in","alpha_in","rgb_out","alpha_out"};
        for(unsigned kind=0;kind<4;kind++){
            fprintf(stderr," %s=",names[kind]);for(unsigned i=0;i<8;i++)fprintf(stderr,"%s%08X",i?",":"",words[kind*8+i]);
        }
        fputc('\n',stderr);
    }
    ++g.material.program[slot].states;g.material.program[slot].triangles+=triangles;
}
static ID3D11ShaderResourceView *gpu_material_prepare(GpuBuffer *buffer)
{
    unsigned rc_triangles[NV_GPU_MAX_STATES]={0};int needs_masks=0;
    for(unsigned i=0;i<g.queued_count;i++)if(g.queued[i].use_rc)++rc_triangles[g.queued[i].state_index];
    for(unsigned i=0;i<g.state_count;i++){
        const Nv2aCombiner *rc=&g.queued_state[i].rc;
        unsigned read=nv_gpu_texture_reads(rc),active=nv_gpu_texture_enabled(rc),clip=nv_gpu_texture_clip(rc);
        unsigned dead=active&~(read|clip)&15u;
        g.material.masks[i]=read|clip;
        int source_work=(g.queued_state[i].color_mask&0x01010101u) || g.queued_state[i].alpha_test;
        if(dead && rc_triangles[i] && source_work)needs_masks=1;
        if(!g.selftest && rc_triangles[i]){
            unsigned dead_count=0,off_count=0;for(unsigned st=0;st<4;st++){dead_count+=(dead>>st)&1u;off_count+=((active>>st)&1u)==0;}
            if(!source_work)dead_count=off_count=0;
            g.material.dead_tri_stages+=(uint64_t)dead_count*rc_triangles[i];
            g.material.off_tri_stages+=(uint64_t)off_count*rc_triangles[i];
            gpu_material_record(&g.queued_state[i],rc_triangles[i]);
        }
    }
    if(!g.material.enabled || !g.native)return NULL;
    if(!g.selftest)++g.material.batches;
    if(!needs_masks)return g.material.fallback.srv;
    if(gpu_upload(buffer,g.material.masks,g.state_count*4,4)){
        if(!g.selftest)++g.material.mask_batches;
        return buffer->srv;
    }
    /* The compiled path receives an all-live mask on any optional allocation
     * failure; accepted draws keep evaluation of all active source stages. */
    if(!g.selftest)++g.material.fallback_batches;
    return g.material.fallback.srv;
}
static void gpu_material_report(void)
{
    fprintf(stderr,"[GPU-MATERIAL-USAGE] enabled=%u batches=%llu mask_batches=%llu fallback_batches=%llu dead_active_tri_stages=%llu off_tri_stages=%llu programs=%u untracked_states=%llu\n",
        (unsigned)(g.native && g.material.enabled),(unsigned long long)g.material.batches,
        (unsigned long long)g.material.mask_batches,(unsigned long long)g.material.fallback_batches,
        (unsigned long long)g.material.dead_tri_stages,(unsigned long long)g.material.off_tri_stages,
        g.material.program_count,(unsigned long long)g.material.untracked_states);
    uint32_t printed=0;
    for(unsigned n=0;n<8;n++){
        unsigned best=32;uint64_t count=0;
        for(unsigned i=0;i<g.material.program_count;i++)if(!(printed&(1u<<i)) && g.material.program[i].triangles>count){best=i;count=g.material.program[i].triangles;}
        if(best==32)break;printed|=1u<<best;
        fprintf(stderr,"[GPU-MATERIAL-WORK] id=%u states=%llu triangles=%llu\n",best,(unsigned long long)g.material.program[best].states,(unsigned long long)count);
    }
    for(unsigned i=0;i<g.material.program_count;i++)g.material.program[i].states=g.material.program[i].triangles=0;
    g.material.batches=g.material.mask_batches=g.material.fallback_batches=0;
    g.material.dead_tri_stages=g.material.off_tri_stages=g.material.untracked_states=0;
}
