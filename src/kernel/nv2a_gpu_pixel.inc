/* Scalar pixel program shared by HLSL and the portable reference fixture.
 * Combiner arithmetic follows the working nv2a_combiner.c evaluator. */
#ifdef NV_GPU_HLSL
#define GPU_FN
#define GPU_UNROLL [unroll]
#define GPU_OUT4(n) out float n[4]
#define GPU_OUT3(n) out float n[3]
#define GPU_FLOOR floor
#define GPU_ISFINITE isfinite
#define GPU_PRECISE precise
#define GPU_ASUINT asuint
#define GPU_ASFLOAT asfloat
#else
#include <math.h>
#include <string.h>
#define GPU_FN static
#define GPU_UNROLL
#define GPU_OUT4(n) float n[4]
#define GPU_OUT3(n) float n[3]
#define GPU_FLOOR floorf
#define GPU_ISFINITE isfinite
#define GPU_PRECISE
static uint32_t GPU_ASUINT(float v) { uint32_t u; memcpy(&u,&v,4);return u; }
static float GPU_ASFLOAT(uint32_t u) { float v;memcpy(&v,&u,4);return v; }
#endif
#define R_ZERO 0
#define R_C0 1
#define R_C1 2
#define R_FOG 3
#define R_V0 4
#define R_V1 5
#define R_T0 8
#define R_R0 12
#define R_R1 13
#define R_SUM 14
#define R_EF 15
GPU_FN void gpu_unpack(uint32_t c, GPU_OUT4(o))
{
    o[0] = (float)((c >> 16) & 0xFF) / 255.0f;
    o[1] = (float)((c >>  8) & 0xFF) / 255.0f;
    o[2] = (float)( c        & 0xFF) / 255.0f;
    o[3] = (float)( c >> 24        ) / 255.0f;
}

GPU_FN float gpu_clampf(float x, float lo, float hi)
{
    return x < lo ? lo : (x > hi ? hi : x);
}

/* PS_INPUTMAPPING, psh.c get_input_var. */
GPU_FN float gpu_map_in(float x, uint32_t byte)
{
    float p = x < 0.0f ? 0.0f : x;
    switch ((byte >> 5) & 7) {
    case 0:  return p;                               /* unsigned identity */
    case 1:  return 1.0f - gpu_clampf(x, 0.0f, 1.0f);    /* unsigned invert   */
    case 2:  return 2.0f * p - 1.0f;                 /* expand normal     */
    case 3:  return -2.0f * p + 1.0f;                /* expand negate     */
    case 4:  return p - 0.5f;                        /* half-bias normal  */
    case 5:  return -p + 0.5f;                       /* half-bias negate  */
    case 6:  return x;                               /* signed identity   */
    default: return -x;                              /* signed negate     */
    }
}

/* PS_COMBINEROUTPUT scale/bias, psh.c get_output. */
GPU_FN float gpu_map_out(float x, uint32_t mapping)
{
    switch (mapping) {
    case 0x08: return x - 0.5f;
    case 0x10: return x * 2.0f;
    case 0x18: return (x - 0.5f) * 2.0f;
    case 0x20: return x * 4.0f;
    case 0x30: return x * 0.5f;
    default:   return x;
    }
}

/* Named combiner registers keep shader indexing out of the register file.
 * The array evaluator remains the independent portable reference. */
#if (defined(NV_GPU_HLSL) && !NV_GPU_ARRAY_REGISTERS) || defined(NV_GPU_FIXED_REGISTERS)
#ifdef NV_GPU_HLSL
#define GPU_REG_INOUT inout NvGpuRegisters R
#define GPU_REG_VECTOR(n) float4 n
#define GPU_REG_VARIABLE(R) NvGpuRegisters R=(NvGpuRegisters)0
#define GPU_REG_FIELD(n) R.n
#define GPU_REG_ARG(R) R
#else
#define GPU_REG_INOUT NvGpuRegisters *R
#define GPU_REG_VECTOR(n) float n[4]
#define GPU_REG_VARIABLE(R) NvGpuRegisters R={0}
#define GPU_REG_FIELD(n) R->n
#define GPU_REG_ARG(R) &(R)
typedef struct NvGpuRegisters NvGpuRegisters;
#endif
struct NvGpuRegisters {
    GPU_REG_VECTOR(c0);
    GPU_REG_VECTOR(c1);
    GPU_REG_VECTOR(fog);
    GPU_REG_VECTOR(v0);
    GPU_REG_VECTOR(v1);
    GPU_REG_VECTOR(unused6);
    GPU_REG_VECTOR(unused7);
    GPU_REG_VECTOR(t0);
    GPU_REG_VECTOR(t1);
    GPU_REG_VECTOR(t2);
    GPU_REG_VECTOR(t3);
    GPU_REG_VECTOR(r0);
    GPU_REG_VECTOR(r1);
    GPU_REG_VECTOR(sum);
    GPU_REG_VECTOR(ef);
};
#define GPU_REG_DECL(R) NvGpuRegisters R
#ifdef NV_GPU_HLSL
GPU_FN float4 gpu_reg_vector(NvGpuRegisters R,uint32_t reg)
{
    switch(reg) {
    case 1:return R.c0;
    case 2:return R.c1;
    case 3:return R.fog;
    case 4:return R.v0;
    case 5:return R.v1;
    case 6:return R.unused6;
    case 7:return R.unused7;
    case 8:return R.t0;
    case 9:return R.t1;
    case 10:return R.t2;
    case 11:return R.t3;
    case 12:return R.r0;
    case 13:return R.r1;
    case 14:return R.sum;
    case 15:return R.ef;
    default:return 0;
    }
}
#endif
GPU_FN float gpu_reg_get(NvGpuRegisters R, uint32_t reg, int channel)
{
    switch(reg) {
    case 1:return R.c0[channel];
    case 2:return R.c1[channel];
    case 3:return R.fog[channel];
    case 4:return R.v0[channel];
    case 5:return R.v1[channel];
    case 6:return R.unused6[channel];
    case 7:return R.unused7[channel];
    case 8:return R.t0[channel];
    case 9:return R.t1[channel];
    case 10:return R.t2[channel];
    case 11:return R.t3[channel];
    case 12:return R.r0[channel];
    case 13:return R.r1[channel];
    case 14:return R.sum[channel];
    case 15:return R.ef[channel];
    default:return 0;
    }
}
GPU_FN void gpu_reg_set(GPU_REG_INOUT, uint32_t reg, int channel, float value)
{
    switch(reg) {
    case 1:GPU_REG_FIELD(c0)[channel]=value;break;
    case 2:GPU_REG_FIELD(c1)[channel]=value;break;
    case 3:GPU_REG_FIELD(fog)[channel]=value;break;
    case 4:GPU_REG_FIELD(v0)[channel]=value;break;
    case 5:GPU_REG_FIELD(v1)[channel]=value;break;
    case 6:GPU_REG_FIELD(unused6)[channel]=value;break;
    case 7:GPU_REG_FIELD(unused7)[channel]=value;break;
    case 8:GPU_REG_FIELD(t0)[channel]=value;break;
    case 9:GPU_REG_FIELD(t1)[channel]=value;break;
    case 10:GPU_REG_FIELD(t2)[channel]=value;break;
    case 11:GPU_REG_FIELD(t3)[channel]=value;break;
    case 12:GPU_REG_FIELD(r0)[channel]=value;break;
    case 13:GPU_REG_FIELD(r1)[channel]=value;break;
    case 14:GPU_REG_FIELD(sum)[channel]=value;break;
    case 15:GPU_REG_FIELD(ef)[channel]=value;break;
    }
}
#define GPU_RGET(R,r,c) gpu_reg_get(R,r,c)
/* Vector destination writes select a named register once for RGB and its
 * optional blue-to-alpha write. Scalar and array evaluators remain references. */
#ifndef NV_GPU_VECTOR_COMBINERS
#define NV_GPU_VECTOR_COMBINERS 0
#endif
#if NV_GPU_VECTOR_COMBINERS
GPU_FN void gpu_reg_write_rgb(GPU_REG_INOUT, uint32_t reg,float value[3],int blue_alpha)
{
#ifdef NV_GPU_HLSL
#define GPU_WRITE_RGB(member) R.member=float4(value[0],value[1],value[2],blue_alpha?value[2]:R.member.w)
#else
#define GPU_WRITE_RGB(member) do { GPU_REG_FIELD(member)[0]=value[0];GPU_REG_FIELD(member)[1]=value[1];GPU_REG_FIELD(member)[2]=value[2];if(blue_alpha)GPU_REG_FIELD(member)[3]=value[2]; } while(0)
#endif
    switch(reg){
    case 1:GPU_WRITE_RGB(c0);break;case 2:GPU_WRITE_RGB(c1);break;
    case 3:GPU_WRITE_RGB(fog);break;case 4:GPU_WRITE_RGB(v0);break;
    case 5:GPU_WRITE_RGB(v1);break;case 6:GPU_WRITE_RGB(unused6);break;
    case 7:GPU_WRITE_RGB(unused7);break;case 8:GPU_WRITE_RGB(t0);break;
    case 9:GPU_WRITE_RGB(t1);break;case 10:GPU_WRITE_RGB(t2);break;
    case 11:GPU_WRITE_RGB(t3);break;case 12:GPU_WRITE_RGB(r0);break;
    case 13:GPU_WRITE_RGB(r1);break;case 14:GPU_WRITE_RGB(sum);break;
    case 15:GPU_WRITE_RGB(ef);break;
    }
#undef GPU_WRITE_RGB
}
#define GPU_WRITE_COMBINER_RGB(R,r,v,b) gpu_reg_write_rgb(GPU_REG_ARG(R),r,v,b)
#endif
#define GPU_RSET(R,r,c,v) gpu_reg_set(GPU_REG_ARG(R),r,c,v)
#else
#define GPU_REG_DECL(R) float R[16][4]
#define GPU_REG_VARIABLE(R) GPU_PRECISE float R[16][4]
#define GPU_RGET(R,r,c) R[r][c]
#define GPU_RSET(R,r,c,v) (R[r][c]=(v))
#endif

GPU_FN void gpu_read_rgb(GPU_REG_DECL(R), uint32_t byte, GPU_OUT3(v))
{
     uint32_t reg = byte & 0xF;
    int i;
#if defined(NV_GPU_HLSL) && !NV_GPU_ARRAY_REGISTERS
    float4 value=gpu_reg_vector(R,reg);
    GPU_UNROLL for(i=0;i<3;i++)v[i]=gpu_map_in((byte & 0x10)?value.w:value[i],byte);
#else
    GPU_UNROLL for (i = 0; i < 3; i++)
        v[i] = gpu_map_in((byte & 0x10) ? GPU_RGET(R,reg,3) : GPU_RGET(R,reg,i), byte);
#endif
}

GPU_FN float gpu_read_alpha(GPU_REG_DECL(R), uint32_t byte)
{
     uint32_t reg = byte & 0xF;
    return gpu_map_in((byte & 0x10) ? GPU_RGET(R,reg,3) : GPU_RGET(R,reg,2), byte);
}

/* Exact common one-stage materials, including QPR2's fogged ATV draw.
 * Constants, mux state and vertex alpha are used only when the words below
 * actually reference them. Other programs retain the complete interpreter. */
GPU_FN int gpu_rc_simple_kind(Nv2aCombiner rc)
{
    if ((rc.control & 255u) != 1 ||
        (rc.stage_program != 1 && rc.stage_program != 2) ||
        rc.color_icw[0] != 0xC8C40000u ||
        (rc.color_ocw[0] != 0x100C0u && rc.color_ocw[0] != 0xC0u) ||
        (rc.alpha_icw[0] != 0xD8301010u && rc.alpha_icw[0] != 0xD8D41010u) ||
        rc.alpha_ocw[0] != 0xC0u ||
        (rc.final0 != 0x130C0300u && rc.final0 != 0xCu) ||
        (rc.final1 != 0x1C80u && rc.final1 != 0x1C00u)) return 0;
    return 1;
}
GPU_FN void gpu_rc_simple(Nv2aCombiner rc, float v0[4], float fog[4],
                        float texel[4], GPU_OUT4(result))
{
    GPU_UNROLL for (int i=0;i<3;i++) {
        GPU_PRECISE float product=texel[i]*v0[i];
        GPU_PRECISE float scaled=rc.color_ocw[0]==0x100C0u?product*2.0f:product;
        GPU_PRECISE float rgb=gpu_clampf(scaled,-1.0f,1.0f);
        rgb=rgb<0.0f?0.0f:rgb;
        if(rc.final0==0x130C0300u) {
            GPU_PRECISE float f=fog[3]<0.0f?0.0f:fog[3];
            GPU_PRECISE float c=fog[i]<0.0f?0.0f:fog[i];
            result[i]=0.0f+c*(1.0f-f)+rgb*f;
        } else result[i]=rgb;
        result[i]=gpu_clampf(result[i],0.0f,1.0f);
    }
    GPU_PRECISE float alpha=texel[3]*(rc.alpha_icw[0]==0xD8D41010u?v0[3]:1.0f);
    result[3]=gpu_clampf(alpha,0.0f,1.0f);
}
#ifndef NV_GPU_SIMPLE_MATERIAL
GPU_FN void gpu_rc_eval(Nv2aCombiner rc,  float v0[4],
                   float v1[4],  float fog[4],
                   float t[4][4], GPU_OUT4(result))
{
#ifdef NV_GPU_HLSL
    /* The graphics path and mixed general batches can use the same exact
     * one-stage algebra without entering the register-array interpreter. */
    if(gpu_rc_simple_kind(rc)){gpu_rc_simple(rc,v0,fog,t[0],result);return;}
#endif
    GPU_REG_VARIABLE(R);
    uint32_t n = rc.control & 0xFF, flags = rc.control >> 8, s;
    int i;

    if (n > 8)
        n = 8;
    /* Only the registers a program can read before writing need a value:
     * zero, r0, r1 and the final-combiner specials. */
    GPU_UNROLL for (i = 0; i < 4; i++) {
        GPU_RSET(R,0,i,0);GPU_RSET(R,6,i,0);GPU_RSET(R,7,i,0);
        GPU_RSET(R,12,i,0);GPU_RSET(R,13,i,0);GPU_RSET(R,14,i,0);GPU_RSET(R,15,i,0);
    }
    GPU_UNROLL for (i = 0; i < 4; i++) {
        GPU_RSET(R,R_FOG,i,fog[i]);
        GPU_RSET(R,R_V0,i,v0[i]);
        GPU_RSET(R,R_V1,i,v1[i]);
        GPU_RSET(R,R_T0 + 0,i,t[0][i]);
        GPU_RSET(R,R_T0 + 1,i,t[1][i]);
        GPU_RSET(R,R_T0 + 2,i,t[2][i]);
        GPU_RSET(R,R_T0 + 3,i,t[3][i]);
    }
    /* r0.a starts as texture 0's alpha, or 1 with stage 0 off (psh.c). */
    GPU_RSET(R,R_R0,3,(rc.stage_program & 0x1F) ? t[0][3] : 1.0f);

    for (s = 0; s < n; s++) {
        uint32_t icw = rc.color_icw[s], ocw = rc.color_ocw[s];
        uint32_t aicw = rc.alpha_icw[s], aocw = rc.alpha_ocw[s];
        uint32_t fl = ocw >> 12, afl = aocw >> 12;
        GPU_PRECISE float A[3], B[3], C[3], D[3], ab[3], cd[3], ms[3];
        GPU_PRECISE float aA, aB, aC, aD, aab, acd, ams;
        int mux_cd;

        /* One c0/c1 for every stage, or one per stage (COMBINERCOUNT). */
        { float unpacked[4];gpu_unpack(rc.factor0[(flags & 0x010) ? s : 0],unpacked);GPU_UNROLL for(int k=0;k<4;k++)GPU_RSET(R,R_C0,k,unpacked[k]); }
        { float unpacked[4];gpu_unpack(rc.factor1[(flags & 0x100) ? s : 0],unpacked);GPU_UNROLL for(int k=0;k<4;k++)GPU_RSET(R,R_C1,k,unpacked[k]); }
        mux_cd = (flags & 1) ? GPU_RGET(R,R_R0,3) >= 0.5f
                             : ((int)(GPU_RGET(R,R_R0,3) * 255.0f) & 1);

        /* Both portions read before either writes (psh.c emits all the
         * arithmetic, then the assignments). */
        gpu_read_rgb(R, icw >> 24, A); gpu_read_rgb(R, icw >> 16, B);
        gpu_read_rgb(R, icw >> 8, C);  gpu_read_rgb(R, icw, D);
        aA = gpu_read_alpha(R, aicw >> 24); aB = gpu_read_alpha(R, aicw >> 16);
        aC = gpu_read_alpha(R, aicw >> 8);  aD = gpu_read_alpha(R, aicw);

        GPU_UNROLL for (i = 0; i < 3; i++) {
            GPU_PRECISE float dab = A[0] * B[0] + A[1] * B[1] + A[2] * B[2];
            GPU_PRECISE float dcd = C[0] * D[0] + C[1] * D[1] + C[2] * D[2];
            GPU_PRECISE float pab = (fl & 2) ? dab : A[i] * B[i];
            GPU_PRECISE float pcd = (fl & 1) ? dcd : C[i] * D[i];
            GPU_PRECISE float sum = (fl & 4) ? (mux_cd ? pcd : pab) : pab + pcd;
            ab[i] = gpu_clampf(gpu_map_out(pab, fl & 0x38), -1.0f, 1.0f);
            cd[i] = gpu_clampf(gpu_map_out(pcd, fl & 0x38), -1.0f, 1.0f);
            ms[i] = gpu_clampf(gpu_map_out(sum, fl & 0x38), -1.0f, 1.0f);
        }
        {
            GPU_PRECISE float pab = aA * aB, pcd = aC * aD;
            GPU_PRECISE float sum = (afl & 4) ? (mux_cd ? pcd : pab) : pab + pcd;
            aab = gpu_clampf(gpu_map_out(pab, afl & 0x38), -1.0f, 1.0f);
            acd = gpu_clampf(gpu_map_out(pcd, afl & 0x38), -1.0f, 1.0f);
            ams = gpu_clampf(gpu_map_out(sum, afl & 0x38), -1.0f, 1.0f);
        }

        /* RGB destinations; blue-to-alpha also writes the destination's .a. */
        if ((ocw >> 4) & 0xF) {
            uint32_t dest = (ocw >> 4) & 0xF;
#ifdef GPU_WRITE_COMBINER_RGB
            GPU_WRITE_COMBINER_RGB(R,dest,ab,fl & 0x80);
#else
            GPU_RSET(R,dest,0,ab[0]); GPU_RSET(R,dest,1,ab[1]); GPU_RSET(R,dest,2,ab[2]);
            if (fl & 0x80) GPU_RSET(R,dest,3,ab[2]);
#endif
        }
        if (ocw & 0xF) {
            uint32_t dest = ocw & 0xF;
#ifdef GPU_WRITE_COMBINER_RGB
            GPU_WRITE_COMBINER_RGB(R,dest,cd,fl & 0x40);
#else
            GPU_RSET(R,dest,0,cd[0]); GPU_RSET(R,dest,1,cd[1]); GPU_RSET(R,dest,2,cd[2]);
            if (fl & 0x40) GPU_RSET(R,dest,3,cd[2]);
#endif
        }
        if ((ocw >> 8) & 0xF) {
            uint32_t dest = (ocw >> 8) & 0xF;
#ifdef GPU_WRITE_COMBINER_RGB
            GPU_WRITE_COMBINER_RGB(R,dest,ms,0);
#else
            GPU_RSET(R,dest,0,ms[0]); GPU_RSET(R,dest,1,ms[1]); GPU_RSET(R,dest,2,ms[2]);
#endif
        }
        if ((aocw >> 4) & 0xF) GPU_RSET(R,(aocw >> 4) & 0xF,3,aab);
        if (aocw & 0xF)        GPU_RSET(R,aocw & 0xF,3,acd);
        if ((aocw >> 8) & 0xF) GPU_RSET(R,(aocw >> 8) & 0xF,3,ams);
        /* Register 0 is zero whatever was "written" to it. */
        GPU_UNROLL for(i=0;i<4;i++)GPU_RSET(R,R_ZERO,i,0);
    }

    if (rc.final0 || rc.final1) {
        uint32_t f0 = rc.final0, f1 = rc.final1, fflags = f1 & 0xFF;
        GPU_PRECISE float A[3], B[3], C[3], D[3], E[3], F[3];

        { float unpacked[4];gpu_unpack(rc.final_c0,unpacked);GPU_UNROLL for(int k=0;k<4;k++)GPU_RSET(R,R_C0,k,unpacked[k]); }
        { float unpacked[4];gpu_unpack(rc.final_c1,unpacked);GPU_UNROLL for(int k=0;k<4;k++)GPU_RSET(R,R_C1,k,unpacked[k]); }
        /* V1R0 sum: optional complements (0x40 v1, 0x20 r0), clamp (0x80). */
        GPU_UNROLL for (i = 0; i < 3; i++) {
            float a = (fflags & 0x40) ? 1.0f - GPU_RGET(R,R_V1,i) : GPU_RGET(R,R_V1,i);
            float b = (fflags & 0x20) ? 1.0f - GPU_RGET(R,R_R0,i) : GPU_RGET(R,R_R0,i);
            GPU_RSET(R,R_SUM,i,(fflags & 0x80) ? gpu_clampf(a + b, 0.0f, 1.0f) : a + b);
        }
        GPU_RSET(R,R_SUM,3,0.0f);
        gpu_read_rgb(R, f1 >> 24, E);
        gpu_read_rgb(R, f1 >> 16, F);
        GPU_UNROLL for (i = 0; i < 3; i++)
            GPU_RSET(R,R_EF,i,E[i] * F[i]);
        GPU_RSET(R,R_EF,3,0.0f);
        gpu_read_rgb(R, f0 >> 24, A); gpu_read_rgb(R, f0 >> 16, B);
        gpu_read_rgb(R, f0 >> 8, C);  gpu_read_rgb(R, f0, D);
        /* D + mix(C, B, A), alpha from G. */
        GPU_UNROLL for (i = 0; i < 3; i++)
            result[i] = D[i] + C[i] * (1.0f - A[i]) + B[i] * A[i];
        result[3] = gpu_read_alpha(R, f1 >> 8);
    } else {
        GPU_UNROLL for (i = 0; i < 4; i++)
            result[i] = GPU_RGET(R,R_R0,i);
    }
    GPU_UNROLL for (i = 0; i < 4; i++)
        result[i] = gpu_clampf(result[i], 0.0f, 1.0f);
}
#endif
