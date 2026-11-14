#ifndef NV2A_OVERLAY_REVISION_H
#define NV2A_OVERLAY_REVISION_H
/* The startup gate and linked renderer must agree on the same revision.
 * 1703: the guest EEPROM and host presenters share the launcher aspect
 * setting, and the presenters support all three picture scaling modes. */
#define QPR2_RENDERER_REVISION 1703u
#endif
