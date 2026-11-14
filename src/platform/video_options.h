#ifndef XBOX_VIDEO_OPTIONS_H
#define XBOX_VIDEO_OPTIONS_H

#include <stdlib.h>
#include <string.h>

/* The same choice goes to the guest EEPROM and the host presenters. A
 * widescreen guest frame is anamorphic and still has 640x480 samples. */
static inline int xbox_video_widescreen(void)
{
    const char *value = getenv("RECOMP_VIDEO_ASPECT");
    return value && strcmp(value, "16:9") == 0;
}

#endif
