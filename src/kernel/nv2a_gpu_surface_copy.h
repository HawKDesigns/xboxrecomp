#ifndef NV2A_GPU_SURFACE_COPY_H
#define NV2A_GPU_SURFACE_COPY_H
#include <stdint.h>
#include <stddef.h>
#include <string.h>
#if !defined(NV_GPU_SURFACE_NO_SSE2) && (defined(_M_X64) || defined(__SSE2__))
#include <emmintrin.h>
#define NV_GPU_SURFACE_SSE2 1
#else
#define NV_GPU_SURFACE_SSE2 0
#endif
/* A mapped staging row contains exact {ARGB, depth-bits} pairs. The source
 * is separate from the host destinations. Either destination may be NULL.
 * Preserve the original color-then-depth scalar order for overlapping host
 * ranges; guest surfaces are not assumed to have disjoint color/depth. */
static int nv_gpu_surface_copy(const void *source,void *color,void *depth,size_t count,int simd)
{
    const unsigned char *src=source;unsigned char *c=color,*z=depth;size_t at=0;int vectorized=0;
    if(!count || (!c && !z))return 0;
#if NV_GPU_SURFACE_SSE2
    uintptr_t a=(uintptr_t)c,b=(uintptr_t)z;
    int overlap=c && z && (a>b?a-b:b-a)<count*4;
    if(simd && !overlap && count>=4){
        for(;at+4<=count;at+=4){
            __m128i lo=_mm_loadu_si128((const __m128i*)(src+at*8));
            __m128i hi=_mm_loadu_si128((const __m128i*)(src+at*8+16));
            lo=_mm_shuffle_epi32(lo,_MM_SHUFFLE(3,1,2,0));
            hi=_mm_shuffle_epi32(hi,_MM_SHUFFLE(3,1,2,0));
            if(c)_mm_storeu_si128((__m128i*)(c+at*4),_mm_unpacklo_epi64(lo,hi));
            if(z)_mm_storeu_si128((__m128i*)(z+at*4),_mm_unpackhi_epi64(lo,hi));
        }
        vectorized=1;
    }
#else
    (void)simd;
#endif
    for(;at<count;at++){
        if(c)memcpy(c+at*4,src+at*8,4);
        if(z)memcpy(z+at*4,src+at*8+4,4);
    }
    return vectorized;
}
#endif
