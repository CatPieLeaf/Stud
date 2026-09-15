// Roblox's own log, on this process's stdout.
//
// The engine logs through Android's liblog, importing
// __android_log_write/print/buf_write directly, confirmed against its own
// exported symbols. On a device those reach logd. Stud has no logd, so
// these functions are interposed at the ELF symbol level: an exported
// symbol in this executable's own global scope wins over liblog.so's for
// every call any loaded library makes, which is ordinary ELF symbol
// resolution and the same technique pthread_create_interpose.cpp in this
// directory already uses. Each call is printed, then forwarded to the real
// implementation through RTLD_NEXT so nothing downstream changes.
//
// Nearly all of a session log's content arrives this way, and it is the
// only view of what the engine itself thinks is happening, FLog/DFLog
// categories and warnings and errors included. Nothing here is Stud's own
// tracing: the priority, tag and text are the engine's.

#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <dlfcn.h>
#include <cstring>

#include "stud/game_instance.h"

namespace {

// Real android/log.h priority levels (public NDK header values),
// duplicated here rather than included since process-b's headers target
// ANDROID_PLATFORM=26 and some of the real symbols below are guarded by
// __INTRODUCED_IN in a way that's easier to sidestep entirely, same
// reasoning as main.cpp's own dlsym-based __android_log_set_logger use.
const char* priority_name(int prio) {
    switch (prio) {
        case 2: return "V";
        case 3: return "D";
        case 4: return "I";
        case 5: return "W";
        case 6: return "E";
        case 7: return "F";
        default: return "?";
    }
}

}  // namespace

extern "C" int __android_log_write(int prio, const char* tag, const char* text) {
    if (text != nullptr) stud::jni_bridge::note_engine_log_line(text, std::strlen(text));
    std::printf("[roblox %s/%s]: %s\n", priority_name(prio), tag ? tag : "(null)",
                text ? text : "(null)");
    std::fflush(stdout);
    using RealFn = int (*)(int, const char*, const char*);
    static auto real = reinterpret_cast<RealFn>(::dlsym(RTLD_NEXT, "__android_log_write"));
    return real ? real(prio, tag, text) : 0;
}

extern "C" int __android_log_buf_write(int buf_id, int prio, const char* tag, const char* text) {
    if (text != nullptr) stud::jni_bridge::note_engine_log_line(text, std::strlen(text));
    std::printf("[roblox buf=%d %s/%s]: %s\n", buf_id, priority_name(prio),
                tag ? tag : "(null)", text ? text : "(null)");
    std::fflush(stdout);
    using RealFn = int (*)(int, int, const char*, const char*);
    static auto real = reinterpret_cast<RealFn>(::dlsym(RTLD_NEXT, "__android_log_buf_write"));
    return real ? real(buf_id, prio, tag, text) : 0;
}

extern "C" int __android_log_vprint(int prio, const char* tag, const char* fmt, va_list ap) {
    char buf[4096];
    va_list ap_copy;
    va_copy(ap_copy, ap);
    std::vsnprintf(buf, sizeof(buf), fmt ? fmt : "", ap_copy);
    va_end(ap_copy);
    stud::jni_bridge::note_engine_log_line(buf, std::strlen(buf));
    std::printf("[roblox %s/%s]: %s\n", priority_name(prio), tag ? tag : "(null)", buf);
    std::fflush(stdout);
    using RealFn = int (*)(int, const char*, const char*, va_list);
    static auto real = reinterpret_cast<RealFn>(::dlsym(RTLD_NEXT, "__android_log_vprint"));
    if (real) {
        va_list ap_forward;
        va_copy(ap_forward, ap);
        int result = real(prio, tag, fmt, ap_forward);
        va_end(ap_forward);
        return result;
    }
    return 0;
}

extern "C" int __android_log_print(int prio, const char* tag, const char* fmt, ...) {
    char buf[4096];
    va_list ap;
    va_start(ap, fmt);
    std::vsnprintf(buf, sizeof(buf), fmt ? fmt : "", ap);
    va_end(ap);
    stud::jni_bridge::note_engine_log_line(buf, std::strlen(buf));
    std::printf("[roblox %s/%s]: %s\n", priority_name(prio), tag ? tag : "(null)", buf);
    std::fflush(stdout);
    using RealFn = int (*)(int, const char*, const char*, va_list);
    static auto real = reinterpret_cast<RealFn>(::dlsym(RTLD_NEXT, "__android_log_vprint"));
    if (real) {
        va_list ap_forward;
        va_start(ap_forward, fmt);
        int result = real(prio, tag, fmt, ap_forward);
        va_end(ap_forward);
        return result;
    }
    return 0;
}

extern "C" void __android_log_assert(const char* cond, const char* tag, const char* fmt, ...) {
    char buf[4096];
    if (fmt) {
        va_list ap;
        va_start(ap, fmt);
        std::vsnprintf(buf, sizeof(buf), fmt, ap);
        va_end(ap);
    } else {
        std::snprintf(buf, sizeof(buf), "Assertion failed: %s", cond ? cond : "(null)");
    }
    std::printf("[roblox assert %s]: %s\n", tag ? tag : "(null)", buf);
    std::fflush(stdout);
    __android_log_write(7, tag, buf);
    std::abort();
}
