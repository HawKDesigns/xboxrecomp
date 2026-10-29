/* CPU storage planning only. A connected union of overlapping advertised
 * stream ranges is copied once. Original stride/type/size and every advertised
 * byte, including stride padding, remain part of exact mesh identity.
 * Disjoint objects are never read across, even if their addresses are adjacent. */
typedef struct {uintptr_t first;size_t bytes;unsigned leader,offset;} NvRawSpan;
typedef struct {NvRawSpan spans[16];unsigned count,offset[16];size_t bytes,logical,unique;} NvRawStreamPlan;
static int gpu_raw_stream_plan(const NvRawInput *in,NvRawStreamPlan *p,int combine)
{
    memset(p,0,sizeof *p);
    for(unsigned a=0;a<16;a++){
        const NvRawStream *s=&in->stream[a];uintptr_t first=(uintptr_t)s->data;
        if(s->bytes>4u*1024u*1024u || (s->bytes && (!s->data || s->bytes>UINTPTR_MAX-first)))return 0;
        p->logical+=s->bytes;
        if(!combine){
            p->offset[a]=(unsigned)p->bytes;
            if(s->bytes)p->spans[p->count++]=(NvRawSpan){first,s->bytes,a,(unsigned)p->bytes};
            p->bytes+=(s->bytes+11u)&~(size_t)3u;p->unique+=s->bytes;
        }else if(s->bytes)p->spans[p->count++]=(NvRawSpan){first,s->bytes,a,0};
    }
    if(!combine)return 1;
    /* At most sixteen spans. Sorting/merging avoids byte-wise repacking and
     * keeps every original attribute's contiguous byte view intact. */
    for(unsigned i=1;i<p->count;i++){
        NvRawSpan v=p->spans[i];unsigned j=i;
        while(j && p->spans[j-1].first>v.first){p->spans[j]=p->spans[j-1];--j;}p->spans[j]=v;
    }
    unsigned n=0;
    for(unsigned i=0;i<p->count;i++){
        NvRawSpan s=p->spans[i];
        if(n && s.first<p->spans[n-1].first+p->spans[n-1].bytes){
            NvRawSpan *last=p->spans+n-1;uintptr_t end=s.first+s.bytes;
            if(end>last->first+last->bytes)last->bytes=(size_t)(end-last->first);
            if(s.leader<last->leader)last->leader=s.leader;
        }else p->spans[n++]=s;
    }
    p->count=n;
    /* Canonical group order follows first attribute, never absolute address.
     * Relocation of a whole mesh can still reuse its immutable resident copy. */
    for(unsigned i=1;i<n;i++){
        NvRawSpan v=p->spans[i];unsigned j=i;
        while(j && p->spans[j-1].leader>v.leader){p->spans[j]=p->spans[j-1];--j;}p->spans[j]=v;
    }
    for(unsigned i=0;i<n;i++){
        NvRawSpan *s=p->spans+i;s->offset=(unsigned)p->bytes;
        p->bytes+=(s->bytes+11u)&~(size_t)3u;p->unique+=s->bytes;
        for(unsigned a=0;a<16;a++)if(in->stream[a].bytes){
            uintptr_t first=(uintptr_t)in->stream[a].data;
            if(first>=s->first && first-s->first<=s->bytes && in->stream[a].bytes<=s->bytes-(first-s->first))
                p->offset[a]=s->offset+(unsigned)(first-s->first);
        }
    }
    if(!p->bytes)p->bytes=8; /* Keep a valid padded zero/default attribute view. */
    return 1;
}
