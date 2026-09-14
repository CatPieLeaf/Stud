// Real, live-value diagnostic (the engineering notes, "GameActivity_
// initializeNativeCode returns NULL" entry): a live one-shot breakpoint
// confirmed the null-return branch is gated on real, empty-string
// checks read via a thread-local scratch buffer (0x29a47f0, a real
// pthread_getspecific()-keyed helper). Static static analysis xref search for
// the real TLS key's own storage global (0x6f4eb88) found zero other
// references, a known class of static analysis gap for indirect/PIC-relative
// addressing, not a real "nothing else touches this" fact.
//
// Interposes pthread_key_create/pthread_setspecific directly, same
// real ELF-symbol-level technique already proven in this codebase
// (pthread_create_interpose.cpp, android_log_interpose.cpp), logs
// every real key created and every real value stored into any TLS
// slot during a live run. If the specific key this check's own reader
// uses is ever pthread_setspecific()'d anywhere in the real process
// (by libroblox.so's own real code, or found to never happen at all,
// itself a real, definitive answer), this makes it directly visible
// with no external tool and no root needed.

#include <cstdio>
#include <cstdlib>
#include <dlfcn.h>
#include <pthread.h>

namespace {

// Off unless asked for. This interpose sits on a path the engine uses
// constantly: it printed 853 lines of a single launch, none of which
// says anything unless someone is actively chasing that TLS key. The
// interpose itself stays in place. It costs a branch, so turning it
// on is a relaunch with STUD_TLS_TRACE=1 rather than a rebuild.
bool tls_trace_enabled() {
    static const bool enabled = [] {
        const char* value = std::getenv("STUD_TLS_TRACE");
        return value != nullptr && *value == '1';
    }();
    return enabled;
}

}  // namespace

extern "C" int pthread_key_create(pthread_key_t* key, void (*destructor)(void*)) {
    using RealFn = int (*)(pthread_key_t*, void (*)(void*));
    static auto real = reinterpret_cast<RealFn>(::dlsym(RTLD_NEXT, "pthread_key_create"));
    int result = real(key, destructor);
    if (tls_trace_enabled())
        std::fprintf(stderr, "stud: [TLS] pthread_key_create() -> key=%u result=%d destructor=%p\n",
                 static_cast<unsigned>(*key), result, reinterpret_cast<void*>(destructor));
    return result;
}

extern "C" int pthread_setspecific(pthread_key_t key, const void* value) {
    using RealFn = int (*)(pthread_key_t, const void*);
    static auto real = reinterpret_cast<RealFn>(::dlsym(RTLD_NEXT, "pthread_setspecific"));
    if (tls_trace_enabled()) {
        std::fprintf(stderr, "stud: [TLS] pthread_setspecific(key=%u, value=%p)\n",
                     static_cast<unsigned>(key), value);
    }
    return real(key, value);
}
