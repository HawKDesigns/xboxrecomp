/* QPR2 raw geometry: persistent raw streams -> vertex programs -> GPU triangles. */
#define NV_GPU_HLSL 1
#define int32_t int
#include "nv2a_gpu_types.h"
#include "nv2a_gpu_raw_types.h"
StructuredBuffer<uint> raw_draw_words : register(t3);
NvRawParams nv_raw_params(uint draw){
 NvRawParams p;uint b=draw*792u+768u;
 p.program=raw_draw_words[b+0];
 p.count=raw_draw_words[b+1];
 p.state=raw_draw_words[b+2];
 p.vertex_base=raw_draw_words[b+3];
 p.left=raw_draw_words[b+4];
 p.top=raw_draw_words[b+5];
 p.right=raw_draw_words[b+6];
 p.bottom=raw_draw_words[b+7];
 p.cull_enable=raw_draw_words[b+8];
 p.cull_face=raw_draw_words[b+9];
 p.front_face=raw_draw_words[b+10];
 p.use_rc=raw_draw_words[b+11];
 p.textured=raw_draw_words[b+12];
 p.fog_enable=raw_draw_words[b+13];
 p.fog_mode=raw_draw_words[b+14];
 p.reserved=raw_draw_words[b+15];
 p.su=asfloat(raw_draw_words[b+16]);
 p.sv=asfloat(raw_draw_words[b+17]);
 p.fog_x=asfloat(raw_draw_words[b+18]);
 p.fog_y=asfloat(raw_draw_words[b+19]);
 p.fog_color[0]=asfloat(raw_draw_words[b+20]);
 p.fog_color[1]=asfloat(raw_draw_words[b+21]);
 p.fog_color[2]=asfloat(raw_draw_words[b+22]);
 p.fog_color[3]=asfloat(raw_draw_words[b+23]);
 return p;
}
StructuredBuffer<NvRawJob> raw_jobs : register(t4);
#if NV_RAW_TRANSFORM
StructuredBuffer<uint> raw_bytes : register(t0);
StructuredBuffer<NvRawAttr> raw_attributes : register(t1);
StructuredBuffer<uint> raw_programs : register(t2);
RWStructuredBuffer<Nv2aVshOutput> raw_vertices : register(u0);
uint nv_raw_data(uint at){return raw_bytes[at];}
NvRawAttr nv_raw_attr(uint draw,uint at){return raw_attributes[nv_raw_params(draw).reserved+at];}
float nv_raw_asfloat(uint v){return asfloat(v);}
float nv_raw_constant(uint draw,int c,int k){return asfloat(raw_draw_words[draw*792u+(uint)c*4u+(uint)k]);}
uint nv_raw_program(uint at){return raw_programs[at];}
#include "nv2a_gpu_vsh.inc"
[numthreads(64,1,1)]
void main(uint3 id:SV_DispatchThreadID){
 NvRawJob job=raw_jobs[id.x];if(job.draw==0xFFFFFFFFu)return;
 raw_vertices[job.output]=nv_raw_vertex(job.draw,job.element);
}
#else
StructuredBuffer<Nv2aVshOutput> raw_vertices : register(t0);
StructuredBuffer<NvRawAttr> raw_attributes : register(t1);
StructuredBuffer<uint> raw_indices : register(t2);
RWStructuredBuffer<NvGpuTri> raw_triangles : register(u0);
#include "nv2a_gpu_prepare.inc"
[numthreads(64,1,1)]
void main(uint3 id:SV_DispatchThreadID){
 NvRawJob job=raw_jobs[id.x];if(job.draw==0xFFFFFFFFu)return;
 NvRawParams p=nv_raw_params(job.draw);
 uint at=raw_attributes[p.reserved+16].offset+job.element*3;
 Nv2aVshOutput a=raw_vertices[p.vertex_base+raw_indices[at]];
 Nv2aVshOutput b=raw_vertices[p.vertex_base+raw_indices[at+1]];
 Nv2aVshOutput c=raw_vertices[p.vertex_base+raw_indices[at+2]];
 raw_triangles[job.output]=nv_raw_prepare(p,a,b,c);
}
#endif
