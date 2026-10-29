/* Included after nv2a_gpu_pixel.inc and gpu_load_texel(). Scalar C/HLSL. */
GPU_FN uint32_t gpu_pack(float c[4])
{
    uint32_t b[4];
    for (int k = 0; k < 4; ++k)
        { GPU_PRECISE float scaled=gpu_clampf(c[k], 0.0f, 1.0f)*255.0f+0.5f; b[k]=(uint32_t)scaled; }
    return (b[3] << 24) | (b[0] << 16) | (b[1] << 8) | b[2];
}
GPU_FN int gpu_wrap(int c, uint32_t size, uint32_t mode)
{
    int n = (int)size;
    if (mode == 1) {
        /* Unsigned arithmetic defines negative wrapping on HLSL as well as C.
         * Power-of-two Xbox textures also avoid integer division entirely. */
        if((size & (size-1u))==0)return (int)((uint32_t)c & (size-1u));
        uint32_t magnitude=c<0?0u-(uint32_t)c:(uint32_t)c;
        uint32_t remainder=magnitude%size;
        return c<0 && remainder?(int)(size-remainder):(int)remainder;
    }
    return c < 0 ? 0 : (c >= n ? n - 1 : c);
}
GPU_FN uint32_t gpu_texel(NvGpuState state, int st, int face, int x, int y)
{
    NvGpuTexture t = state.tex[st];
    if (!t.valid) return 0xFFFFFFFFu;
    return gpu_load_texel(t, st, face, gpu_wrap(x, t.width, t.addr_u), gpu_wrap(y, t.height, t.addr_v));
}
#ifndef NV_GPU_QUAD_SAMPLING
#define NV_GPU_QUAD_SAMPLING 0
#endif
GPU_FN void gpu_sample(NvGpuState state, int st, int face, float u, float v, GPU_OUT4(result))
{
    NvGpuTexture t = state.tex[st];
    if (t.normalized) { u *= (float)t.width; v *= (float)t.height; }
    uint32_t mag = (t.filter >> 24) & 15, minfilter = (t.filter >> 16) & 255;
    if (mag == 2 || minfilter == 2 || minfilter == 4 || minfilter == 6) {
        GPU_PRECISE float fu = u - 0.5f, fv = v - 0.5f;
        int iu = (int)GPU_FLOOR(fu), iv = (int)GPU_FLOOR(fv);
        GPU_PRECISE float wu = fu - (float)iu, wv = fv - (float)iv;
        float c[4][4];
#if NV_GPU_QUAD_SAMPLING
        /* Four explicit loads share the two wrapped coordinates per axis.
         * Keep texel order and all filtering arithmetic exactly as before. */
        if(t.valid){
            int x0=gpu_wrap(iu,t.width,t.addr_u),x1=gpu_wrap(iu+1,t.width,t.addr_u);
            int y0=gpu_wrap(iv,t.height,t.addr_v),y1=gpu_wrap(iv+1,t.height,t.addr_v);
            gpu_unpack(gpu_load_texel(t,st,face,x0,y0),c[0]);
            gpu_unpack(gpu_load_texel(t,st,face,x1,y0),c[1]);
            gpu_unpack(gpu_load_texel(t,st,face,x0,y1),c[2]);
            gpu_unpack(gpu_load_texel(t,st,face,x1,y1),c[3]);
        }else{
            gpu_unpack(0xFFFFFFFFu,c[0]);gpu_unpack(0xFFFFFFFFu,c[1]);
            gpu_unpack(0xFFFFFFFFu,c[2]);gpu_unpack(0xFFFFFFFFu,c[3]);
        }
#else
        for (int k = 0; k < 4; ++k)
            gpu_unpack(gpu_texel(state, st, face, iu + (k & 1), iv + (k >> 1)), c[k]);
#endif
        for (int j = 0; j < 4; ++j)
            result[j] = (c[0][j] * (1.0f - wu) + c[1][j] * wu) * (1.0f - wv)
                      + (c[2][j] * (1.0f - wu) + c[3][j] * wu) * wv;
    } else gpu_unpack(gpu_texel(state, st, face, (int)GPU_FLOOR(u), (int)GPU_FLOOR(v)), result);
}
GPU_FN int gpu_stage(NvGpuState state, int st, float c[4], GPU_OUT4(result))
{
    NvGpuTexture t = state.tex[st];
    uint32_t mode = (state.rc.stage_program >> (st * 5)) & 31;
    float q = c[3] != 0.0f ? c[3] : 1.0f;
    for (int k = 0; k < 4; ++k) result[k] = 0;
    if (mode == 0) { result[3] = 1; return 1; }
    if (mode == 1 || mode == 2) { gpu_sample(state, st, 0, c[0]/q, c[1]/q, result); return 1; }
    if (mode == 3) {
        if (!t.cube) { gpu_sample(state, st, 0, c[0], c[1], result); return 1; }
        float x = c[0], y = c[1], z = c[2];
        float ax = x < 0 ? -x : x, ay = y < 0 ? -y : y, az = z < 0 ? -z : z;
        float ma, sc, tc; int face;
        if (ax >= ay && ax >= az) { face = x > 0 ? 0 : 1; ma = ax; sc = x > 0 ? -z : z; tc = -y; }
        else if (ay >= az) { face = y > 0 ? 2 : 3; ma = ay; sc = x; tc = y > 0 ? z : -z; }
        else { face = z > 0 ? 4 : 5; ma = az; sc = z > 0 ? x : -x; tc = -y; }
        if (ma == 0) ma = 1;
        gpu_sample(state, st, face, (sc/ma+1.0f)*0.5f, (tc/ma+1.0f)*0.5f, result);return 1;
    }
    if (mode == 4) { for (int k = 0; k < 4; ++k) result[k] = gpu_clampf(c[k],0,1);return 1; }
    if (mode == 5) {
        for (int k = 0; k < 4; ++k) {
            int ge = (state.clip_plane_mode >> (st * 4 + k)) & 1;
            if (ge ? c[k] >= 0.0f : c[k] < 0.0f) return 0;
        }
    }
    return 1;
}
GPU_FN int gpu_compare(float x, float y, uint32_t func)
{
    if (func == 0x200) return 0;
    if (func == 0x201) return x < y;
    if (func == 0x202) return x == y;
    if (func == 0x203) return x <= y;
    if (func == 0x204) return x > y;
    if (func == 0x205) return x != y;
    if (func == 0x206) return x >= y;
    return 1;
}
GPU_FN float gpu_depth(NvGpuState state, float z)
{
    if (!GPU_ISFINITE(z)) return z;
    int z16 = ((state.format >> 4) & 15) == 1;
    if (state.control0 & (1u << 12)) {
        if (z16) return z;
        if (z <= 0) return 0;
        return GPU_ASFLOAT(GPU_ASUINT(z) & ~127u);
    }
    if (z <= 0) return 0;
    float maximum = z16 ? 65535.0f : 16777215.0f;
    if (z >= maximum) return maximum;
    /* Above 2^23 float already has integral precision. Adding 0.5 in float
     * there would round odd integers upward, unlike the CPU's double floor. */
    return z >= 8388608.0f ? z : GPU_FLOOR(z + 0.5f);
}
GPU_FN void gpu_blend_factor(uint32_t factor, uint32_t constant_color,
                             float s[4], float d[4], GPU_OUT4(result))
{
    float constant_value[4];gpu_unpack(constant_color,constant_value);
    for (int k = 0; k < 4; ++k) {
        float v = 0;
        if (factor == 1) v = 1;
        else if (factor == 0x300) v = s[k];
        else if (factor == 0x301) v = 1-s[k];
        else if (factor == 0x302) v = s[3];
        else if (factor == 0x303) v = 1-s[3];
        else if (factor == 0x304) v = d[3];
        else if (factor == 0x305) v = 1-d[3];
        else if (factor == 0x306) v = d[k];
        else if (factor == 0x307) v = 1-d[k];
        else if (factor == 0x308) v = k == 3 ? 1 : (s[3] < 1-d[3] ? s[3] : 1-d[3]);
        else if (factor == 0x8001) v = constant_value[k];
        else if (factor == 0x8002) v = 1-constant_value[k];
        else if (factor == 0x8003) v = constant_value[3];
        else if (factor == 0x8004) v = 1-constant_value[3];
        else if (factor != 0) v = 1;
        result[k] = v;
    }
}
GPU_FN uint32_t gpu_blend(NvGpuState state, uint32_t color, uint32_t previous)
{
    if (state.blend_enable && !(state.blend_sfactor == 1 && state.blend_dfactor == 0)) {
        GPU_PRECISE float s[4], d[4], sf[4], df[4], result[4];gpu_unpack(color,s);gpu_unpack(previous,d);
        gpu_blend_factor(state.blend_sfactor,state.blend_color,s,d,sf);
        gpu_blend_factor(state.blend_dfactor,state.blend_color,s,d,df);
        for (int k = 0; k < 4; ++k) {
            float a = s[k]*sf[k], b = d[k]*df[k];
            if (state.blend_equation == 0x800A) result[k] = a-b;
            else if (state.blend_equation == 0x800B) result[k] = b-a;
            else if (state.blend_equation == 0x8007) result[k] = s[k] < d[k] ? s[k] : d[k];
            else if (state.blend_equation == 0x8008) result[k] = s[k] > d[k] ? s[k] : d[k];
            else result[k] = a+b;
        }
        color = gpu_pack(result);
    }
    uint32_t keep = 0;
    if (!(state.color_mask & 0x01000000u)) keep |= 0xFF000000u;
    if (!(state.color_mask & 0x00010000u)) keep |= 0x00FF0000u;
    if (!(state.color_mask & 0x00000100u)) keep |= 0x0000FF00u;
    if (!(state.color_mask & 0x00000001u)) keep |= 0x000000FFu;
    color = (color & ~keep) | (previous & keep);
    if (state.bpp == 2)
        color = 0xFF000000u | (color & 0xF80000u) | (color & 0x00FC00u) | (color & 0x0000F8u);
    return color;
}
GPU_FN int gpu_edge(float edge, float area, int inclusive)
{
    return (area > 0 ? edge > 0 : edge < 0) || (edge == 0 && inclusive);
}
/* Source shading is independent of the destination. Native graphics does
 * it before entering ordered storage, leaving only depth/blend/commit inside. */
#ifndef NV_GPU_HLSL
typedef struct NvGpuBary NvGpuBary;
typedef struct NvGpuSource NvGpuSource;
#endif
struct NvGpuBary { float l0,l1,l2,depth;uint32_t covered; };
struct NvGpuSource { uint32_t argb,keep; };
GPU_FN NvGpuBary gpu_bary(NvGpuState state,NvGpuTri tri,int x,int y)
{
    NvGpuBary bary;bary.l0=bary.l1=bary.l2=bary.depth=0;bary.covered=0;
    if (x < tri.minx || x >= tri.maxx || y < tri.miny || y >= tri.maxy) return bary;
    GPU_PRECISE float px = (float)x+0.5f, py = (float)y+0.5f;
    GPU_PRECISE float w0 = (tri.v[1].pos[0]-tri.v[0].pos[0])*(py-tri.v[0].pos[1])
                         -(tri.v[1].pos[1]-tri.v[0].pos[1])*(px-tri.v[0].pos[0]);
    GPU_PRECISE float w1 = (tri.v[2].pos[0]-tri.v[1].pos[0])*(py-tri.v[1].pos[1])
                         -(tri.v[2].pos[1]-tri.v[1].pos[1])*(px-tri.v[1].pos[0]);
    GPU_PRECISE float w2 = (tri.v[0].pos[0]-tri.v[2].pos[0])*(py-tri.v[2].pos[1])
                         -(tri.v[0].pos[1]-tri.v[2].pos[1])*(px-tri.v[2].pos[0]);
    if (!(gpu_edge(w0,tri.area,tri.inclusive[0]) && gpu_edge(w1,tri.area,tri.inclusive[1]) &&
          gpu_edge(w2,tri.area,tri.inclusive[2]))) return bary;
    GPU_PRECISE float l0 = w1*tri.inv_area, l1 = w2*tri.inv_area, l2 = w0*tri.inv_area;
    GPU_PRECISE float z = tri.v[0].pos[2]+l1*(tri.v[1].pos[2]-tri.v[0].pos[2])
                                             +l2*(tri.v[2].pos[2]-tri.v[0].pos[2]);
    bary.l0=l0;bary.l1=l1;bary.l2=l2;bary.depth=gpu_depth(state,z);bary.covered=1;
    return bary;

}
/* The immutable batch-start depth is only a conservative rejection bound.
 * The short ROV transaction still tests current depth and owns all writes. */
GPU_FN int gpu_seed_source_visible(NvGpuState state,NvGpuBary bary,float seed_depth)
{
    if(!bary.covered)return 0;
    if(state.depth_available && state.depth_test &&
       (state.depth_func==0x201u || state.depth_func==0x203u))
        return gpu_compare(bary.depth,seed_depth,state.depth_func);
    return 1;
}
#include "nv2a_gpu_source.inc"
GPU_FN NvGpuPixel gpu_commit(NvGpuState state,NvGpuBary bary,NvGpuSource source,
                              uint32_t previous,float stored_depth)
{
    NvGpuPixel result;result.color=previous;result.depth=stored_depth;
    result.depth_fail=result.pixels=result.zpass=result.pixel_max=0;
    if(!bary.covered)return result;
    if(state.depth_available && state.depth_test && !gpu_compare(bary.depth,stored_depth,state.depth_func)) {
        result.depth_fail=1;return result;
    }
    if(!source.keep)return result;
    if(state.depth_available && state.depth_mask)result.depth=bary.depth;
    if(state.zpass_enable)result.zpass=1;
    if(!(state.color_mask & 0x01010101u))return result;
    result.pixels=1;result.pixel_max=source.argb;
    result.color=gpu_blend(state,source.argb,previous);return result;
}
GPU_FN NvGpuPixel gpu_pixel(NvGpuState state,NvGpuTri tri,int x,int y,
                             uint32_t previous,float stored_depth)
{
    NvGpuBary bary=gpu_bary(state,tri,x,y);NvGpuSource source;source.argb=source.keep=0;
    /* Compute/reference keep early depth rejection ahead of texture work. */
    if(bary.covered && !(state.depth_available && state.depth_test &&
                        !gpu_compare(bary.depth,stored_depth,state.depth_func)))
        source=gpu_source_color(state,tri,bary.l0,bary.l1,bary.l2);
    return gpu_commit(state,bary,source,previous,stored_depth);
}
