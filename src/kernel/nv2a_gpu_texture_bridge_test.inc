/* Private producer -> GPU texture -> consumer comparison. Source host pixels
 * deliberately stay at their original value until the normal final sync. */
static int gpu_bridge_test_decode(void *opaque,uint32_t face,uint32_t x,uint32_t y,uint32_t *v)
{
    (void)face;const NvGpuTextureSource *source=opaque;NvGpuState surface={0};
    surface.width=surface.height=16;surface.pitch=64;surface.bpp=4;
    surface.format=source->gpu_argb_layout==2?0x04040228u:0x128u;
    memcpy(v,source->raw+gpu_color_at(&surface,x,y),4);return 1;
}
static int gpu_bridge_selftest(void)
{
    enum {side=16,pixels=256};
    uint32_t source[pixels],initial[pixels],expected_source[pixels],target[pixels],expected_target[pixels];
    float source_depth[pixels],expected_source_depth[pixels],depth[pixels],expected_depth[pixels];
    int ok=1;
    int resume_enabled=g.resume.enabled,resume_testing=g.resume.testing,resume_retire=g.resume.retire_pixels;
    for(unsigned scenario=0;scenario<(g.completion.feedback_enabled?3u:2u) && ok;scenario++) {
        unsigned layout=scenario==1?2:1;
        int feedback_same_target=scenario==2;
        if(feedback_same_target && g.completion.feedback_enabled){
            g.resume.enabled=g.resume.testing=g.resume.retire_pixels=1;
        }
        NvGpuState s={0};NvGpuTextureSource textures[4]={{0}};NvGpuTri tri[2]={{0}};
        s.width=s.height=side;s.pitch=side*4;s.bpp=4;s.format=layout==2?0x04040228u:0x128u;
        s.color_mask=0x01010101u;s.depth_available=s.depth_test=s.depth_mask=s.zpass_enable=1;s.depth_func=0x203;
        for(unsigned i=0;i<pixels;i++) {
            uint32_t color=0xFF000000u|((i*7)&255u)|(((i*3)&255u)<<8)|(((i*11)&255u)<<16);
            memcpy((uint8_t*)source+gpu_color_at(&s,i%side,i/side),&color,4);
            expected_source[i]=color;source_depth[i]=expected_source_depth[i]=100;
            target[i]=expected_target[i]=0xFF345678u;depth[i]=expected_depth[i]=100;
        }
        memcpy(initial,source,sizeof source);
        for(unsigned n=0;n<2;n++) {
            NvGpuTri *t=tri+n;t->maxx=t->maxy=side;t->area=side*side;t->inv_area=1.0f/t->area;
            for(unsigned v=0;v<3;v++) {
                float x=v==1?side:0,y=v==2?side:0;
                if(n){x=side-x;y=side-y;}
                t->v[v].pos[0]=x;t->v[v].pos[1]=y;t->v[v].pos[2]=10;t->v[v].pos[3]=t->iw[v]=1;
                for(unsigned c=0;c<4;c++)t->v[v].d0[c]=1;
                t->uv[v][0]=x;t->uv[v][1]=y;
            }
            for(unsigned v=0;v<3;v++)t->inclusive[v]=nv2a_edge_inclusive(t->v[v].pos,t->v[(v+1)%3].pos,t->area);
        }
        NvGpuTri producer=tri[0];for(unsigned v=0;v<3;v++) {
            producer.v[v].d0[0]=0.25f;producer.v[v].d0[1]=0.5f;producer.v[v].d0[2]=0.75f;
        }
        NvGpuCount reference={0},count={0};
        for(unsigned y=0;y<side;y++)for(unsigned x=0;x<side;x++) {
            unsigned at=y*side+x;NvGpuPixel pixel=gpu_pixel(s,producer,x,y,expected_source[at],expected_source_depth[at]);
            expected_source[at]=pixel.color;expected_source_depth[at]=pixel.depth;
            reference.zpass+=pixel.zpass;reference.pixels+=pixel.pixels;reference.depth_fail+=pixel.depth_fail;
            if((pixel.pixel_max&0xFFFFFFu)>(reference.pixel_max&0xFFFFFFu))reference.pixel_max=pixel.pixel_max;
        }
        if(nv_gpu_draw(&s,&producer,1,(uint8_t*)source,source_depth,side,textures)!=1){ok=-1;break;}
        /* Simulate a completed guest fence while the exact source remains on
         * GPU. The consumer must bridge its immutable snapshot without
         * publishing the source's pixels early. */
        if(g.completion.feedback_enabled)g.completion.unpublished=1;
        if(feedback_same_target) {
            memcpy(expected_target,expected_source,sizeof expected_target);
            memcpy(expected_depth,expected_source_depth,sizeof expected_depth);
        }
        s.format=0x128u;s.tex[0].width=s.tex[0].height=side;s.tex[0].valid=1;
        s.tex[0].format=layout==2?0x06:0x12;s.tex[0].addr_u=s.tex[0].addr_v=1;
        textures[0].raw=(uint8_t*)source;textures[0].bytes=sizeof source;
        textures[0].opaque=&textures[0];textures[0].decode=gpu_bridge_test_decode;textures[0].gpu_argb_layout=layout;
        tri[0].textured=tri[1].textured=1;
        if(nv_gpu_draw(&s,tri,2,feedback_same_target?(uint8_t*)source:(uint8_t*)target,
                       feedback_same_target?source_depth:depth,side,textures)!=1){ok=-1;break;}
        if(!g.pending_count || !g.bound[0] || !g.bound[0]->gpu_serial || memcmp(source,initial,sizeof source)) {
            fprintf(stderr,"[GPU-TEXTURE-BRIDGE] scenario=%u early pending=%u serial=%u host_changed=%u\n",
                scenario,g.pending_count,g.bound[0]?g.bound[0]->gpu_serial:0,
                (unsigned)(memcmp(source,initial,sizeof source)!=0));
            fprintf(stderr,"[GPU-TEXTURE-BRIDGE] resume checked=%llu changed=%llu conflicts=%llu restores=%llu readbacks=%llu feedback=%llu\n",
                (unsigned long long)g.resume.checks,(unsigned long long)g.resume.changed,
                (unsigned long long)g.resume.conflicts,(unsigned long long)g.resume.restores,
                (unsigned long long)g.readbacks,(unsigned long long)g.completion.feedback_snapshots);ok=0;break;
        }
        for(unsigned n=0;n<2;n++)for(unsigned y=0;y<side;y++)for(unsigned x=0;x<side;x++) {
            unsigned at=y*side+x;NvGpuBary b=gpu_bary(s,tri[n],x,y);
            if(b.covered && gpu_compare(b.depth,expected_depth[at],s.depth_func)) {
                expected_target[at]=expected_source[at];expected_depth[at]=b.depth;
                ++reference.pixels;++reference.zpass;
                if((expected_source[at]&0xFFFFFFu)>(reference.pixel_max&0xFFFFFFu))reference.pixel_max=expected_source[at];
            }
        }
        if(nv_gpu_sync(&count)<0){ok=-1;break;}
        for(unsigned y=0;y<side;y++)for(unsigned x=0;x<side;x++) {
            unsigned at=y*side+x;NvGpuState original=s;original.format=layout==2?0x04040228u:0x128u;
            uint32_t got;memcpy(&got,(uint8_t*)source+gpu_color_at(&original,x,y),4);
            if(got!=(feedback_same_target?expected_target[at]:expected_source[at]) ||
               source_depth[at]!=(feedback_same_target?expected_depth[at]:expected_source_depth[at]) ||
               (!feedback_same_target && (target[at]!=expected_target[at] || depth[at]!=expected_depth[at]))) {
                fprintf(stderr,"[GPU-TEXTURE-BRIDGE] scenario=%u pixel=%u got=%08X expected=%08X depth=%f expected_depth=%f\n",
                    scenario,at,got,feedback_same_target?expected_target[at]:expected_source[at],
                    source_depth[at],feedback_same_target?expected_depth[at]:expected_source_depth[at]);ok=0;
            }
        }
        gpu_reference_counts(&reference);
        if(count.zpass!=reference.zpass || count.pixels!=reference.pixels || count.depth_fail!=reference.depth_fail || count.pixel_max!=reference.pixel_max)ok=0;
    }
    g.resume.enabled=resume_enabled;g.resume.testing=resume_testing;g.resume.retire_pixels=resume_retire;
    g.pending_count=0;g.active=g.resident=0;g.queued_count=g.state_count=g.queued_refs=0;
    g.host_color=NULL;g.host_depth=NULL;memset(&g.completed,0,sizeof g.completed);
    for(unsigned i=0;i<128;i++) {
        g.cache[i].queued=0;
        if(g.cache[i].gpu_serial)gpu_texture_release(g.cache+i);
    }
    return ok;
}
