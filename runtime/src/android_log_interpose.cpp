// Real, live-caught investigation this session (the engineering notes, "FLog
// output has never appeared" gap): the earlier assumption that
// __android_log_set_logger(__android_log_stderr_logger) (main.cpp) was
// confirmed working, based on real linker warnings and [JNIVM] diagnostic
// lines appearing in Stud's own captured output, turns out to be a false
// correlation, checked directly this session: [JNIVM] lines are a
// plain printf() (libjnivm's own internal/log.h, no HAVE_LOGGER defined
// in this build), nothing to do with liblog at all. Whether the
// registered logger callback ever actually receives libroblox.so's own
// real FLog calls (confirmed, against the library's exported symbols, to import
// __android_log_write/print/buf_write directly) was never actually
// verified independently.
//
// This interposes the real __android_log_write/print/buf_write/vprint
// family at the ELF symbol level, same real technique already proven
// by pthread_create_interpose.cpp in this same directory: a real,
// exported symbol in this executable's own global scope takes priority
// over real liblog.so's own definition for every call any loaded
// library (including libroblox.so) makes, standard ELF symbol
// resolution. Every call is printed directly to this process's own
// stdout (bypassing __android_log_set_logger's registered callback
// entirely, so this works regardless of whatever that mechanism is or
// isn't doing), then forwarded to the real underlying liblog
// implementation via RTLD_NEXT so real behavior (including the
// stderr-logger redirect main.cpp already sets up) is unaffected.
//
// This directly answers the long-open question: does libroblox.so's own
// FLog machinery call these functions at all, and if so, with what
// priority/tag/content, something no prior technique in this project
// (static analysis, planted breakpoints) could observe
// without it either already being visible in liblog's own output.

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
    std::printf("[FLOG-INTERPOSE %s/%s]: %s\n", priority_name(prio), tag ? tag : "(null)",
                text ? text : "(null)");
    std::fflush(stdout);
    using RealFn = int (*)(int, const char*, const char*);
    static auto real = reinterpret_cast<RealFn>(::dlsym(RTLD_NEXT, "__android_log_write"));
    return real ? real(prio, tag, text) : 0;
}

extern "C" int __android_log_buf_write(int buf_id, int prio, const char* tag, const char* text) {
    if (text != nullptr) stud::jni_bridge::note_engine_log_line(text, std::strlen(text));
    std::printf("[FLOG-INTERPOSE buf=%d %s/%s]: %s\n", buf_id, priority_name(prio),
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
    std::printf("[FLOG-INTERPOSE %s/%s]: %s\n", priority_name(prio), tag ? tag : "(null)", buf);
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
    std::printf("[FLOG-INTERPOSE %s/%s]: %s\n", priority_name(prio), tag ? tag : "(null)", buf);
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
    std::printf("[FLOG-INTERPOSE ASSERT %s]: %s\n", tag ? tag : "(null)", buf);
    std::fflush(stdout);
    __android_log_write(7, tag, buf);
    std::abort();
}
