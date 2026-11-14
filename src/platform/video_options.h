#ifndef XBOX_VIDEO_OPTIONS_H
#define XBOX_VIDEO_OPTIONS_H

#include <stdlib.h>
#include <string.h>

/* The dashboard preference and host output must agree for anamorphic Xbox
 * modes. Keep the old guest-only switch as an alias for manual launches. */
static inline int xbox_video_widescreen(void)
{
    const char *value = getenv("RECOMP_VIDEO_ASPECT");
    if (value && strcmp(value, "16:9") == 0)
        return 1;
    if (value && strcmp(value, "4:3") == 0)
        return 0;
    const char *legacy = getenv("RECOMP_GUEST_WIDESCREEN");
    return legacy && strcmp(legacy, "1") == 0;
}

#endif
