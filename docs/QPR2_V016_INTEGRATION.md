# QPR2 trial of XboxRecomp v0.16.0

This experimental branch merges upstream v0.16.0 (`f0f8a461496a4f23d9b0aeaa284b8d70ba7ab82b`)
with the QPR2 v126 runtime branch. It does not replace the working v126 branch.

The merge kept QPR2's existing audio, FIFO executor, GPU, input, memory, and
video implementations where upstream changed the same files. It imported the
nonoverlapping upstream changes and ported QPR2's port I/O, WBINVD, and REP
STOS lifter handling onto the v0.16 lifter. The vertex interpreter applies
v0.16's color-output clamp to both the interpreted and QPR2 native fast paths.

The custom QPR2 FIFO scan is not the v0.16 DMA walker; the standalone upstream
`nv2a_pb_walk` fixture targets that walker and is not a valid test of this
branch. QPR2 retains its tested FIFO path pending a separate integration of
the DMA walker. The locally generated game-specific shader headers remain
ignored; fresh clones use generic fallbacks until the user generates or
installs those headers from their own copy.

Validation before publication: 691 recompiler tests passed (one skipped),
plus standalone NV2A vertex-shader and raster-batch C tests on Linux. The
full runtime and QPR2 race still require a Windows build and run. A QPR2 test
branch pins this commit so that it can be compared against v126 without
changing the normal project branch.
