#ifndef NV2A_OVERLAY_REVISION_H
#define NV2A_OVERLAY_REVISION_H
/* The startup gate and linked renderer must agree on the same revision.
 * 1702: the title calls platform/thread_cpu.h and the UTF-8 frame-trace
 * start; the POSIX window takes resolved frames. A 1701 title or runtime
 * does not pair with this one. */
#define QPR2_RENDERER_REVISION 1702u
#endif
