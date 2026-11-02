// AOSP's libhardware gralloc header: only the usage bits gfxstream's guest
// names, and the pixel formats it carries in.
#pragma once

#include <system/graphics.h>

enum {
    GRALLOC_USAGE_SW_READ_OFTEN = 0x00000003,
    GRALLOC_USAGE_SW_WRITE_OFTEN = 0x00000030,
    GRALLOC_USAGE_HW_TEXTURE = 0x00000100,
    GRALLOC_USAGE_HW_RENDER = 0x00000200,
    GRALLOC_USAGE_HW_COMPOSER = 0x00000800,
    GRALLOC_USAGE_HW_FB = 0x00001000,
};
