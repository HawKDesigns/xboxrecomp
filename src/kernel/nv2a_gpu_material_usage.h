#ifndef NV2A_GPU_MATERIAL_USAGE_H
#define NV2A_GPU_MATERIAL_USAGE_H
/* Conservative read set, not algebraic dead-code elimination. Any texture
 * register referenced anywhere is live, even if a prior stage overwrites it.
 * r0.a is initialized from T0 before the first combiner. Clip stages have
 * independent rejection side effects and must always execute. */
static uint32_t nv_gpu_texture_word_reads(uint32_t word)
{
    uint32_t mask=0;
    for(unsigned byte=0;byte<4;byte++){
        unsigned reg=(word>>(byte*8))&15u;
        if(reg>=8 && reg<=11)mask|=1u<<(reg-8);
    }
    return mask;
}
static uint32_t nv_gpu_texture_reads(const Nv2aCombiner *rc)
{
    uint32_t mask=(rc->stage_program&31u)?1u:0u;
    unsigned n=rc->control&255u;if(n>8)n=8;
    for(unsigned stage=0;stage<n;stage++)
        mask|=nv_gpu_texture_word_reads(rc->color_icw[stage])|
              nv_gpu_texture_word_reads(rc->alpha_icw[stage]);
    if(rc->final0 || rc->final1)
        mask|=nv_gpu_texture_word_reads(rc->final0)|
              nv_gpu_texture_word_reads(rc->final1&0xFFFFFF00u);
    return mask;
}
static uint32_t nv_gpu_texture_enabled(const Nv2aCombiner *rc)
{
    uint32_t mask=0;for(unsigned st=0;st<4;st++)if((rc->stage_program>>(st*5))&31u)mask|=1u<<st;return mask;
}
static uint32_t nv_gpu_texture_clip(const Nv2aCombiner *rc)
{
    uint32_t mask=0;for(unsigned st=0;st<4;st++)if(((rc->stage_program>>(st*5))&31u)==5u)mask|=1u<<st;return mask;
}
#endif
