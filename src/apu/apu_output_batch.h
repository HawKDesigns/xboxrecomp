#ifndef APU_OUTPUT_BATCH_H
#define APU_OUTPUT_BATCH_H
#include <stdint.h>
#include <string.h>

/* Four distinct DSP blocks per device buffer. Keep a full pending batch if
 * the device is temporarily full; never overwrite samples still in flight. */
#define APU_OUTPUT_BATCH_SAMPLES 1024
typedef struct APUOutputBatch {
    int16_t samples[APU_OUTPUT_BATCH_SAMPLES][2];
    int count;
} APUOutputBatch;
typedef int (*APUOutputEmit)(void *, const int16_t *, int);

static int apu_output_batch_push(APUOutputBatch *batch, const int16_t *samples,
                                int count, APUOutputEmit emit, void *context)
{
    if (!samples || count <= 0 || count > APU_OUTPUT_BATCH_SAMPLES) return 0;
    if (batch->count == APU_OUTPUT_BATCH_SAMPLES) {
        if (!emit(context, &batch->samples[0][0], batch->count)) return 0;
        batch->count = 0;
    }
    if (count > APU_OUTPUT_BATCH_SAMPLES - batch->count) return 0;
    memcpy(batch->samples[batch->count], samples, (size_t)count * 2 * sizeof(int16_t));
    batch->count += count;
    if (batch->count == APU_OUTPUT_BATCH_SAMPLES &&
        emit(context, &batch->samples[0][0], batch->count)) batch->count = 0;
    return 1; /* Accepted into owned storage, including a pending full batch. */
}
#endif
