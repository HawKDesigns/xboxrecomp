/* All states must keep or decrease depth; the frozen image is refreshed for
 * each eligible batch. This is a shading bound, not a visibility result. */
#ifndef NV2A_GPU_DEPTH_SEED_H
#define NV2A_GPU_DEPTH_SEED_H
static int nv_gpu_depth_seed_state_safe(const NvGpuState *s)
{
    if(!s->depth_available || !s->depth_mask)return 1;
    return s->depth_test && s->depth_func>=0x200u && s->depth_func<=0x203u;
}
static int nv_gpu_depth_seed_batch_safe(const NvGpuState *states,unsigned count)
{
    if(!count)return 0;
    for(unsigned i=0;i<count;i++)if(!nv_gpu_depth_seed_state_safe(states+i))return 0;
    return 1;
}
static int nv_gpu_depth_seed_batch_useful(const NvGpuState *states,unsigned count)
{
    for(unsigned i=0;i<count;i++)if(states[i].depth_available && states[i].depth_test &&
       (states[i].depth_func==0x201u || states[i].depth_func==0x203u))return 1;
    return 0;
}
/* A later depth writer with a non-monotone comparison cannot invalidate the
 * frozen depth for draws that precede it. Restrict seed reads to that prefix;
 * the remainder still executes the ordinary ordered pixel program. */
static unsigned nv_gpu_depth_seed_prefix(const NvGpuState *states,unsigned state_count,
    const NvGpuTri *triangles,unsigned triangle_count,int *useful)
{
    *useful=0;
    for(unsigned i=0;i<triangle_count;i++) {
        unsigned state_index=triangles[i].state_index;
        if(state_index>=state_count || !nv_gpu_depth_seed_state_safe(states+state_index))return i;
        const NvGpuState *s=states+state_index;
        if(s->depth_available && s->depth_test &&
           (s->depth_func==0x201u || s->depth_func==0x203u))*useful=1;
    }
    return triangle_count;
}
#endif
