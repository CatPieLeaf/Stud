// AOSP's atrace macros, as no-ops.
#pragma once

#define ATRACE_TAG_GRAPHICS 0
#define ATRACE_BEGIN(name) ((void)0)
#define ATRACE_END() ((void)0)
#define ATRACE_CALL() ((void)0)
#define ATRACE_NAME(name) ((void)0)
#define ATRACE_INT(name, value) ((void)0)
#define ATRACE_ENABLED() 0

#include <cstdint>
static inline void atrace_int(uint64_t /*tag*/, const char* /*name*/, int32_t /*value*/) {}
static inline void atrace_begin(uint64_t /*tag*/, const char* /*name*/) {}
static inline void atrace_end(uint64_t /*tag*/) {}
