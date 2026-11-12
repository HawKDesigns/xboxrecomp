/* Ordered native surface clear. Each lane owns one paired color/depth pixel.
 * This runs between draws, preserving the channel not being cleared and all
 * visibility/statistics counters. No guest completion is signalled here. */
StructuredBuffer<uint> parameters : register(t0);
RWStructuredBuffer<uint2> surface : register(u0);
[numthreads(64,1,1)]
void main(uint3 id : SV_DispatchThreadID)
{
    uint width=parameters[0],height=parameters[1];
    uint left=parameters[2],top=parameters[3];
    uint right=parameters[4],bottom=parameters[5],mask=parameters[6];
    uint span=right-left,count=span*(bottom-top);
    if(id.x>=count)return;
    uint x=left+id.x%span,y=top+id.x/span;
    if(x>=width || y>=height)return;
    uint at=y*width+x;
    uint2 value=surface[at];
    if(mask&1)value.x=parameters[7];
    if(mask&2)value.y=parameters[8];
    surface[at]=value;
}
