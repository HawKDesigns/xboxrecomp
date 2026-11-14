#ifndef XBOX_VIDEO_OPTIONS_H
#define XBOX_VIDEO_OPTIONS_H

#include <stdlib.h>
#include <string.h>

/* Select the output rectangle. Console video flags are independent because
 * some lifted titles cannot initialize their widescreen display mode. */
static inline int xbox_video_widescreen(void)
{
    const char *value = getenv("RECOMP_VIDEO_ASPECT");
    return value && strcmp(value, "16:9") == 0;
}

#endif
