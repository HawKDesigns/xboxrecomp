#ifndef XBOX_FRAME_CLOCK_H
#define XBOX_FRAME_CLOCK_H
#include <stdint.h>

/* Fractional display clock. NTSC: 60000/1001 Hz = 3 ticks/50050 us;
 * PAL: 50 Hz. Wakes may be arbitrarily late without losing elapsed ticks. */
typedef struct XBOX_FRAME_CLOCK {
    uint64_t last_us;
    uint64_t fraction;
    uint32_t refresh;
    int initialized;
} XBOX_FRAME_CLOCK;

static inline uint64_t xbox_frame_clock_advance(XBOX_FRAME_CLOCK *clock,
                                               uint64_t now_us,
                                               uint32_t refresh)
{
    uint64_t delta, denominator, numerator, scaled, ticks;
    refresh = refresh == 50u ? 50u : 60u;
    if (!clock->initialized || now_us < clock->last_us
                           || refresh != clock->refresh) {
        clock->initialized = 1;
        clock->last_us = now_us;
        clock->fraction = 0;
        clock->refresh = refresh;
        return 0;
    }
    delta = now_us - clock->last_us;
    clock->last_us = now_us;
    numerator = refresh == 50u ? 50u : 3u;
    denominator = refresh == 50u ? 1000000u : 50050u;
    /* Divide first to keep the multiply bounded even across long stalls. */
    ticks = (delta / denominator) * numerator;
    scaled = (delta % denominator) * numerator + clock->fraction;
    ticks += scaled / denominator;
    clock->fraction = scaled % denominator;
    return ticks;
}
#endif
