/* Whole-image conversion only after normal texture hazards/cache comparison.
 * Read the renderer's immutable snapshot, not guest memory. Unsupported layouts
 * return before writing and keep sample_tex as the reference/fallback path. */
static int gpu_texture_decode_image(void *opaque,const uint8_t *raw,size_t bytes,
                                    uint32_t *out,size_t capacity)
{
    const Texture *t=(const Texture*)opaque;
    if(!t || !t->valid || !raw || !out || !t->width || !t->height ||
       t->width>4096 || t->height>4096 || t->levels>16)return 0;
    unsigned faces=t->cube?6u:1u,w=t->width,h=t->height;
    size_t pixels=(size_t)w*h;
    if(pixels>capacity/faces)return 0;
    unsigned block=d3d8_format_dxt_block_bytes(t->color);
    int morton=t->color==0x06,linear=t->color==0x12;
    if(!block && !morton && !linear)return 0;
    if(morton && ((w&(w-1)) || (h&(h-1))))return 0;
    if(linear && t->pitch<(size_t)w*4)return 0;
    size_t needed=block?(size_t)((w+3)/4)*((h+3)/4)*block:
        (linear?(size_t)(h-1)*t->pitch+(size_t)w*4:pixels*4);
    size_t stride=tex_face_stride(t);
    if(stride<needed || needed>bytes || (faces-1)>((bytes-needed)/stride))return 0;
    uint32_t xoff[4096];
    if(morton)for(unsigned x=0;x<w;x++)xoff[x]=swizzle_offset(x,0,w,h);
    for(unsigned face=0;face<faces;face++) {
        const uint8_t *src=raw+face*stride;uint32_t *dst=out+face*pixels;
        if(linear) {
            for(unsigned y=0;y<h;y++)memcpy(dst+(size_t)y*w,src+(size_t)y*t->pitch,(size_t)w*4);
        }else if(morton) {
            for(unsigned y=0;y<h;y++) {
                unsigned yo=swizzle_offset(0,y,w,h);
                for(unsigned x=0;x<w;x++)memcpy(dst+(size_t)y*w+x,src+(size_t)(xoff[x]|yo)*4,4);
            }
        }else {
            unsigned bw=(w+3)/4,bh=(h+3)/4;
            for(unsigned by=0;by<bh;by++)for(unsigned bx=0;bx<bw;bx++) {
                uint32_t decoded[16];
                d3d8_dxt_decode_block(src+((size_t)by*bw+bx)*block,t->color,decoded);
                unsigned columns=w-bx*4<4?w-bx*4:4,rows=h-by*4<4?h-by*4:4;
                for(unsigned y=0;y<rows;y++)memcpy(dst+(size_t)(by*4+y)*w+bx*4,decoded+y*4,columns*4);
            }
        }
    }
    return 1;
}
