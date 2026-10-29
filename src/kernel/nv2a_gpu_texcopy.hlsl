/* GPU-resident ARGB texture handoff. No arithmetic or guest memory writes. */
StructuredBuffer<uint2> completed_surface : register(t0);
StructuredBuffer<uint> parameters : register(t1);
RWStructuredBuffer<uint> packed_color : register(u0);
[numthreads(64,1,1)]
void main(uint3 id : SV_DispatchThreadID)
{
    if(id.x<parameters[0])packed_color[id.x]=completed_surface[id.x].x;
}
