/* The generic material renderer remains available without local captures. */
#ifndef NV2A_GPU_LITERAL_FALLBACK_H
#define NV2A_GPU_LITERAL_FALLBACK_H
#define NV_GPU_LITERAL_CLASSES 3u
static unsigned nv_gpu_literal_program(const Nv2aCombiner *rc)
{
    (void)rc;
    return 0;
}
static void nv_gpu_literal_fixture(Nv2aCombiner *rc, unsigned program, unsigned variant)
{
    (void)rc;
    (void)program;
    (void)variant;
}
#endif
