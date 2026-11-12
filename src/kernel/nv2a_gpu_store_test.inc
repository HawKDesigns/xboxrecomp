/* Exact generated sequence: real writers surround unchanged/zpass-only,
 * masked, failed-depth and identity-blend invocations. General and simple
 * programs must publish identical bits and visibility after the final writer.
 * No guest assets or runtime readback are added by this startup-only test. */
static int gpu_store_selftest(void)
{
    enum {side=16,pixels=side*side,draws=13};
    uint32_t actual[pixels],expected[pixels];float depth[pixels],reference_depth[pixels];
    NvGpuState states[draws];NvGpuTri tri[draws];NvGpuTextureSource sources[4];
    ID3D11PixelShader *simple=g.simple_pixel_shader;int ok=1;
    memset(sources,0,sizeof sources);
    for(unsigned mode=0;mode<(simple?2u:1u) && ok>0;mode++){
        g.simple_pixel_shader=mode?simple:NULL;
        for(unsigned at=0;at<pixels;at++){
            actual[at]=expected[at]=0xFF112233u;depth[at]=reference_depth[at]=100;
        }
        memset(states,0,sizeof states);memset(tri,0,sizeof tri);
        for(unsigned i=0;i<draws;i++){
            NvGpuState *s=states+i;NvGpuTri *t=tri+i;
            s->width=s->height=side;s->pitch=side*4;s->bpp=4;s->format=0x128;
            s->depth_available=s->depth_test=s->depth_mask=s->zpass_enable=1;
            s->depth_func=0x203;s->color_mask=0x01010101u;
            t->maxx=t->maxy=side;t->area=side*side;t->inv_area=1.0f/(side*side);
            t->v[1].pos[0]=t->v[2].pos[1]=side;
            float z=i<4?70:(i<6?60:(i<9?50:(i<12?40:30)));
            if(i==2 || i==11)z=90;
            for(unsigned v=0;v<3;v++){
                t->v[v].pos[2]=z;t->v[v].pos[3]=t->iw[v]=1;
                t->v[v].d0[0]=i<6?1:0;t->v[v].d0[1]=i==12?1:0;
                t->v[v].d0[2]=i>=6 && i<12?1:0;t->v[v].d0[3]=1;
                t->inclusive[v]=nv2a_edge_inclusive(t->v[v].pos,t->v[(v+1)%3].pos,t->area);
            }
            if(i==3 || i==10){s->depth_mask=0;s->color_mask=0;}
            if(i==4)s->color_mask=0;
            if(i==5){s->alpha_test=1;s->alpha_func=0x200;}
            if(i==8){s->depth_test=s->depth_mask=0;s->blend_enable=1;s->blend_dfactor=1;s->blend_equation=0x8006;}
            if(i==9)s->color_mask=1;
            if(nv_gpu_draw(s,t,1,(uint8_t*)actual,depth,side,sources)!=1){ok=-1;break;}
        }
        NvGpuCount count={0},reference={0};unsigned changed=0,unchanged=0,unchanged_zpass=0;
        if(ok>0)for(unsigned i=0;i<draws;i++)for(unsigned y=0;y<side;y++)for(unsigned x=0;x<side;x++){
            unsigned at=y*side+x;uint32_t before=GPU_ASUINT(reference_depth[at]);
            NvGpuPixel p=gpu_pixel(states[i],tri[i],x,y,expected[at],reference_depth[at]);
            if(p.color!=expected[at] || GPU_ASUINT(p.depth)!=before)++changed;
            else {++unchanged;if(p.zpass)++unchanged_zpass;}
            expected[at]=p.color;reference_depth[at]=p.depth;
            reference.depth_fail+=p.depth_fail;reference.pixels+=p.pixels;reference.zpass+=p.zpass;
        }
        if(ok>0 && nv_gpu_sync(&count)<0)ok=-1;
        if(ok>0){
            gpu_reference_counts(&reference);
            ok=!memcmp(actual,expected,sizeof actual) && !memcmp(depth,reference_depth,sizeof depth) &&
                count.depth_fail==reference.depth_fail && count.pixels==reference.pixels && count.zpass==reference.zpass &&
                changed && unchanged && unchanged_zpass;
            if(!ok)fprintf(stderr,"[GPU-STORE] ordered sequence mismatch mode=%u zpass=%u/%u\n",mode,count.zpass,reference.zpass);
        }
        g.active=g.resident=0;g.queued_count=g.state_count=g.queued_refs=0;
        for(unsigned i=0;i<128;i++)g.cache[i].queued=0;
        g.host_color=NULL;g.host_depth=NULL;memset(&g.completed,0,sizeof g.completed);
    }
    g.simple_pixel_shader=simple;return ok;
}
