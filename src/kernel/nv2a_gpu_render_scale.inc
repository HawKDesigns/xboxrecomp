/* The guest owns the original color/depth layout. Two optional CPU snapshots
 * preserve finer samples across full GPU drains and backbuffer swaps. A
 * guest write can invalidate either channel without invalidating the other. */
static unsigned gpu_scaled_target(const NvGpuState *target)
{
    if(g.native && g.scaled.scale > 1 && (!g.selftest || g.scaled.testing) && !s_capture &&
       target->width == NV2A_SCANOUT_WIDTH && target->height == NV2A_SCANOUT_HEIGHT &&
       target->bpp == 4 && target->pitch == NV2A_SCANOUT_WIDTH * 4 &&
       ((target->format >> 8) & 15) == 1 && !(target->format & 0xF000u))
        return g.scaled.scale;
    return 1;
}

static GpuScaledSnapshot *gpu_scaled_find(const void *color)
{
    uintptr_t key=gpu_canonical_host(color);
    for(unsigned i=0;i<2;i++) {
        GpuScaledSnapshot *snapshot=&g.scaled.snapshots[i];
        if(snapshot->used && snapshot->color==key)return snapshot;
    }
    return NULL;
}

static void gpu_scaled_invalidate(const void *address,size_t bytes)
{
    for(unsigned i=0;i<2;i++) {
        GpuScaledSnapshot *snapshot=&g.scaled.snapshots[i];
        if(!snapshot->used)continue;
        if(snapshot->color_valid &&
           gpu_host_overlap(address,bytes,(const void *)snapshot->color,
                            (size_t)snapshot->pitch*NV2A_SCANOUT_HEIGHT))
            snapshot->color_valid=0;
        if(snapshot->depth_valid &&
           gpu_host_overlap(address,bytes,snapshot->depth,
                            ((NV2A_SCANOUT_HEIGHT-1u)*snapshot->stride+NV2A_SCANOUT_WIDTH)*sizeof(float)))
            snapshot->depth_valid=0;
    }
}

static void gpu_scaled_invalidate_target(const NvGpuState *target,
                                         const uint8_t *color,const float *depth,unsigned stride)
{
    if(!g.scaled.scale)return;
    if(target->color_mask & 0x01010101u)
        gpu_scaled_invalidate(color,gpu_target_color_bytes(target));
    if(depth && target->depth_mask)
        gpu_scaled_invalidate(depth,gpu_target_depth_bytes(target,stride));
}

void nv_gpu_scaled_cpu_write(const void *address,size_t bytes)
{
    if(InterlockedCompareExchange(&s_startup,0,0)==2 && g.scaled.scale)
        gpu_scaled_invalidate(address,bytes);
}

static void gpu_scaled_seed(const NvGpuState *target,const uint8_t *color,
                            const float *depth,unsigned stride)
{
    unsigned scale=g.scaled.active, width=target->width*scale;
    GpuScaledSnapshot *saved=gpu_scaled_find(color);
    int have_color=saved && saved->color_valid && saved->format==target->format;
    const GpuScaledSnapshot *saved_depth=NULL;
    for(unsigned i=0;i<2;i++) {
        const GpuScaledSnapshot *candidate=&g.scaled.snapshots[i];
        if(candidate->used && candidate->depth_valid && candidate->depth==depth &&
           candidate->stride==stride && (!saved_depth || candidate->used>saved_depth->used))
            saved_depth=candidate;
    }
    for(unsigned y=0;y<target->height;y++) {
        for(unsigned x=0;x<target->width;x++) {
            size_t source=(size_t)y*target->width+x;
            size_t center=(size_t)nv2a_native_sample(y,scale)*width+nv2a_native_sample(x,scale);
            GpuSurface base={g.color_upload[source],0};
            memcpy(&base.depth_bits,g.depth_upload+source,4);
            /* The CPU may have edited guest RAM between completions. Compare
             * the original sample bitwise before trusting the finer samples. */
            int keep_color=have_color && saved->pixels[center].color==base.color;
            int keep_depth=saved_depth && saved_depth->pixels[center].depth_bits==base.depth_bits;
            for(unsigned dy=0;dy<scale;dy++) {
                size_t left=(size_t)(y*scale+dy)*width+x*scale;
                for(unsigned dx=0;dx<scale;dx++) {
                    GpuSurface value=base;
                    if(keep_color)value.color=saved->pixels[left+dx].color;
                    if(keep_depth)value.depth_bits=saved_depth->pixels[left+dx].depth_bits;
                    g.scaled.upload[left+dx]=value;
                }
            }
        }
    }
}

/* A full drain often rebinds the same GPU allocation on the next batch.
 * Keep it when both guest mirrors are unchanged and no CPU write invalidated
 * a channel. */
static int gpu_scaled_reuse(const NvGpuState *target,const uint8_t *color,
                            const float *depth,unsigned stride)
{
    GpuScaledSnapshot *saved=gpu_scaled_find(color);
    if(!saved || !saved->color_valid || saved->format!=target->format ||
       (depth && (!saved->depth_valid || saved->depth!=depth || saved->stride!=stride)))
        return 0;
    for(unsigned y=0;y<target->height;y++) {
        size_t at=(size_t)y*target->width;
        if(memcmp(g.color_upload+at,color+(size_t)y*target->pitch,target->width*4u) ||
           (depth && memcmp(g.depth_upload+at,depth+(size_t)y*stride,target->width*4u)))
            return 0;
    }
    return 1;
}

static void gpu_scaled_unpack(const GpuSurface *pixels)
{
    unsigned scale=g.scaled.active, width=g.target.width*scale;
    GpuSurface *resolved=g.scaled.reused?gpu_scaled_find(g.host_color)->pixels:g.scaled.upload;
    /* Staging contains whole dirty rows, but only the dirty columns may have
     * changed. Keep the seeded values outside that rectangle. */
    for(unsigned y=g.dirty_top;y<g.dirty_bottom;y++) {
        size_t first=(size_t)y*scale*width+g.dirty_left*scale;
        size_t count=(size_t)(g.dirty_right-g.dirty_left)*scale;
        for(unsigned dy=0;dy<scale;dy++) {
            size_t row=first+(size_t)dy*width;
            memcpy(resolved+row,pixels+row,count*sizeof(*pixels));
        }
        size_t row=(size_t)nv2a_native_sample(y,scale)*width;
        for(unsigned x=g.dirty_left;x<g.dirty_right;x++) {
            size_t guest=(size_t)y*g.target.width+x;
            GpuSurface sample=pixels[row+nv2a_native_sample(x,scale)];
            g.color_upload[guest]=sample.color;
            memcpy(g.depth_upload+guest,&sample.depth_bits,4);
        }
    }
}

static void gpu_scaled_save(void)
{
    GpuScaledSnapshot *saved=gpu_scaled_find(g.host_color);
    GpuSurface *resolved=g.scaled.reused?saved->pixels:g.scaled.upload;
    gpu_scaled_invalidate(g.host_color,gpu_target_color_bytes(&g.target));
    if(!saved) {
        saved=&g.scaled.snapshots[g.scaled.snapshots[0].used<=g.scaled.snapshots[1].used?0:1];
    }
    /* A depth cache can be shared by backbuffers. Only the latest depth
     * snapshot for that physical cache may be a source for a later bind. */
    if(g.host_depth)
        gpu_scaled_invalidate(g.host_depth,gpu_target_depth_bytes(&g.target,g.depth_stride));
    GpuSurface *spare=saved->pixels;
    *saved=(GpuScaledSnapshot){
        .pixels=resolved, .color=gpu_canonical_host(g.host_color),
        .depth=g.host_depth, .stride=g.depth_stride, .pitch=g.target.pitch,
        .format=g.target.format, .color_valid=1, .depth_valid=g.host_depth!=NULL,
        .used=++g.scaled.tick,
    };
    if(!g.scaled.reused)g.scaled.upload=spare;
}

static int gpu_scaled_selftest(void)
{
    const unsigned width=NV2A_SCANOUT_WIDTH,height=NV2A_SCANOUT_HEIGHT;
    const unsigned scale=g.scaled.scale;
    const size_t pixels=(size_t)width*height;
    uint32_t *color=calloc(pixels,sizeof(*color));
    float *depth=malloc(pixels*sizeof(*depth));
    if(!color || !depth){free(color);free(depth);return 0;}
    for(size_t i=0;i<pixels;i++)depth[i]=25;

    NvGpuState state={0};
    state.width=width;state.height=height;state.pitch=width*4;state.bpp=4;state.format=0x128;
    state.control0=1u<<12;state.color_mask=0x01010101u;
    state.depth_test=state.depth_available=state.depth_mask=state.zpass_enable=1;
    state.depth_func=0x203;
    NvGpuTextureSource sources[4]={{0}};
    NvGpuTri triangle={0};
    /* This thin triangle crosses the finer grid but no native sample. */
    const float points[3][2]={{1.9f,1.9f},{2.4f,1.9f},{1.9f,3.4f}};
    for(unsigned i=0;i<3;i++) {
        triangle.v[i].pos[0]=points[i][0];
        triangle.v[i].pos[1]=points[i][1];
        triangle.v[i].pos[2]=5;
        triangle.v[i].pos[3]=1;
        triangle.iw[i]=1;
        for(unsigned j=0;j<4;j++)triangle.v[i].d0[j]=1;
    }
    triangle.minx=triangle.miny=1;triangle.maxx=3;triangle.maxy=4;
    triangle.area=(points[1][0]-points[0][0])*(points[2][1]-points[0][1]);
    triangle.inv_area=1.0f/triangle.area;
    int submitted=nv_gpu_draw(&state,&triangle,1,(uint8_t*)color,depth,width,sources);
    int ok=submitted==1;
    NvGpuCount count={0};
    if(ok)ok=nv_gpu_sync(&count)==1 && !count.zpass;
    GpuScaledSnapshot *saved=gpu_scaled_find(color);
    unsigned extra=0;
    for(unsigned y=2*scale;ok && saved && y<4*scale;y++) {
        for(unsigned x=2*scale;x<3*scale;x++) {
            size_t at=(size_t)y*width*scale+x;
            GpuSurface sample=saved->pixels[at];
            float z;
            memcpy(&z,&sample.depth_bits,sizeof z);
            if(sample.color && z==5)++extra;
            if((sample.color && z!=5) || (!sample.color && z!=25))ok=0;
        }
    }
    if(!extra)ok=0;
    for(unsigned y=2;y<4;y++)
        if(color[y*width+2] || depth[y*width+2]!=25)ok=0;
    if(!ok)fprintf(stderr,"[RENDER-SCALE] offscreen mismatch submitted=%d snapshot=%d extra_samples=%u native_zpass=%u\n",
                    submitted,saved!=NULL,extra,count.zpass);
    if(saved)saved->used=0;
    g.active=g.resident=0;
    g.scaled.active=0;
    g.host_color=NULL;g.host_depth=NULL;
    free(color);free(depth);
    return ok;
}

const uint32_t *nv_gpu_scaled_present(const uint8_t *address,
    const uint32_t *native_pixels,unsigned pitch,unsigned bpp,unsigned *scale)
{
    if(InterlockedCompareExchange(&s_startup,0,0)!=2 || !g.scaled.scale || s_capture || bpp!=4)
        return NULL;
    GpuScaledSnapshot *saved=gpu_scaled_find(address);
    if(!saved || !saved->color_valid || saved->pitch!=pitch)return NULL;
    unsigned factor=g.scaled.scale,width=NV2A_SCANOUT_WIDTH*factor;
    for(unsigned y=0;y<NV2A_SCANOUT_HEIGHT;y++) {
        size_t center=(size_t)nv2a_native_sample(y,factor)*width;
        for(unsigned x=0;x<NV2A_SCANOUT_WIDTH;x++) {
            uint32_t native=native_pixels[(size_t)y*NV2A_SCANOUT_WIDTH+x];
            int unchanged=saved->pixels[center+nv2a_native_sample(x,factor)].color==native;
            for(unsigned dy=0;dy<factor;dy++) {
                size_t left=(size_t)(y*factor+dy)*width+x*factor;
                for(unsigned dx=0;dx<factor;dx++)
                    g.scaled.present[left+dx]=unchanged?saved->pixels[left+dx].color:native;
            }
        }
    }
    *scale=factor;
    return g.scaled.present;
}
