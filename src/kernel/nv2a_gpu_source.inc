GPU_FN NvGpuSource gpu_source_color(NvGpuState state,NvGpuTri tri,float l0,float l1,float l2)
{
#if NV_GPU_LITERAL_CLASS
    /* Every admitted family has the same exact two active texture modes.
     * Keep material colors/coordinates dynamic while exposing modes to FXC. */
    state.rc.stage_program=0x21u;
#endif
    NvGpuSource source;source.argb=source.keep=0;
    uint32_t argb = 0;
    if ((state.color_mask & 0x01010101u) || state.alpha_test) {
        GPU_PRECISE float col[4];
        for (int k = 0; k < 4; ++k)
            col[k] = l0*tri.v[0].d0[k]+l1*tri.v[1].d0[k]+l2*tri.v[2].d0[k];
        GPU_PRECISE float pw = l0*tri.iw[0]+l1*tri.iw[1]+l2*tri.iw[2];
        if (tri.use_rc) {
#ifdef NV_GPU_SIMPLE_MATERIAL
            GPU_PRECISE float fog[4],shade[4],tc[4],texel[4];
            for(int k=0;k<3;k++)fog[k]=tri.fogc[k];
            fog[3]=l0*tri.vfog[0]+l1*tri.vfog[1]+l2*tri.vfog[2];
            for(int k=0;k<4;k++)
                tc[k]=(l0*tri.stc[0][0][k]+l1*tri.stc[1][0][k]+l2*tri.stc[2][0][k])/pw;
            float q=tc[3]!=0.0f?tc[3]:1.0f;
            gpu_sample(state,0,0,tc[0]/q,tc[1]/q,texel);
            gpu_rc_simple(state.rc,col,fog,texel,shade);
#else
            GPU_PRECISE float d1[4], fog[4], t[4][4], shade[4], tc[4];int keep = 1;
            for (int k = 0; k < 4; ++k)
                d1[k] = l0*tri.v[0].d1[k]+l1*tri.v[1].d1[k]+l2*tri.v[2].d1[k];
            for (int k = 0; k < 3; ++k) fog[k] = tri.fogc[k];
            fog[3] = l0*tri.vfog[0]+l1*tri.vfog[1]+l2*tri.vfog[2];
#if NV_GPU_STAGE_LIVENESS
#if NV_GPU_LITERAL_CLASS
            uint32_t live_stages=15u;
#else
            uint32_t live_stages=GPU_TEXTURE_STAGE_MASK(tri);
#endif
#endif
#if NV_GPU_LITERAL_CLASS
            GPU_UNROLL
#endif
            for (int st = 0; st < 4 && keep; ++st) {
#if NV_GPU_STAGE_LIVENESS
                uint32_t mode=(state.rc.stage_program>>(st*5))&31u;
                /* OFF has a constant result. An unreferenced register may
                 * remain zero, but CLIP always executes for its side effect. */
                if(mode==0 || (!(live_stages&(1u<<st)) && mode!=5u)){
                    for(int k=0;k<4;k++)t[st][k]=0;
                    if(mode==0)t[st][3]=1;
                    continue;
                }
#endif
                for (int k = 0; k < 4; ++k)
                    tc[k] = (l0*tri.stc[0][st][k]+l1*tri.stc[1][st][k]+l2*tri.stc[2][st][k])/pw;
                keep = gpu_stage(state,st,tc,t[st]);
            }
            if (!keep) return source;
#if NV_GPU_LITERAL_CLASS
#if NV_GPU_COMPACT_MATERIALS
            gpu_rc_compact(state.rc,col,d1,fog,t,shade);
#else
            gpu_rc_literal(state.rc,col,d1,fog,t,shade);
#endif
#else
            gpu_rc_eval(state.rc,col,d1,fog,t,shade);
#endif
#endif
            if (state.alpha_test && !gpu_compare((float)(int)(shade[3]*255.0f+0.5f),
                                                (float)(state.alpha_ref & 255),state.alpha_func)) return source;
            argb = gpu_pack(shade);
        } else {
            argb = gpu_pack(col);
            if (tri.textured) {
                float u = (l0*tri.uv[0][0]+l1*tri.uv[1][0]+l2*tri.uv[2][0])/pw;
                float v = (l0*tri.uv[0][1]+l1*tri.uv[1][1]+l2*tri.uv[2][1])/pw;
                float sample_value[4];gpu_unpack(gpu_texel(state,0,0,(int)GPU_FLOOR(u),(int)GPU_FLOOR(v)),sample_value);
                for (int k = 0; k < 4; ++k) sample_value[k] *= col[k];
                argb = gpu_pack(sample_value);
            }
            if (state.alpha_test && !gpu_compare((float)(argb >> 24),
                                               (float)(state.alpha_ref & 255),state.alpha_func)) return source;
        }
    }
    source.argb=argb;source.keep=1;return source;
}
