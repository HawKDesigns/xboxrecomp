# QPR2 runtime on XboxRecomp v0.17.0

This fork branch starts at upstream v0.17.0, commit
`343b6d8901723b37ce568b2418c5df4e1fe1d223`. The QPR2 port uses the v126
source at `f39fdf00ab65ffb1f6c3ccc18280885f1a5e6f09` as the implementation
reference and `92a5f030d0e9442d35e7fdfb8cb86344716c677f` as the last
hardware-tested runtime. The QPR2 repository pins the integrated runtime
commit as a submodule.

- NV2A uses QPR2's registered ring, bounded FIFO slices, ordered GPU completion,
  native D3D11 ROV, raw geometry and target coherency. Upstream's DMA walker,
  flip hold and software raster batching overlap that path and need separate
  work to integrate. The v0.17 decoded vertex interpreter handles unsupported
  programs while preserving v126 vertex colors at the CPU/GPU boundary.
- QPR2's elapsed display clock drives native XMV independently of completed
  presents. Upstream's vblank interrupt acknowledgement remains available.
  The Windows presenter publishes QPR2's resolved framebuffer with the
  v0.17 frame lock and keeps the close guard and optional F8 capture.
- The runtime keeps QPR2's 64 MiB tagged bus mapping, OHCI enumeration,
  controller fallback, wide PCM mixer, rate converter and HRTF routing.
  The v0.17 audio backend batches periods and keeps its queue pacing and
  device recovery. The QPR2 launcher requests 1,024-sample buffers and six
  slots for the initial comparison.
- The lifter exposes QPR2's source-addressed port I/O and host cache fence
  through `--runtime-instruction-hooks`. `--track-guest-pc` emits guest block
  markers. Define `RECOMP_ENABLE_GUEST_PC` when compiling a diagnostic build
  to record those addresses at runtime.

## Local shader generation

The QPR2 project regenerates `nv2a_vsh_compiled.h`,
`nv2a_gpu_literal_programs.h` and `nv2a_gpu_literal.inc` from the user's own
game archive and local runtime observations. These files and the embedded
`nv2a_gpu_shader.h` are ignored. A fresh clone builds with the generic shader
fallbacks. See the QPR2 project's [shader preparation guide](https://github.com/HawKDesigns/qpr2-recomp/blob/clean-v1-v017/PUBLISHING.md).

The Windows retail boot sequence, ROV activation and race performance need
hardware validation against the tested v126 reference.
