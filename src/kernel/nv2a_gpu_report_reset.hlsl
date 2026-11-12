/* A visibility-counter reset is an ordered GPU command, not a CPU readback.
 * Preserve rendered pixels, depth and the other diagnostic counters. */
RWStructuredBuffer<uint> statistics : register(u2);
[numthreads(64,1,1)]
void main(uint3 id : SV_DispatchThreadID)
{
    uint elements,stride;statistics.GetDimensions(elements,stride);
    if(id.x<elements/4)statistics[id.x*4+2]=0;
}
