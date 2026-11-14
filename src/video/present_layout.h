#ifndef XBOX_PRESENT_LAYOUT_H
#define XBOX_PRESENT_LAYOUT_H

#include <stdint.h>
#include "platform/video_options.h"

typedef enum {
    XBOX_PRESENT_LINEAR,
    XBOX_PRESENT_POINT,
    XBOX_PRESENT_UNSCALED
} XboxPresentFilter;

typedef struct {
    int x, y, width, height;
} XboxPresentRect;

static inline XboxPresentFilter xbox_present_filter(XboxPresentFilter fallback)
{
    const char *value = getenv("RECOMP_PRESENT_FILTER");
    if (value) {
        if (strcmp(value, "off") == 0)
            return XBOX_PRESENT_UNSCALED;
        if (value[0] == 'p' || value[0] == 'P')
            return XBOX_PRESENT_POINT;
        if (strcmp(value, "linear") == 0)
            return XBOX_PRESENT_LINEAR;
    }
    return fallback;
}

static inline int xbox_present_display_width(int width, int height, int widescreen)
{
    return widescreen ? (int)(((int64_t)height * 16 + 4) / 9) : width;
}

/* All sizes are positive. Off limits the rectangle to the native frame
 * height. The anamorphic correction still applies in 16:9. */
static inline XboxPresentRect xbox_present_rect(
    int client_width, int client_height, int frame_width, int frame_height,
    int widescreen, XboxPresentFilter filter)
{
    int ratio_width = widescreen ? 16 : frame_width;
    int ratio_height = widescreen ? 9 : frame_height;
    int width = client_width, height = client_height;

    if (filter == XBOX_PRESENT_UNSCALED && height > frame_height)
        height = frame_height;
    if ((int64_t)width * ratio_height <= (int64_t)height * ratio_width)
        height = (int)(((int64_t)width * ratio_height + ratio_width / 2) / ratio_width);
    else
        width = (int)(((int64_t)height * ratio_width + ratio_height / 2) / ratio_height);
    if (width < 1)
        width = 1;
    if (height < 1)
        height = 1;
    return (XboxPresentRect){(client_width - width) / 2, (client_height - height) / 2,
                             width, height};
}

#endif
