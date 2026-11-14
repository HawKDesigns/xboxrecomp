#ifndef NV2A_RENDER_SCALE_H
#define NV2A_RENDER_SCALE_H

#define NV2A_SCANOUT_WIDTH 640u
#define NV2A_SCANOUT_HEIGHT 480u
#define NV2A_MAX_RENDER_SCALE 3u
#define NV2A_HOST_DEPTH_STRIDE 1024u

/* The center member of each sample group is the original guest sample.
 * An even factor shifts the raster viewport by half a host pixel so that
 * the uniform grid includes the guest's exact half-pixel position. */
static inline unsigned nv2a_native_sample(unsigned pixel, unsigned scale)
{
    return pixel * scale + scale / 2;
}

static inline float nv2a_scaled_sample(unsigned pixel, unsigned scale)
{
    return (float)(pixel / scale) + 0.5f +
           ((float)(pixel % scale) - (float)(scale / 2)) / (float)scale;
}

#endif
