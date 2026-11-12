/* Candidate hash only: gpu_raw_mesh_equal still proves every metadata field,
 * index and advertised stream byte before reuse. Keys are private to this run.
 * memcpy permits unaligned inputs; the tail reads exactly the remaining bytes.
 * No host pointers enter this hash. Unsigned arithmetic wraps deliberately. */
static uint64_t gpu_raw_word_hash(uint64_t h,const void *data,size_t bytes)
{
    if(!bytes)return h;
    const uint8_t *p=data;size_t left=bytes;
    h^=(uint64_t)bytes*0x9e3779b185ebca87ull;
    while(left>=8){
        uint64_t word;memcpy(&word,p,8);h^=word;h*=0x9e3779b185ebca87ull;
        h=(h<<27)|(h>>37);p+=8;left-=8;
    }
    if(left){uint64_t tail=0;memcpy(&tail,p,left);h^=tail;h*=0x9e3779b185ebca87ull;}
    h^=h>>30;h*=0xbf58476d1ce4e5b9ull;
    h^=h>>27;h*=0x94d049bb133111ebull;
    return h^(h>>31);
}
