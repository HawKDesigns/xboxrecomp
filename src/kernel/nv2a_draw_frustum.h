#ifndef NV2A_DRAW_FRUSTUM_H
#define NV2A_DRAW_FRUSTUM_H
#include <math.h>
#include <stdint.h>
#include "nv2a_vsh_interp.h"
#define NV_FRUSTUM_EYE 1u
#define NV_FRUSTUM_PLANES 127u
#define NV_FRUSTUM_UNKNOWN 128u
/* Active draw projection, never player/world-space heuristics. Screen-space
 * output and w reconstruct the same homogeneous planes as the exact clipper. */
typedef struct { double left,right,top,bottom,zmin,zmax;float eye;int zclip; } NvDrawFrustum;
static uint8_t nv_draw_outcode(const NvDrawFrustum *f,const Nv2aVshOutput *v)
{
    const float *p=v->pos;uint8_t code=0;
    for(unsigned k=0;k<4;k++)if(!isfinite(p[k]))return NV_FRUSTUM_UNKNOWN;
    if(p[3]==f->eye)return NV_FRUSTUM_UNKNOWN;
    if(p[3]<f->eye)code|=NV_FRUSTUM_EYE;
    if(p[3]>0) {
        if(p[0]<f->left)code|=2;if(p[0]>f->right)code|=4;
        if(p[1]<f->top)code|=8;if(p[1]>f->bottom)code|=16;
        if(f->zclip){if(p[2]<f->zmin)code|=32;if(p[2]>f->zmax)code|=64;}
    }else if(p[3]<0) {
        if(p[0]>f->left)code|=2;if(p[0]<f->right)code|=4;
        if(p[1]>f->top)code|=8;if(p[1]<f->bottom)code|=16;
        if(f->zclip){if(p[2]>f->zmin)code|=32;if(p[2]<f->zmax)code|=64;}
    }
    /* w=0 lies on all lateral/depth planes but outside the eye plane. */
    return code;
}
#endif
