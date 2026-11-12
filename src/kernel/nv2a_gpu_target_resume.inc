/* Resume only a complete, exact target version. CPU baselines are separate
 * from the rendered snapshot: an unchanged stale mirror is not GPU output. */
static int gpu_resume_host_equal(const NvGpuState *s,const uint8_t *color,
    const float *depth,unsigned stride,const uint32_t *colors,const uint32_t *depths)
{
    for(unsigned y=0;y<s->height;y++) {
        unsigned at=y*s->width;
        if(((s->format>>8)&15)!=2) {
            if(memcmp(color+(size_t)y*s->pitch,colors+at,(size_t)s->width*4))return 0;
        } else for(unsigned x=0;x<s->width;x++) {
            uint32_t v;memcpy(&v,color+gpu_color_at(s,x,y),4);
            if(v!=colors[at+x])return 0;
        }
        if(depth && memcmp(depth+(size_t)y*stride,depths+at,(size_t)s->width*4))return 0;
    }
    return 1;
}
static void gpu_resume_save(GpuPendingReadback *p)
{
    p->baseline_valid=0;
    if(!g.resume.enabled || !g.bridge.enabled || !p->serial || !g.native || s_capture ||
       (g.selftest && !g.resume.testing) || g.target.bpp!=4)return;
    /* Canonical upload mirrors still describe the CPU input at the bind.
     * A CPU edit/clear since then makes this version ineligible, even if it
     * occurred before the queued snapshot was created. */
    if(!gpu_resume_host_equal(&g.target,g.host_color,g.host_depth,g.depth_stride,
        g.color_upload,(const uint32_t*)g.depth_upload)) {
        if(!g.selftest)++g.resume.changed;return;
    }
    unsigned bytes=g.target.width*g.target.height*8u;
    if(p->baseline_bytes<bytes) {
        uint32_t *next=malloc(bytes);
        if(!next) {
            g.resume.enabled=0;if(!g.selftest)++g.resume.allocation_failures;
            fprintf(stderr,"[GPU-TARGET-RESUME] baseline allocation failed; CPU target dependencies retained\n");return;
        }
        free(p->host_baseline);p->host_baseline=next;p->baseline_bytes=bytes;
    }
    unsigned pixels=bytes/8;
    memcpy(p->host_baseline,g.color_upload,pixels*4u);
    memcpy(p->host_baseline+pixels,g.depth_upload,pixels*4u);
    p->baseline_valid=1;
}
static GpuPendingReadback *gpu_resume_source(const NvGpuState *s,const uint8_t *color,const float *depth,unsigned stride)
{
    if(!g.resume.enabled || !g.bridge.enabled || !g.native || g.auto_enabled ||
       s_capture || (g.selftest && !g.resume.testing) || s->bpp!=4 || !g.pending_count)return NULL;
    GpuPendingReadback *match=NULL;
    size_t cb=gpu_target_color_bytes(s),db=depth?gpu_target_depth_bytes(s,stride):0;
    for(unsigned i=0;i<g.pending_count;i++) {
        GpuPendingReadback *p=g.pending+i;
        size_t pcb=gpu_target_color_bytes(&p->target),pdb=p->host_depth?gpu_target_depth_bytes(&p->target,p->depth_stride):0;
        int overlap=gpu_host_overlap(color,cb,p->host_color,pcb) ||
            gpu_host_overlap(color,cb,p->host_depth,pdb) ||
            gpu_host_overlap(depth,db,p->host_color,pcb) ||
            gpu_host_overlap(depth,db,p->host_depth,pdb);
        if(!overlap)continue;
        /* Multiple writers, shared depth, shifted/tiled aliases and partial
         * targets require normal ordered CPU publication. Never guess latest. */
        if(match || !p->baseline_valid || !p->serial || !p->snapshot.buffer ||
           color!=p->host_color || depth!=p->host_depth || stride!=p->depth_stride ||
           s->width!=p->target.width || s->height!=p->target.height ||
           s->pitch!=p->target.pitch || s->bpp!=p->target.bpp || s->format!=p->target.format ||
           p->snapshot.bytes!=s->width*s->height*8u) {
            if(!g.selftest)++g.resume.conflicts;return NULL;
        }
        match=p;
    }
    if(!match)return NULL;
    if(!g.selftest)++g.resume.checks;
    unsigned pixels=s->width*s->height;
    if(!gpu_resume_host_equal(s,color,depth,stride,match->host_baseline,match->host_baseline+pixels)) {
        if(!g.selftest)++g.resume.changed;return NULL;
    }
    return match;
}
static int gpu_resume_target(const NvGpuState *s,uint8_t *color,float *depth,unsigned stride)
{
    GpuPendingReadback *p=gpu_resume_source(s,color,depth,stride);
    if(!p)return 0;
    if(!gpu_native_target(s->width,s->height) || !gpu_target_buffers(s->width*s->height*4u))return -1;
    ID3D11DeviceContext_CopyResource(g.context,(ID3D11Resource*)g.surface.buffer,(ID3D11Resource*)p->snapshot.buffer);
    unsigned pixels=s->width*s->height;
    /* Preserve the CPU input baseline for a later attempt to queue this
     * resumed target. It intentionally differs from rendered GPU pixels. */
    memcpy(g.color_upload,p->host_baseline,pixels*4u);
    memcpy(g.depth_upload,p->host_baseline+pixels,pixels*4u);
    g.target=*s;g.host_color=color;g.host_depth=depth;g.depth_stride=stride;
    g.active=g.resume.active=1;g.resident=0;g.target_written=1;g.pending_triangles=0;
    g.stats_required=g.stats_initialized=0;
    g.dirty_left=s->width;g.dirty_top=s->height;g.dirty_right=g.dirty_bottom=0;
    if(g.resume.retire_pixels) {
        /* Transfer every outstanding color/depth publication obligation to
         * the restored full GPU image BEFORE retiring the older CPU copy.
         * Later small draws or clears must not lose pixels outside their box. */
        g.dirty_left=p->left;g.dirty_top=p->top;g.dirty_right=p->right;g.dirty_bottom=p->bottom;
        p->publish_surface=0;
        if(!g.selftest){++g.resume.retired;g.resume.retired_bytes+=p->surface_bytes;}
    }
    ++g.resume.restores;g.resume.bytes+=(uint64_t)pixels*8u;
    if(!g.selftest){
        ++g.target_binds;
        ++g.target_shape[gpu_target_shape(s->width,s->height)].binds;
    }
    return 1;
}
