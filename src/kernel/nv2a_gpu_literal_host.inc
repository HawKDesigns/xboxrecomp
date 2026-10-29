/* Optional, independently gated native programs. No runtime shader compilation
 * or guest readback is introduced: all candidates are checked before readiness. */
static void gpu_literal_prepare(void)
{
    memset(g.literal.state_class,0,g.state_count);
    if(!g.literal.enabled || !g.native_batch)return;
    for(unsigned state=0;state<g.state_count;state++){
        unsigned id=nv_gpu_literal_program(&g.queued_state[state].rc);
        if(id && g.literal.shader[id-1] && (g.literal.testing==id || (g.literal.ready&(1u<<(id-1)))))
            g.literal.state_class[state]=(uint8_t)id;
    }
}
static unsigned gpu_literal_program(unsigned triangle)
{
    if(!g.literal.enabled || !g.native_batch || g.queued_kind[triangle])return 0;
    return g.literal.state_class[g.queued[triangle].state_index];
}
static int gpu_literal_initialize(void)
{
    const char *enabled=getenv("RECOMP_GPU_LITERAL_MATERIALS");
    g.literal.enabled=NV_GPU_LITERAL_AVAILABLE && (!enabled || atoi(enabled)>0);
    if(!g.literal.enabled || !g.native || !g.native_batch)return 1;
    const char *compact=getenv("RECOMP_GPU_COMPACT_MATERIALS");
    g.literal.compact_enabled=!compact || atoi(compact)>0;
    for(unsigned id=1;id<=NV_GPU_LITERAL_CLASSES;id++){
        for(unsigned attempt=0;attempt<(g.literal.compact_enabled?2u:1u);attempt++){
            unsigned is_compact=g.literal.compact_enabled && !attempt;
            uint64_t key=gpu_compile_key(nv_gpu_rov_source,sizeof(nv_gpu_rov_source)-1,"pixel_main","ps_5_0",gpu_native_variant(g.array_registers?1u:0u)|(id<<12)|(is_compact?32768u:0u));
            char path[128];snprintf(path,sizeof path,"shader-cache/qpr2-rov-literal%u%s-%016llx.dxbc",id,is_compact?"-compact":"",(unsigned long long)key);
            g.literal.compiling=id;g.literal.compact_compiling=is_compact;
            int ok=gpu_rov_code(0,0,path,key);g.literal.compiling=0;
            if(ok){
                g.literal.testing=id;
                unsigned variants=id==2?3:1;
                for(unsigned variant=0;variant<variants && ok;variant++){
                    g.literal.variant=variant;ok=gpu_material_selftest();
                    if(ok<0 || g.readback_failed){g.literal.testing=g.literal.variant=g.literal.compact_compiling=0;return 0;}
                }
                g.literal.testing=g.literal.variant=0;
            }
            g.literal.compact_compiling=0;
            if(ok){
                g.literal.ready|=1u<<(id-1);
                if(is_compact)g.literal.compact_ready|=1u<<(id-1);
                fprintf(stderr,"[GPU-LITERAL] program=%u material comparison passed compact=%u\n",id,is_compact);
                break;
            }
            char label[48];snprintf(label,sizeof label,"native-literal%u%s-ps",id,is_compact?"-compact":"");gpu_keep_failed_shader(path,label);remove(path);
            if(g.literal.shader[id-1]){ID3D11PixelShader_Release(g.literal.shader[id-1]);g.literal.shader[id-1]=NULL;}
            if(is_compact){
                g.literal.compact_rejected|=1u<<(id-1);
                fprintf(stderr,"[GPU-COMPACT] program=%u rejected; testing V97 literal shader\n",id);
            }else{
                g.literal.rejected|=1u<<(id-1);
                fprintf(stderr,"[GPU-LITERAL] program=%u rejected; general material shader retained\n",id);
            }
        }
    }
    return 1;
}
static void gpu_literal_report(void)
{
    fprintf(stderr,"[GPU-LITERAL] enabled=%u ready_mask=%u rejected_mask=%u draws=%llu triangles=%llu generic_draws=%llu\n",
        (unsigned)(g.native && g.literal.enabled && g.native_batch),g.literal.ready,g.literal.rejected,
        (unsigned long long)g.literal.draws,(unsigned long long)g.literal.triangles,(unsigned long long)g.literal.generic_draws);
    fprintf(stderr,"[GPU-COMPACT] enabled=%u ready_mask=%u rejected_mask=%u\n",g.literal.compact_enabled,g.literal.compact_ready,g.literal.compact_rejected);
    for(unsigned id=0;id<NV_GPU_LITERAL_CLASSES;id++)if(g.literal.class_triangles[id])
        fprintf(stderr,"[GPU-LITERAL-WORK] program=%u triangles=%llu\n",id+1,(unsigned long long)g.literal.class_triangles[id]);
    g.literal.draws=g.literal.triangles=g.literal.generic_draws=0;
    memset(g.literal.class_triangles,0,sizeof g.literal.class_triangles);
}
