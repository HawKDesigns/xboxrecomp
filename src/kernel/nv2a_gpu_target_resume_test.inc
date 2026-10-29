/* An independent CPU pixel reference checks resume depth, untouched colors,
 * fresh visibility counters and final publication of both target versions. */
static int gpu_resume_selftest(void)
{
    enum {side=16,pixels=256};
    uint32_t color[2][pixels],initial[2][pixels],expected[2][pixels];
    float depth[2][pixels],reference_depth[2][pixels];
    int ok=1;
    for(unsigned layout=0;layout<2 && ok;layout++) {
        NvGpuState s={0};s.width=s.height=side;s.pitch=64;s.bpp=4;
        s.format=layout?0x04040228u:0x128u;s.color_mask=0x01010101u;
        s.depth_available=s.depth_test=s.depth_mask=s.zpass_enable=1;s.depth_func=0x203;
        for(unsigned target=0;target<2;target++)for(unsigned y=0;y<side;y++)for(unsigned x=0;x<side;x++) {
            unsigned at=y*side+x;uint32_t v=0xFF010203u+at*0x102u;
            expected[target][at]=v;memcpy((uint8_t*)color[target]+gpu_color_at(&s,x,y),&v,4);
            depth[target][at]=reference_depth[target][at]=100;
        }
        memcpy(initial,color,sizeof color);
        NvGpuTri t[4]={{0}};unsigned destinations[4]={0,1,0,0};
        for(unsigned n=0;n<4;n++) {
            t[n].maxx=t[n].maxy=side;t[n].area=side*side;t[n].inv_area=1.0f/t[n].area;
            if(n>=2){t[n].minx=t[n].miny=n==2?4:6;t[n].maxx=t[n].maxy=n==2?12:14;}
            for(unsigned v=0;v<3;v++) {
                float x=v==1?side:0,y=v==2?side:0;if(n==3){x=side-x;y=side-y;}
                t[n].v[v].pos[0]=x;t[n].v[v].pos[1]=y;t[n].v[v].pos[2]=n==2?20:(n==3?5:10);
                t[n].v[v].pos[3]=t[n].iw[v]=1;t[n].v[v].d0[3]=1;t[n].v[v].d0[n%3]=1;
            }
            for(unsigned v=0;v<3;v++)t[n].inclusive[v]=nv2a_edge_inclusive(t[n].v[v].pos,t[n].v[(v+1)%3].pos,t[n].area);
        }
        NvGpuTextureSource sources[4]={{0}};NvGpuCount reference={0},actual={0};
        uint64_t before=g.resume.restores;
        for(unsigned n=0;n<4 && ok;n++) {
            unsigned d=destinations[n];
            for(unsigned y=0;y<side;y++)for(unsigned x=0;x<side;x++) {
                unsigned at=y*side+x;NvGpuPixel p=gpu_pixel(s,t[n],x,y,expected[d][at],reference_depth[d][at]);
                expected[d][at]=p.color;reference_depth[d][at]=p.depth;
                reference.depth_fail+=p.depth_fail;reference.pixels+=p.pixels;reference.zpass+=p.zpass;
                if((p.pixel_max&0xFFFFFFu)>(reference.pixel_max&0xFFFFFFu))reference.pixel_max=p.pixel_max;
            }
            int result=nv_gpu_draw(&s,t+n,1,(uint8_t*)color[d],depth[d],side,sources);
            if(result!=1){ok=result<0?-1:0;break;}
            if(n==2 && (g.resume.restores!=before+1 || g.pending_count!=2 || memcmp(initial,color,sizeof color)))ok=0;
        }
        /* Inspect the ENTIRE resumed GPU image, including pixels outside the
         * new draws' dirty bounds. Older queued CPU publication must not hide
         * a bad restore in those untouched GPU pixels or depth values. */
        if(ok){g.dirty_left=g.dirty_top=0;g.dirty_right=g.dirty_bottom=side;}
        if(ok && nv_gpu_sync(&actual)<0)ok=-1;
        if(ok)for(unsigned d=0;d<2;d++)for(unsigned y=0;y<side;y++)for(unsigned x=0;x<side;x++) {
            unsigned at=y*side+x;uint32_t v;memcpy(&v,(uint8_t*)color[d]+gpu_color_at(&s,x,y),4);
            if(v!=expected[d][at] || depth[d][at]!=reference_depth[d][at])ok=0;
        }
        gpu_reference_counts(&reference);
        if(ok && (reference.depth_fail!=actual.depth_fail || reference.pixels!=actual.pixels ||
           reference.zpass!=actual.zpass || reference.pixel_max!=actual.pixel_max))ok=0;
    }
    g.pending_count=0;g.active=g.resident=0;g.queued_count=g.state_count=g.queued_refs=0;
    g.host_color=NULL;g.host_depth=NULL;g.target_written=0;memset(&g.completed,0,sizeof g.completed);
    for(unsigned i=0;i<128;i++)g.cache[i].queued=0;
    return ok;
}
