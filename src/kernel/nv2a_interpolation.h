#ifndef NV2A_INTERPOLATION_H
#define NV2A_INTERPOLATION_H

/* Affine interpolation in difference form preserves a constant depth exactly.
 * Summing three independently rounded weighted depths can vary a coplanar
 * UI surface by whole depth units at Z24 magnitudes, punching holes in it. */
static inline float nv2a_interpolate_depth(float za, float zb, float zc,
                                          float weight_b, float weight_c)
{
    return za + weight_b * (zb - za) + weight_c * (zc - za);
}
#endif
