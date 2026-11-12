/* Exact scalar triangle preparation shared by geometry CS and C fixtures. */
#ifndef NV_GPU_HLSL
#define PREP_PRECISE
#define PREP_FN static
#define PREP_FINITE isfinite
#define PREP_MIN fminf
#define PREP_MAX fmaxf
#define PREP_FLOOR floorf
#define PREP_CEIL ceilf
#define PREP_EXP2 exp2f
#define PREP_ABS fabsf
#else
#define PREP_PRECISE precise
#define PREP_FN
#define PREP_FINITE isfinite
#define PREP_MIN min
#define PREP_MAX max
#define PREP_FLOOR floor
#define PREP_CEIL ceil
#define PREP_EXP2 exp2
#define PREP_ABS abs
#endif
PREP_FN float nv_raw_fog(NvRawParams p,float d){
 if(!p.fog_enable)return 1.0f;
 PREP_PRECISE float f;
 if(p.fog_mode==0x800u || p.fog_mode==0x802u)f=p.fog_x+PREP_EXP2(d*p.fog_y*16.0f)-1.5f;
 else if(p.fog_mode==0x801u || p.fog_mode==0x803u)f=p.fog_x+PREP_EXP2(-d*d*p.fog_y*p.fog_y*32.0f)-1.5f;
 else f=p.fog_x+d*p.fog_y-1.0f;
 if(p.fog_mode==0x802u || p.fog_mode==0x803u || p.fog_mode==0x804u)f=PREP_ABS(f);
 if(f!=f)f=1;return f<0?0:(f>1?1:f);
}
PREP_FN int nv_raw_edge(float ax,float ay,float bx,float by,float area){
 float dx=bx-ax,dy=by-ay;if(area<0){dx=-dx;dy=-dy;}
 return dy<0 || (dy==0 && dx>0);
}
PREP_FN int nv_raw_bound(float v,uint32_t lo,uint32_t hi,int upper){
 if(v<=(float)lo)return (int)lo;if(v>=(float)hi)return (int)hi;
 return (int)(upper?PREP_CEIL(v):PREP_FLOOR(v));
}
PREP_FN NvGpuTri nv_raw_prepare(NvRawParams p,Nv2aVshOutput a,Nv2aVshOutput b,Nv2aVshOutput c){
 NvGpuTri t;
#ifdef NV_GPU_HLSL
 t=(NvGpuTri)0;
#else
 memset(&t,0,sizeof t);
#endif
 t.state_index=p.state;
 /* Invalid primitives are degenerate outside the viewport. They consume no
  * variable-size output and never require a CPU count/readback/indirect wait. */
 for(int j=0;j<3;j++){t.v[j].pos[0]=t.v[j].pos[1]=-16;t.v[j].pos[3]=1;}
 PREP_PRECISE float area=(b.pos[0]-a.pos[0])*(c.pos[1]-a.pos[1])-(b.pos[1]-a.pos[1])*(c.pos[0]-a.pos[0]);
 if(a.pos[3]<=0 || b.pos[3]<=0 || c.pos[3]<=0 || !PREP_FINITE(area) || area==0)return t;
 for(int j=0;j<4;j++)if(!PREP_FINITE(a.pos[j]) || !PREP_FINITE(b.pos[j]) || !PREP_FINITE(c.pos[j]))return t;
 if(p.cull_enable){
  int front=p.front_face==0x901u?area<0:area>0;
  if(p.cull_face==0x408u || (p.cull_face==0x404u && front) || (p.cull_face==0x405u && !front))return t;
 }
 int minx=nv_raw_bound(PREP_MIN(a.pos[0],PREP_MIN(b.pos[0],c.pos[0])),p.left,p.right,0);
 int maxx=nv_raw_bound(PREP_MAX(a.pos[0],PREP_MAX(b.pos[0],c.pos[0])),p.left,p.right,1);
 int miny=nv_raw_bound(PREP_MIN(a.pos[1],PREP_MIN(b.pos[1],c.pos[1])),p.top,p.bottom,0);
 int maxy=nv_raw_bound(PREP_MAX(a.pos[1],PREP_MAX(b.pos[1],c.pos[1])),p.top,p.bottom,1);
 if(minx>=maxx || miny>=maxy)return t;
 t.v[0]=a;t.v[1]=b;t.v[2]=c;t.area=area;t.inv_area=1.0f/area;
 t.minx=minx;t.maxx=maxx;t.miny=miny;t.maxy=maxy;
 t.inclusive[0]=nv_raw_edge(a.pos[0],a.pos[1],b.pos[0],b.pos[1],area);
 t.inclusive[1]=nv_raw_edge(b.pos[0],b.pos[1],c.pos[0],c.pos[1],area);
 t.inclusive[2]=nv_raw_edge(c.pos[0],c.pos[1],a.pos[0],a.pos[1],area);
 t.use_rc=p.use_rc;t.textured=p.textured;
 for(int j=0;j<3;j++){
  t.iw[j]=1.0f/t.v[j].pos[3];t.uv[j][0]=t.v[j].tex[0][0]*p.su*t.iw[j];t.uv[j][1]=t.v[j].tex[0][1]*p.sv*t.iw[j];
  if(p.use_rc){for(int st=0;st<4;st++)for(int k=0;k<4;k++)t.stc[j][st][k]=t.v[j].tex[st][k]*t.iw[j];t.vfog[j]=nv_raw_fog(p,t.v[j].fog[0]);}
 }
 if(p.use_rc)for(int j=0;j<4;j++)t.fogc[j]=p.fog_color[j];
 return t;
}
