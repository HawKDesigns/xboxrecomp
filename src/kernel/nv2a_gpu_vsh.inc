/* Shared scalar NV2A operations. Loaders are supplied by the C reference or HLSL. */
#ifndef NV_GPU_HLSL
#define RAW_LOOP
#define RAW_PRECISE
#define RAW_FN static
#define RAW_OUT(n) float n[4]
#define RAW_INOUT(n) float n[4]
#define RAW_ABS fabsf
#define RAW_SQRT sqrtf
#define RAW_POW powf
#define RAW_EXP2 exp2f
#define RAW_LOG2 log2f
#define RAW_FLOOR floorf
#define RAW_INF INFINITY
#else
#define RAW_LOOP [loop]
#define RAW_PRECISE precise
#define RAW_FN
#define RAW_OUT(n) out float n[4]
#define RAW_INOUT(n) inout float n[4]
#define RAW_ABS abs
#define RAW_SQRT sqrt
#define RAW_POW pow
#define RAW_EXP2 exp2
#define RAW_LOG2 log2
#define RAW_FLOOR floor
#define RAW_INF asfloat(0x7F800000u)
#endif
struct NvRawVec {RAW_PRECISE float v[4];};
#ifndef NV_GPU_HLSL
typedef struct NvRawVec NvRawVec;
#endif
RAW_FN uint32_t nv_raw_bits(uint32_t value,int lo,int width){return (value>>lo)&((1u<<width)-1u);}
RAW_FN uint32_t nv_raw_unaligned(uint32_t at){
 uint32_t shift=(at&3u)*8u,v=nv_raw_data(at/4u);
 return shift?(v>>shift)|(nv_raw_data(at/4u+1u)<<(32u-shift)):v;
}
RAW_FN NvRawVec nv_raw_attribute(uint32_t draw,uint32_t vertex,uint32_t attr){
 NvRawAttr a=nv_raw_attr(draw,attr);NvRawVec r;
 r.v[0]=r.v[1]=r.v[2]=0.0f;r.v[3]=1.0f;
 if(!a.size || !a.stride)return r;
 uint32_t at=a.offset+vertex*a.stride,v=nv_raw_unaligned(at);
 if(a.type==0){r.v[0]=(float)((v>>16)&255)/255.0f;r.v[1]=(float)((v>>8)&255)/255.0f;r.v[2]=(float)(v&255)/255.0f;r.v[3]=(float)(v>>24)/255.0f;}
 else if(a.type==6){r.v[0]=(float)((int32_t)(v<<21)>>21)/1023.0f;r.v[1]=(float)((int32_t)(v<<10)>>21)/1023.0f;r.v[2]=(float)((int32_t)v>>22)/511.0f;}
 else for(uint32_t j=0;j<a.size && j<4;j++){
  if(a.type==2)r.v[j]=nv_raw_asfloat(nv_raw_unaligned(at+j*4));
  else if(a.type==4)r.v[j]=(float)(nv_raw_unaligned(at+j)&255u)/255.0f;
  else if(a.type==1 || a.type==5){float f=(float)((int32_t)(nv_raw_unaligned(at+j*2)<<16)>>16);r.v[j]=a.type==1?f/32767.0f:f;}
 }
 return r;
}
RAW_FN NvRawVec nv_raw_source(uint32_t ins[4],int which,uint32_t draw,uint32_t vertex,NvRawVec temp[12],NvRawVec opos,int a0){
 uint32_t neg,swz,mux,reg;
 if(which==0){neg=nv_raw_bits(ins[1],8,1);swz=nv_raw_bits(ins[1],0,8);mux=nv_raw_bits(ins[2],26,2);reg=nv_raw_bits(ins[2],28,4);}
 else if(which==1){neg=nv_raw_bits(ins[2],25,1);swz=nv_raw_bits(ins[2],17,8);mux=nv_raw_bits(ins[2],11,2);reg=nv_raw_bits(ins[2],13,4);}
 else{neg=nv_raw_bits(ins[2],10,1);swz=nv_raw_bits(ins[2],2,8);mux=nv_raw_bits(ins[3],28,2);reg=(nv_raw_bits(ins[2],0,2)<<2)|nv_raw_bits(ins[3],30,2);}
 NvRawVec base,r;
 if(mux==1){if(reg==12)base=opos;else base=temp[reg<12?reg:0];}
 else if(mux==2)base=nv_raw_attribute(draw,vertex,nv_raw_bits(ins[1],9,4));
 else{int ci=(int)nv_raw_bits(ins[1],13,8);if(ins[3]&2u)ci+=a0;if(ci<0 || ci>=192)ci=0;for(int k=0;k<4;k++)base.v[k]=nv_raw_constant(draw,ci,k);}
 for(int k=0;k<4;k++){float f=base.v[(swz>>(6-k*2))&3u];r.v[k]=neg?-f:f;}return r;
}
RAW_FN void nv_raw_mask(RAW_INOUT(dst),NvRawVec value,uint32_t mask){for(int k=0;k<4;k++)if(mask&(8u>>k))dst[k]=value.v[k];}
RAW_FN NvRawVec nv_raw_splat(float f)
{
    NvRawVec r;
    r.v[0] = r.v[1] = r.v[2] = r.v[3] = f;
    return r;
}

RAW_FN NvRawVec nv_raw_mac(uint32_t op, NvRawVec a, NvRawVec b, NvRawVec c)
{
    NvRawVec r = a;
    int i;

    switch (op) {
    case 1: break;                                            /* mov */
    case 2: for (i = 0; i < 4; i++) r.v[i] = a.v[i] * b.v[i]; break;
    case 3: for (i = 0; i < 4; i++) r.v[i] = a.v[i] + c.v[i]; break;
    case 4: for (i = 0; i < 4; i++) r.v[i] = a.v[i] * b.v[i] + c.v[i]; break;
    case 5: r = nv_raw_splat(a.v[0]*b.v[0] + a.v[1]*b.v[1] + a.v[2]*b.v[2]); break;
    case 6: r = nv_raw_splat(a.v[0]*b.v[0] + a.v[1]*b.v[1] + a.v[2]*b.v[2]
                      + b.v[3]); break;                        /* dph */
    case 7: r = nv_raw_splat(a.v[0]*b.v[0] + a.v[1]*b.v[1] + a.v[2]*b.v[2]
                      + a.v[3]*b.v[3]); break;
    case 8: r.v[0] = 1.0f; r.v[1] = a.v[1] * b.v[1];            /* dst */
            r.v[2] = a.v[2]; r.v[3] = b.v[3]; break;
    case 9: for (i = 0; i < 4; i++) r.v[i] = a.v[i] < b.v[i] ? a.v[i] : b.v[i]; break;
    case 10: for (i = 0; i < 4; i++) r.v[i] = a.v[i] >= b.v[i] ? a.v[i] : b.v[i]; break;
    case 11: for (i = 0; i < 4; i++) r.v[i] = a.v[i] < b.v[i] ? 1.0f : 0.0f; break;
    case 12: for (i = 0; i < 4; i++) r.v[i] = a.v[i] >= b.v[i] ? 1.0f : 0.0f; break;
    case 13: break;         /* arl */
    default: break;
    }
    return r;
}

RAW_FN NvRawVec nv_raw_ilu(uint32_t op, NvRawVec c)
{
    RAW_PRECISE float x = c.v[0];
    NvRawVec r = c;

    switch (op) {
    case 1: break;                                            /* mov */
    case 2: r = nv_raw_splat(x == 0.0f ? RAW_INF : 1.0f / x); break;  /* rcp */
    case 3: {                                                  /* rcc */
        RAW_PRECISE float f = 1.0f / x;
        if (RAW_ABS(f) < 5.42101e-20f) f = f < 0 ? -5.42101e-20f : 5.42101e-20f;
        if (RAW_ABS(f) > 1.884467e19f) f = f < 0 ? -1.884467e19f : 1.884467e19f;
        r = nv_raw_splat(f);
        break;
    }
    case 4: r = nv_raw_splat(1.0f / RAW_SQRT(RAW_ABS(x))); break;           /* rsq */
    case 5: {                                                  /* exp */
        RAW_PRECISE float fl = RAW_FLOOR(x);
        r.v[0] = RAW_EXP2(fl); r.v[1] = x - fl; r.v[2] = RAW_EXP2(x); r.v[3] = 1.0f;
        break;
    }
    case 6: {                                                  /* log */
        RAW_PRECISE float ax = RAW_ABS(x);
        if (ax == 0.0f) {
            r.v[0] = r.v[2] = -RAW_INF; r.v[1] = 1.0f;
        } else {
            RAW_PRECISE float e = RAW_FLOOR(RAW_LOG2(ax));
            r.v[0] = e; r.v[1] = ax / RAW_EXP2(e); r.v[2] = RAW_LOG2(ax);
        }
        r.v[3] = 1.0f;
        break;
    }
    case 7: {                                                  /* lit */
        RAW_PRECISE float nl = c.v[0] > 0.0f ? c.v[0] : 0.0f;
        RAW_PRECISE float nh = c.v[1] > 0.0f ? c.v[1] : 0.0f;
        RAW_PRECISE float p = c.v[3] < -127.9961f ? -127.9961f
                : (c.v[3] > 127.9961f ? 127.9961f : c.v[3]);
        r.v[0] = 1.0f; r.v[1] = nl;
        r.v[2] = c.v[0] > 0.0f ? RAW_POW(nh, p) : 0.0f; r.v[3] = 1.0f;
        break;
    }
    default: break;
    }
    return r;
}


RAW_FN Nv2aVshOutput nv_raw_vertex(uint32_t draw,uint32_t vertex){
 NvRawVec temp[12],opos;RAW_PRECISE float regs[13][4];Nv2aVshOutput out_value;int a0=0;
 for(int k=0;k<4;k++){opos.v[k]=0;for(int j=0;j<12;j++)temp[j].v[k]=0;for(int j=0;j<13;j++)regs[j][k]=0;}
 regs[3][3]=regs[4][3]=regs[9][3]=regs[10][3]=regs[11][3]=regs[12][3]=1.0f;
 NvRawParams p=nv_raw_params(draw);
 RAW_LOOP for(uint32_t slot=0;slot<p.count && slot<136;slot++){
  uint32_t ins[4];for(int j=0;j<4;j++)ins[j]=nv_raw_program(p.program+slot*4+j);
  uint32_t mac=nv_raw_bits(ins[1],21,4),ilu=nv_raw_bits(ins[1],25,3),dst=nv_raw_bits(ins[3],20,4);
  NvRawVec a=nv_raw_source(ins,0,draw,vertex,temp,opos,a0),b=nv_raw_source(ins,1,draw,vertex,temp,opos,a0),c=nv_raw_source(ins,2,draw,vertex,temp,opos,a0);
  NvRawVec m=nv_raw_splat(0),i=nv_raw_splat(0);
  if(mac)m=nv_raw_mac(mac,a,b,c);if(ilu)i=nv_raw_ilu(ilu,c);
  if(mac==13)a0=(int)RAW_FLOOR(a.v[0]+0.001f);
  if(mac && mac!=13){uint32_t mask=nv_raw_bits(ins[3],24,4);if(dst==12)nv_raw_mask(opos.v,m,mask);else if(dst<12)nv_raw_mask(temp[dst].v,m,mask);}
  if(ilu){uint32_t at=mac?1u:dst,mask=nv_raw_bits(ins[3],16,4);if(at==12)nv_raw_mask(opos.v,i,mask);else if(at<12)nv_raw_mask(temp[at].v,i,mask);}
  uint32_t mask=nv_raw_bits(ins[3],12,4),at=nv_raw_bits(ins[3],3,8);
  if(mask && (ins[3]&2048u)){NvRawVec value;if(ins[3]&4u)value=i;else value=m;if(!at)nv_raw_mask(opos.v,value,mask);else if(at<13)nv_raw_mask(regs[at],value,mask);}
  if(ins[3]&1u)break;
 }
 for(int k=0;k<4;k++){
  out_value.pos[k]=opos.v[k];out_value.d0[k]=regs[3][k];out_value.d1[k]=regs[4][k];out_value.fog[k]=regs[5][k];
  for(int j=0;j<4;j++)out_value.tex[j][k]=regs[j+9][k];
 }
 return out_value;
}
