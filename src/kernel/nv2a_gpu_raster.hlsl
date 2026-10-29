#define NV_GPU_HLSL 1
#include "nv2a_gpu_types.h"
StructuredBuffer<NvGpuTri> triangles : register(t0);
StructuredBuffer<uint> references : register(t1);
StructuredBuffer<NvGpuTile> tiles : register(t2);
StructuredBuffer<NvGpuState> configuration : register(t3);
StructuredBuffer<uint> texture_atlas : register(t4);
RWStructuredBuffer<uint> color_buffer : register(u0);
RWStructuredBuffer<float> depth_buffer : register(u1);
RWStructuredBuffer<uint> statistics : register(u2);
uint gpu_load_texel(NvGpuTexture t, int st, int face, int x, int y)
{
    uint offset = ((uint)face*t.height+(uint)y)*t.width+(uint)x;
    return texture_atlas[t.texel_base+offset];
}
#include "nv2a_gpu_pixel.inc"
#include "nv2a_gpu_shade.inc"
groupshared uint4 reductions[64];
groupshared uint maxima[64];
[numthreads(8,8,1)]
void main(uint3 group : SV_GroupID, uint3 thread : SV_GroupThreadID, uint index : SV_GroupIndex)
{
    uint width=configuration[0].width, height=configuration[0].height;
    /* Four independent wave-sized quadrants own the original 16x16 tile.
     * No pixel has multiple owners, and the ordered list is unchanged. */
    uint tile_index=group.x/4, quadrant=group.x&3;
    NvGpuTile tile = tiles[tile_index];
    uint x = tile.x+(quadrant&1)*8+thread.x, y = tile.y+(quadrant>>1)*8+thread.y;
    uint4 counts = 0;uint best = 0;
    if (x < width && y < height) {
        uint at = y*width+x;
        uint color = color_buffer[at];float depth = depth_buffer[at];
        [loop] for (uint j = 0; j < tile.count; ++j) {
            uint primitive=references[tile.first+j];
            /* Reject bbox misses before materializing the large varying and
             * combiner arrays. Narrow 3D triangles leave most tile lanes empty. */
            if((int)x<triangles[primitive].minx || (int)x>=triangles[primitive].maxx ||
               (int)y<triangles[primitive].miny || (int)y>=triangles[primitive].maxy)continue;
            precise float px=(float)x+0.5f, py=(float)y+0.5f;
            precise float ax=triangles[primitive].v[0].pos[0],ay=triangles[primitive].v[0].pos[1];
            precise float bx=triangles[primitive].v[1].pos[0],by=triangles[primitive].v[1].pos[1];
            precise float cx=triangles[primitive].v[2].pos[0],cy=triangles[primitive].v[2].pos[1];
            precise float e0=(bx-ax)*(py-ay)-(by-ay)*(px-ax);
            precise float e1=(cx-bx)*(py-by)-(cy-by)*(px-bx);
            precise float e2=(ax-cx)*(py-cy)-(ay-cy)*(px-cx);
            float area=triangles[primitive].area;
            if(!(gpu_edge(e0,area,triangles[primitive].inclusive[0]) &&
                 gpu_edge(e1,area,triangles[primitive].inclusive[1]) &&
                 gpu_edge(e2,area,triangles[primitive].inclusive[2])))continue;
            NvGpuTri tri = triangles[primitive];
            NvGpuPixel pixel = gpu_pixel(configuration[tri.state_index],tri,(int)x,(int)y,color,depth);
            color = pixel.color;depth = pixel.depth;
            counts.x += pixel.depth_fail;counts.y += pixel.pixels;counts.z += pixel.zpass;
            if ((pixel.pixel_max & 0xFFFFFFu) > (best & 0xFFFFFFu)) best = pixel.pixel_max;
        }
        color_buffer[at] = color;depth_buffer[at] = depth;
    }
    reductions[index] = counts;maxima[index] = best;
    GroupMemoryBarrierWithGroupSync();
    for (uint step = 32; step > 0; step >>= 1) {
        if (index < step) {
            reductions[index] += reductions[index+step];
            if ((maxima[index+step] & 0xFFFFFFu) > (maxima[index] & 0xFFFFFFu)) maxima[index] = maxima[index+step];
        }
        GroupMemoryBarrierWithGroupSync();
    }
    if (index == 0) {
        uint base=tile_index*4;
        InterlockedAdd(statistics[base],reductions[0].x);
        InterlockedAdd(statistics[base+1],reductions[0].y);
        InterlockedAdd(statistics[base+2],reductions[0].z);
        uint previous = statistics[base+3], observed;
        while ((maxima[0] & 0xFFFFFFu) > (previous & 0xFFFFFFu)) {
            InterlockedCompareExchange(statistics[base+3],previous,maxima[0],observed);
            if (observed == previous) break;
            previous = observed;
        }
    }
}
