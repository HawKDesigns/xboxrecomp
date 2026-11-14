#ifndef NV2A_OVERLAY_REVISION_H
#define NV2A_OVERLAY_REVISION_H
/* The startup gate and linked renderer must agree on the same revision.
 * 1705: the guest EEPROM, AV-pack query and presenter agree on the aspect
 * preference, allowing Xbox D3D to admit native widescreen modes. */
#define QPR2_RENDERER_REVISION 1705u
#endif
