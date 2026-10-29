#include "nv2a_gpu_raster.h"
#include <string.h>

int nv_gpu_bin(const NvGpuTri *tri, unsigned n, uint32_t width, uint32_t height,
               NvGpuTile *tiles, unsigned *tile_count, uint32_t *refs,
               unsigned capacity, unsigned *ref_count)
{
    uint32_t count[NV_GPU_MAX_TILES] = {0}, cursor[NV_GPU_MAX_TILES];
    unsigned tw = (width + 15) / 16, th = (height + 15) / 16, total = 0, used = 0;
    *tile_count = *ref_count = 0;
    if (!width || !height || width > 1024 || height > 1024 || n > NV_GPU_MAX_TRIANGLES)
        return 0;
    for (unsigned i = 0; i < n; ++i) {
        const NvGpuTri *t = tri + i;
        if (t->minx < 0 || t->miny < 0 || t->maxx > (int)width || t->maxy > (int)height ||
            t->minx >= t->maxx || t->miny >= t->maxy) return 0;
        for (int y = t->miny / 16; y <= (t->maxy - 1) / 16; ++y)
            for (int x = t->minx / 16; x <= (t->maxx - 1) / 16; ++x)
                ++count[(unsigned)y * tw + x];
    }
    for (unsigned y = 0; y < th; ++y) for (unsigned x = 0; x < tw; ++x) {
        unsigned at = y * tw + x;
        cursor[at] = total;
        if (!count[at]) continue;
        if (count[at] > capacity - total) return 0;
        tiles[used].x = x * 16; tiles[used].y = y * 16;
        tiles[used].first = total; tiles[used++].count = count[at];
        total += count[at];
    }
    /* Original triangle order is preserved independently in each tile. */
    for (unsigned i = 0; i < n; ++i) {
        const NvGpuTri *t = tri + i;
        for (int y = t->miny / 16; y <= (t->maxy - 1) / 16; ++y)
            for (int x = t->minx / 16; x <= (t->maxx - 1) / 16; ++x)
                refs[cursor[(unsigned)y * tw + x]++] = i;
    }
    *tile_count = used; *ref_count = total;
    return 1;
}
