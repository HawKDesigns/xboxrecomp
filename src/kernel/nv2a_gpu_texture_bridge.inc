/* Only exact ARGB layouts can bypass a CPU texture decoder. Snapshot identity
 * proves the version; host addresses alone never prove immutable pixels. */
static GpuPendingReadback *gpu_bridge_source(const NvGpuTexture *meta,const NvGpuTextureSource *source)
{
    if(!g.bridge.enabled || !g.native || g.auto_enabled || s_capture ||
       (g.selftest && !g.bridge.testing) || !source->gpu_argb_layout ||
       source->palette_bytes || meta->cube || !meta->width || !meta->height ||
       source->bytes!=(size_t)meta->width*meta->height*4 ||
       !((source->gpu_argb_layout==1 && meta->format==0x12) ||
         (source->gpu_argb_layout==2 && meta->format==0x06)))return NULL;
    GpuPendingReadback *match=NULL;
    for(unsigned i=0;i<g.pending_count;i++) {
        GpuPendingReadback *p=&g.pending[i];
        if(gpu_host_overlap(source->raw,source->bytes,p->host_depth,
                            gpu_target_depth_bytes(&p->target,p->depth_stride)))return NULL;
        if(!gpu_host_overlap(source->raw,source->bytes,p->host_color,
                             gpu_target_color_bytes(&p->target)))continue;
        if(match || !p->serial || !p->snapshot.srv || p->target.bpp!=4 ||
           gpu_canonical_host(source->raw)!=gpu_canonical_host(p->host_color) ||
           meta->width!=p->target.width || meta->height!=p->target.height ||
           (source->gpu_argb_layout==2)!=(((p->target.format>>8)&15)==2) ||
           (source->gpu_argb_layout==1 && p->target.pitch!=p->target.width*4u)) {
            if(!g.selftest)++g.bridge.misses;
            return NULL;
        }
        match=p;
    }
    if(match && !g.selftest)++g.bridge.eligible;
    return match;
}
static int gpu_bridge_prepare(unsigned texels)
{
    if(g.bridge.texels.bytes>=texels*4u)return 1;
    GpuBuffer next={0};
    if(!gpu_buffer_create(&next,texels*4u,4,2))return 0;
    gpu_buffer_release(&g.bridge.texels);g.bridge.texels=next;return 1;
}
static int gpu_bridge_pack(GpuPendingReadback *p,unsigned base,unsigned texels)
{
    if(!p->serial || !p->snapshot.srv || !g.bridge.shader ||
       !g.bridge.parameters.buffer || g.bridge.texels.bytes<texels*4u ||
       (uint64_t)(base+texels)*4u>g.atlas.bytes)return 0;
    uint32_t parameters[4]={texels,0,0,0};
    ID3D11DeviceContext_UpdateSubresource(g.context,(ID3D11Resource*)g.bridge.parameters.buffer,0,NULL,parameters,0,0);
    ID3D11ShaderResourceView *srv[5]={p->snapshot.srv,g.bridge.parameters.srv,NULL,NULL,NULL};
    ID3D11UnorderedAccessView *uav[3]={g.bridge.texels.uav,NULL,NULL};
    ID3D11DeviceContext_CSSetShader(g.context,g.bridge.shader,NULL,0);
    ID3D11DeviceContext_CSSetShaderResources(g.context,0,5,srv);
    ID3D11DeviceContext_CSSetUnorderedAccessViews(g.context,0,3,uav,NULL);
    ID3D11DeviceContext_Dispatch(g.context,(texels+63)/64,1,1);
    memset(srv,0,sizeof srv);memset(uav,0,sizeof uav);
    ID3D11DeviceContext_CSSetShaderResources(g.context,0,5,srv);
    ID3D11DeviceContext_CSSetUnorderedAccessViews(g.context,0,3,uav,NULL);
    /* Both buffers here have identical 4-byte structured elements. The 8-byte
     * ordered color/depth records were packed by the compute shader above. */
    D3D11_BOX box={0,0,0,texels*4u,1,1};
    ID3D11DeviceContext_CopySubresourceRegion(g.context,(ID3D11Resource*)g.atlas.buffer,0,base*4u,0,0,
        (ID3D11Resource*)g.bridge.texels.buffer,0,&box);
    if(!g.selftest){++g.bridge.packs;g.bridge.bytes+=(uint64_t)texels*4u;}
    return 1;
}
static int gpu_bridge_initialize(void)
{
    const char *enabled=getenv("RECOMP_GPU_TEXTURE_BRIDGE");
    const char *experimental=getenv("RECOMP_GPU_AUTOTUNE_EXPERIMENT");
    if(!g.native || !g.readback_queue || (enabled && atoi(enabled)<=0) ||
       (experimental && atoi(experimental)>0))return 0;
    uint64_t key=gpu_compile_key(nv_gpu_texcopy_source,sizeof(nv_gpu_texcopy_source)-1,"main","cs_5_0",0);
    char path[128];snprintf(path,sizeof path,"shader-cache/qpr2-texture-handoff-%016llx.dxbc",(unsigned long long)key);
    if(!gpu_buffer_create(&g.bridge.parameters,16,4,0))return 0;
    int ok=gpu_shader_load(path,key,&g.bridge.shader);
    if(!ok) {
        ID3DBlob *code=NULL,*errors=NULL;
        HRESULT hr=D3DCompile(nv_gpu_texcopy_source,sizeof(nv_gpu_texcopy_source)-1,"qpr2_gpu_texcopy",
            NULL,NULL,"main","cs_5_0",GPU_SHADER_FLAGS,0,&code,&errors);
        if(SUCCEEDED(hr)) {
            hr=ID3D11Device_CreateComputeShader(g.device,ID3D10Blob_GetBufferPointer(code),ID3D10Blob_GetBufferSize(code),NULL,&g.bridge.shader);
            if(SUCCEEDED(hr))gpu_shader_save(path,key,code);
        } else fprintf(stderr,"[GPU-TEXTURE-BRIDGE] compile failed: %s; CPU texture dependencies retained\n",
            errors?(char*)ID3D10Blob_GetBufferPointer(errors):"no diagnostic");
        if(errors)ID3D10Blob_Release(errors);if(code)ID3D10Blob_Release(code);
        ok=SUCCEEDED(hr);
    }
    g.bridge.enabled=ok;return ok;
}
