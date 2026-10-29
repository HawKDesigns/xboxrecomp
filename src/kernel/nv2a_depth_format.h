#ifndef NV2A_DEPTH_FORMAT_H
#define NV2A_DEPTH_FORMAT_H
#include <stdint.h>
#include <string.h>
#include <math.h>

/* Unsigned NV2A floating depth encodings. QPR2's retail D3D clear routine
 * packs F24 by selecting the high double word, adding 0xF8000000,
 * masking off four mantissa bits and shifting left four (00198770..8D).
 * Inverting that encoding gives the positive float bits below. F16 uses
 * the corresponding double high word >> 8, minus 0x8000 (0019873A..54).
 * These are not IEEE binary16 or normalized integer depth. */
static inline float nv2a_depth_clear(uint32_t format, uint32_t control0,
                                     uint32_t packed)
{
    uint32_t z16 = ((format >> 4) & 15) == 1;
    uint32_t value = z16 ? packed & 0xFFFFu : packed >> 8;
    uint32_t bits;
    float result;
    if (!(control0 & (1u << 12))) return (float)value;
    if (!value) return 0.0f;
    bits = z16 ? (value << 11) + 0x3C000000u : value << 7;
    memcpy(&result, &bits, sizeof(result));
    return result;
}

/* Compare/store in the surface's precision. Keeping all 23 IEEE mantissa
 * bits in an F24 surface makes nominally equal second passes fail EQUAL.
 * The F24 SDK encoding retains float bits 30..7; clear and draw must agree.
 * F16 draw conversion is not modelled here; retain its existing path. */
static inline float nv2a_depth_store(uint32_t format, uint32_t control0, float z)
{
    int z16 = ((format >> 4) & 15) == 1;
    uint32_t bits;
    if (!isfinite(z)) return z;
    if (control0 & (1u << 12)) {
        if (z16) return z;
        if (z <= 0) return 0;
        memcpy(&bits, &z, 4);
        bits &= ~127u;
        memcpy(&z, &bits, 4);
        return z;
    }
    if (z <= 0) return 0;
    if (z >= (z16 ? 65535.0f : 16777215.0f))
        return z16 ? 65535.0f : 16777215.0f;
    return (float)floor((double)z + 0.5);
}
#endif
