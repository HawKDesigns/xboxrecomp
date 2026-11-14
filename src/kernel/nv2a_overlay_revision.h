#ifndef NV2A_OVERLAY_REVISION_H
#define NV2A_OVERLAY_REVISION_H
/* The startup gate and linked renderer must agree on the same revision.
 * 1704: the Windows rasterizer and presenters support 2x/3x internal render
 * resolution for eligible 640x480 scanout targets. */
#define QPR2_RENDERER_REVISION 1704u
#endif
