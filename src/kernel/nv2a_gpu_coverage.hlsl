/* Hardware creates conservative triangle candidates; the unchanged pixel
 * program still owns exact NV2A coverage, interpolation, depth and blending. */
#define NV_GPU_HLSL 1
#include "nv2a_gpu_types.h"
StructuredBuffer<NvGpuTri> triangles : register(t0);
StructuredBuffer<NvGpuState> configuration : register(t3);
struct RasterInput { float4 position : SV_Position; nointerpolation uint primitive : TEXCOORD0; };
RasterInput vertex_main(uint id : SV_VertexID)
{
    uint primitive=id/3,vertex=id%3;
    NvGpuTri tri=triangles[primitive];
    float width=(float)configuration[0].width,height=(float)configuration[0].height;
    /* Keep member addresses static instead of dynamically indexing full vertex outputs. */
    float2 pixel=vertex==0?float2(tri.v[0].pos[0],tri.v[0].pos[1]):
                 vertex==1?float2(tri.v[1].pos[0],tri.v[1].pos[1]):
                           float2(tri.v[2].pos[0],tri.v[2].pos[1]);
    RasterInput out_value;
    out_value.position=float4(pixel.x*2.0f/width-1.0f,1.0f-pixel.y*2.0f/height,0.5f,1.0f);
    out_value.primitive=primitive;return out_value;
}
