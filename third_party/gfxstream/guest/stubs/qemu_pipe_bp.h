// The emulator's QEMU pipe, which Stud does not have. Only goldfish gralloc's
// colour-buffer refcount pipe in egl.cpp names it, a path Stud's gralloc
// never takes; every pipe is invalid.
#pragma once

#include <cstddef>

typedef int QEMU_PIPE_HANDLE;
#define QEMU_PIPE_INVALID_HANDLE (-1)

static inline QEMU_PIPE_HANDLE qemu_pipe_open(const char*) { return QEMU_PIPE_INVALID_HANDLE; }
static inline bool qemu_pipe_valid(QEMU_PIPE_HANDLE h) { return h >= 0; }
static inline void qemu_pipe_close(QEMU_PIPE_HANDLE) {}
static inline int qemu_pipe_write(QEMU_PIPE_HANDLE, const void*, size_t) { return -1; }
static inline int qemu_pipe_read(QEMU_PIPE_HANDLE, void*, size_t) { return -1; }
static inline int qemu_pipe_write_fully(QEMU_PIPE_HANDLE, const void*, size_t) { return -1; }
static inline int qemu_pipe_read_fully(QEMU_PIPE_HANDLE, void*, size_t) { return -1; }
static inline bool qemu_pipe_try_again(int) { return false; }
