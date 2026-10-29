/* Admit the complete native shader set atomically. A rejected candidate keeps
 * the already validated V100 programs and all their rendering policies. */
static int gpu_store_initialize(void)
{
    const char *enabled=getenv("RECOMP_GPU_SKIP_UNCHANGED");
    if(!g.native || (enabled && atoi(enabled)<=0))return 1;
    ID3D11PixelShader **slots[5]={&g.pixel_shader,&g.simple_pixel_shader,
        &g.literal.shader[0],&g.literal.shader[1],&g.literal.shader[2]};
    ID3D11PixelShader *original[5];char paths[5][128];unsigned mask=0,attempted=0;
    memset(paths,0,sizeof paths);
    for(unsigned i=0;i<5;i++)original[i]=*slots[i];
    g.skip_unchanged=1;
    int ok=1;
    for(unsigned i=0;i<5 && ok;i++)if(original[i]){
        unsigned id=i>=2?i-1:0,compact=id && (g.literal.compact_ready&(1u<<(id-1)));
        unsigned variant=gpu_native_variant((i==1?2u:0u)|(g.array_registers?1u:0u));
        if(id)variant|=(id<<12)|(compact?32768u:0u);
        uint64_t key=gpu_compile_key(nv_gpu_rov_source,sizeof(nv_gpu_rov_source)-1,"pixel_main","ps_5_0",variant);
        if(id)snprintf(paths[i],sizeof paths[i],"shader-cache/qpr2-rov-literal%u%s-%016llx.dxbc",id,compact?"-compact":"",(unsigned long long)key);
        else snprintf(paths[i],sizeof paths[i],i==1?"shader-cache/qpr2-rov-simple-%016llx.dxbc":"shader-cache/qpr2-rov-ps-%016llx.dxbc",(unsigned long long)key);
        *slots[i]=NULL;attempted|=1u<<i;g.literal.compiling=id;g.literal.compact_compiling=compact!=0;
        ok=gpu_rov_code(0,i==1,paths[i],key);
        g.literal.compiling=g.literal.compact_compiling=0;
        if(ok)mask|=1u<<i;
    }
    if(ok)ok=gpu_store_selftest();
    if(ok>0)ok=gpu_selftest();
    if(ok>0)ok=gpu_mixed_selftest();
    if(ok>0)ok=gpu_material_selftest();
    for(unsigned id=1;id<=3 && ok>0;id++)if(g.literal.ready&(1u<<(id-1))){
        g.literal.testing=id;
        for(unsigned v=0;v<(id==2?3u:1u) && ok>0;v++){
            g.literal.variant=v;ok=gpu_material_selftest();
        }
        g.literal.testing=g.literal.variant=0;
    }
    if(ok>0 && !g.readback_failed){
        for(unsigned i=0;i<5;i++)if(original[i])ID3D11PixelShader_Release(original[i]);
        g.store_ready=mask;
        fprintf(stderr,"[GPU-STORE] comparison passed ready_mask=%u; unchanged color/depth omit store/fence, every writer fenced\n",mask);
        return 1;
    }
    g.skip_unchanged=0;g.store_rejected=1;
    for(unsigned i=0;i<5;i++){
        if((attempted&(1u<<i)) && *slots[i])ID3D11PixelShader_Release(*slots[i]);
        *slots[i]=original[i];
        if(paths[i][0]){gpu_keep_failed_shader(paths[i],"native-store-ps");remove(paths[i]);}
    }
    g.literal.compiling=g.literal.compact_compiling=g.literal.testing=g.literal.variant=0;
    g.active=g.resident=0;g.queued_count=g.state_count=g.queued_refs=0;
    for(unsigned i=0;i<128;i++)g.cache[i].queued=0;
    g.host_color=NULL;g.host_depth=NULL;memset(&g.completed,0,sizeof g.completed);
    if(ok<0 || g.readback_failed)return 0;
    fprintf(stderr,"[GPU-STORE] candidate rejected; validated V100 writer/fence policy retained\n");
    /* A mismatch must not leave private destinations or corrupt GPU state alive. */
    return gpu_store_selftest()==1 && gpu_selftest() && gpu_mixed_selftest()==1 && gpu_material_selftest()==1;
}
