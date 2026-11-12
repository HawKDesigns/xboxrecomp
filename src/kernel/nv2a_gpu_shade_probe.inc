/* A sampled simple-material range is replayed to a private UAV after its real
 * draw. This measures source/texture work with matched geometry and material,
 * without overwriting the game's surface, depth or visibility counters.
 * Full/source timings are paired but NOT an additive cost decomposition. */
#ifndef NV_GPU_NO_TIMESTAMPS
static int gpu_shade_probe_initialize(void)
{
    if(!g.shade_probe.enabled || !gpu_time.enabled || !gpu_time.shade)return 1;
    g.shade_probe.compiling=1;
    unsigned variant=gpu_native_variant(2u|(g.array_registers?1u:0u))|65536u;
    uint64_t key=gpu_compile_key(nv_gpu_rov_source,sizeof(nv_gpu_rov_source)-1,
                                  "pixel_main","ps_5_0",variant);
    char path[128];snprintf(path,sizeof path,"shader-cache/qpr2-shade-probe-%016llx.dxbc",
                            (unsigned long long)key);
    int ok=gpu_rov_code(0,1,path,key);
    g.shade_probe.compiling=0;
    if(ok)ok=gpu_buffer_create(&g.shade_probe.sink,1024u*1024u*4u,4,2);
    if(!ok){
        if(g.shade_probe.shader){ID3D11PixelShader_Release(g.shade_probe.shader);g.shade_probe.shader=NULL;}
        gpu_buffer_release(&g.shade_probe.sink);
        g.shade_probe.enabled=gpu_time.shade=0;
        fprintf(stderr,"[GPU-SHADE-PROBE] unavailable; normal rendering retained\n");
        return 1;
    }
    g.shade_probe.ready=1;
    fprintf(stderr,"[GPU-SHADE-PROBE] ready=1 scratch_only=1 paired_simple_material=1\n");
    return 1;
}

static void gpu_shade_probe_replay(unsigned token,unsigned simple,unsigned first,unsigned end,unsigned factor)
{
    if(!g.shade_probe.ready || !gpu_time.enabled || !gpu_time.shade || !token || !simple ||
       g.selftest || g.auto_busy || s_capture || g.target.width>1024 || g.target.height>1024 ||
       g.target.width*g.target.height*4u>g.shade_probe.sink.bytes){
        if(token && simple && g.shade_probe.ready)++g.shade_probe.skips;
        return;
    }
    /* The only output is a private structured buffer. The existing private
     * dummy RTV may receive zeros when UAV-only mode is unavailable. */
    ID3D11UnorderedAccessView *private_uav[5]={NULL,NULL,NULL,NULL,g.shade_probe.sink.uav};
    if(g.uav_only)ID3D11DeviceContext_OMSetRenderTargetsAndUnorderedAccessViews(g.context,0,NULL,NULL,0,5,private_uav,NULL);
    else ID3D11DeviceContext_OMSetRenderTargetsAndUnorderedAccessViews(g.context,1,&g.raster_view,NULL,1,4,private_uav+1,NULL);
    ID3D11DeviceContext_PSSetShader(g.context,g.shade_probe.shader,NULL,0);
    gpu_timing_shade_begin();
    if(g.native_batch)ID3D11DeviceContext_DrawIndexed(g.context,factor*(end-first),factor*first,0);
    else ID3D11DeviceContext_Draw(g.context,factor*(end-first),0);
    gpu_timing_shade_end();
    /* Clear u4 explicitly before restoring the exact production UAV set. */
    ID3D11UnorderedAccessView *none[5]={0};
    if(g.uav_only)ID3D11DeviceContext_OMSetRenderTargetsAndUnorderedAccessViews(g.context,0,NULL,NULL,0,5,none,NULL);
    else ID3D11DeviceContext_OMSetRenderTargetsAndUnorderedAccessViews(g.context,1,&g.raster_view,NULL,1,4,none+1,NULL);
    ID3D11UnorderedAccessView *live[5]={NULL,g.surface.uav,g.stats.uav,g.startup_trace.uav,NULL};
    if(g.uav_only)ID3D11DeviceContext_OMSetRenderTargetsAndUnorderedAccessViews(g.context,0,NULL,NULL,0,5,live,NULL);
    else ID3D11DeviceContext_OMSetRenderTargetsAndUnorderedAccessViews(g.context,1,&g.raster_view,NULL,1,4,live+1,NULL);
    ++g.shade_probe.replays;g.shade_probe.scratch_bytes+=(uint64_t)g.target.width*g.target.height*4u;
}
#else
static int gpu_shade_probe_initialize(void){return 1;}
static void gpu_shade_probe_replay(unsigned token,unsigned simple,unsigned first,unsigned end,unsigned factor)
{(void)token;(void)simple;(void)first;(void)end;(void)factor;}
#endif
