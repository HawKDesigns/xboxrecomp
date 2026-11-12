#ifndef NV2A_FRAME_CAPTURE_H
#define NV2A_FRAME_CAPTURE_H
#include <stdint.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
/* Copy one finished linear framebuffer. The returned pixels feed both the
 * window and BMP writer, so capture never re-reads a partially drawn target. */
static inline int nv2a_frame_copy(uint32_t *dst, const uint8_t *src,
    uint32_t pitch, uint32_t width, uint32_t height, uint32_t bpp)
{
    if (!dst || !src || !width || !height || (bpp!=2 && bpp!=4) ||
            pitch < (uint64_t)width*bpp) return 0;
    for (uint32_t y=0;y<height;y++) {
        const uint8_t *row=src+(size_t)y*pitch;
        uint32_t *out=dst+(size_t)y*width;
        if(bpp==4) memcpy(out,row,(size_t)width*4);
        else for(uint32_t x=0;x<width;x++) {
            uint16_t p;memcpy(&p,row+2*x,2);
            out[x]=(((p>>11)&31)*255/31<<16)|(((p>>5)&63)*255/63<<8)|(p&31)*255/31;
        }
    }
    return 1;
}
static inline int nv2a_frame_write_bmp(const char *path, const uint32_t *pixels,
                                      uint32_t width,uint32_t height)
{
    uint8_t header[54]={0}, *row;
    uint32_t row_bytes=(width*3+3)&~3u, image_bytes=row_bytes*height, size=54+image_bytes;
    FILE *file;
    int ok;
    if(!path || !pixels || !width || !height) return 0;
    row=(uint8_t*)calloc(1,row_bytes);if(!row)return 0;
    file=fopen(path,"wb");if(!file){free(row);return 0;}
    header[0]='B';header[1]='M';memcpy(header+2,&size,4);header[10]=54;header[14]=40;
    memcpy(header+18,&width,4);memcpy(header+22,&height,4);header[26]=1;header[28]=24;
    memcpy(header+34,&image_bytes,4);ok=fwrite(header,1,54,file)==54;
    for(uint32_t y=height;y-- >0 && ok;) {
        for(uint32_t x=0;x<width;x++) {
            uint32_t p=pixels[(size_t)y*width+x];
            row[3*x]=(uint8_t)p;row[3*x+1]=(uint8_t)(p>>8);row[3*x+2]=(uint8_t)(p>>16);
        }
        ok=fwrite(row,1,row_bytes,file)==row_bytes;
    }
    if(fclose(file)!=0)ok=0;
    free(row);return ok;
}
#endif
