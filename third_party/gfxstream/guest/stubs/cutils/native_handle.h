// AOSP's native_handle_t, as system/core/libcutils declares it. The NDK does
// not ship it; gfxstream's gralloc interface names it.
#pragma once

typedef struct native_handle {
    int version; /* sizeof(native_handle_t) */
    int numFds;  /* number of file-descriptors at &data[0] */
    int numInts; /* number of ints at &data[numFds] */
    int data[0];
} native_handle_t;
