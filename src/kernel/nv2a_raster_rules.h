#ifndef NV2A_RASTER_RULES_H
#define NV2A_RASTER_RULES_H
#include <stdint.h>
/* Projected framebuffer coordinates use y down. Positive area is clockwise.
 * The register values are defined by nv2a_regs.h and emitted by retail D3D. */
static inline int nv2a_face_culled(float area, uint32_t enabled,
                                  uint32_t face, uint32_t front)
{
    int is_front;
    if (!enabled) return 0;
    is_front = front == 0x901u ? area < 0 : area > 0;
    return face == 0x408u || (face == 0x404u && is_front) ||
           (face == 0x405u && !is_front);
}
/* In either winding, each shared edge belongs to exactly one triangle. */
static inline int nv2a_edge_inclusive(const float a[2], const float b[2], float area)
{
    float dx=b[0]-a[0], dy=b[1]-a[1];
    if (area < 0) { dx=-dx; dy=-dy; }
    return dy < 0 || (dy == 0 && dx > 0);
}
static inline int nv2a_edge_inside(float edge, float area, int inclusive)
{
    return (area > 0 ? edge > 0 : edge < 0) || (edge == 0 && inclusive);
}
#endif
