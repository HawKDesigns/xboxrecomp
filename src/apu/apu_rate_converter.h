#ifndef APU_RATE_CONVERTER_H
#define APU_RATE_CONVERTER_H
#include <math.h>
#include <string.h>

typedef int (*APURateRead)(void *context, float samples[][2], int requested);
typedef struct APUVoiceRateConverter {
    float input[32][2], current[2], next[2];
    int input_at, input_count, have_current, have_next, started;
    double phase;
} APUVoiceRateConverter;

static inline int apu_rate_read_one(APUVoiceRateConverter *state, float sample[2],
                             APURateRead read, void *context)
{
    if (state->input_at == state->input_count) {
        int count = read(context, state->input, 32);
        if (count <= 0) return 0;
        state->input_at = 0;
        state->input_count = count;
    }
    memcpy(sample, state->input[state->input_at++], sizeof(float) * 2);
    return 1;
}

/* ratio is output frames per source frame, as used by the MCPX pitch path.
 * Preserve fractional position and prefetched samples across APU slices. */
static inline int apu_rate_convert(APUVoiceRateConverter *state, float output[][2],
                            int requested, double ratio,
                            APURateRead read, void *context)
{
    int count = 0;
    double step;
    if (!isfinite(ratio) || ratio <= 0 || requested <= 0) return 0;
    step = 1.0 / ratio;
    state->started = 1;
    if (!state->have_current) {
        if (!apu_rate_read_one(state, state->current, read, context)) return 0;
        state->have_current = 1;
        /* A source that temporarily ran dry resumes at the next frame that
         * was pending, rather than skipping that newly available frame. */
        if (state->phase >= 1.0) state->phase -= 1.0;
    }
    while (count < requested) {
        while (state->phase >= 1.0) {
            if (state->have_next) {
                memcpy(state->current, state->next, sizeof(state->current));
                state->have_next = 0;
            } else if (!apu_rate_read_one(state, state->current, read, context)) {
                state->have_current = 0;
                return count;
            }
            state->phase -= 1.0;
        }
        if (state->phase > 0 && !state->have_next)
            state->have_next = apu_rate_read_one(state, state->next, read, context);
        for (int channel = 0; channel < 2; ++channel) {
            float next = state->have_next ? state->next[channel] : state->current[channel];
            output[count][channel] = state->current[channel] +
                (float)state->phase * (next - state->current[channel]);
        }
        ++count;
        state->phase += step;
    }
    return count;
}
#endif
