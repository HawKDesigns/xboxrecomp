# QPR2 runtime branch

This branch starts at XboxRecomp v0.13.1 (`193e2995ffaec871bb3be43a43bd66aa5fccfeb1`)
and adds the runtime changes used by the QPR2 recompilation project. The
upstream history, LICENSE, NOTICE, CONTRIBUTORS.md, and LICENSES/ remain in
place. The QPR2 repository pins an exact commit of this branch as a submodule.

The additions cover NV2A pushbuffer execution and GPU rasterization, shader
handling, audio output, memory and video bridges, input, toolchain fixes, and
focused tests. This branch is not yet rebased or merged onto upstream v0.16.0.
That integration should be tested as a separate branch before changing QPR2's
submodule pointer.

## Local game-specific shader data

Two headers are intentionally ignored and absent from the public branch:

- `src/kernel/nv2a_vsh_compiled.h` contains exact-match vertex shader
  specializations generated from an extracted copy of the game archive. With
  the header absent, the vertex interpreter remains available. From your own
  archive, run `python tools/compile_qpr2_vertex_programs.py ATV.zip` in the
  QPR2 project to restore the tested fast path locally.
- `src/kernel/nv2a_gpu_literal_programs.h` contains combiner program shapes
  captured during a game run. With the header absent, the generic GPU material
  path remains available. If you already have this local header from your own
  build, keep it in this directory to restore the tested literal fast path.

The public fallback paths make fresh source builds possible; they can perform
differently from the QPR2 v126 configuration using locally generated headers.
Neither ignored header should be added to the public Git history by a blanket
`git add -f`.

The generated `nv2a_gpu_shader.h` is separate: it embeds this project's HLSL
sources and the QPR2 CMake build regenerates it when those sources change.
