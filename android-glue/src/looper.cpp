#include "stud/android_glue.h"
#include "stud/ndk_types.h"

#include <sys/epoll.h>
#include <sys/syscall.h>
#include <unistd.h>

#include <cstdio>
#include <cstdlib>

#include <algorithm>
#include <atomic>
#include <mutex>
#include <unordered_map>
#include <vector>

// Real epoll-based event loop -- the same underlying mechanism Android's
// own ALooper uses, not a fake. Built on epoll_create1/epoll_ctl/epoll_wait
// (already in libc-shim's safe-forward set).
struct ALooper {
    int epoll_fd;
    std::atomic<int> ref_count{1};

    struct CallbackEntry {
        ALooper_callbackFunc callback;
        void* data;
        int ident;
    };
    std::unordered_map<int, CallbackEntry> fd_callbacks;
};

namespace {
thread_local ALooper* t_looper = nullptr;

// Real registry of every ALooper ever created, independent of t_looper's
// per-thread lifetime -- see poll_orphaned_loopers_once()'s own doc
// comment (android_glue.h) for the full real reasoning: a thread that
// created a looper can exit (as native_app_glue's android_app_entry
// thread does, confirmed via its real source never calling
// ALooper_release()) while leaving a perfectly valid, still-open epoll
// fd behind -- this list is what lets a *different* thread find and
// poll it. Mutex-protected: ALooper_prepare() can run concurrently on
// different real bionic threads (already true in this codebase --
// nativeAppBridgeAppStart() alone spawns four).
std::mutex g_all_loopers_mutex;
std::vector<ALooper*> g_all_loopers;

// Real, stable, public NDK ABI shape (android_native_app_glue.h's
// `struct android_poll_source`, unchanged for over a decade) --
// reimplemented here from its documented layout, not copied from any
// vendored source, since poll_orphaned_loopers_once() needs to interpret
// a raw void* this shape without depending on the whole header. `app`
// and `source` are passed back to `process` as plain void* -- the real
// header's more specific `struct android_app*`/`struct
// android_poll_source*` types are opaque to Stud anyway, and process()
// itself (real native_app_glue code) does the real, correctly-typed
// cast internally.
struct AndroidPollSourceShape {
    int32_t id;
    int32_t _pad;
    void* app;
    void (*process)(void* app, void* source);
};

// Shared polling core: does one non-blocking-or-blocking epoll_wait
// against a specific looper (not necessarily the calling thread's own
// t_looper) and returns exactly what real ALooper_pollOnce would.
// ALooper_pollOnce() itself is now a thin wrapper over this.
int poll_once_on_looper(ALooper* looper, int timeoutMillis, int* outFd, int* outEvents,
                         void** outData) {
    if (looper == nullptr) return ALOOPER_POLL_ERROR;

    // Real, live-caught issue, fixed (the engineering notes, "fix the busy-loop
    // thread"): a real Roblox-internal worker thread calls this with
    // timeoutMillis=0 in a tight loop expecting some OTHER call each
    // iteration (a real vsync-paced swap, on a real device) to naturally
    // throttle it -- confirmed live via a live syscall trace pegging ~100% CPU on one
    // core indefinitely, calling epoll_pwait(fd,[],1,0,...) back-to-back
    // with nothing ready. A real Android device's own scheduler never
    // guarantees true zero-latency for a 0ms poll either, so a real app
    // requesting timeoutMillis=0 already has to tolerate *some* real
    // delay -- passing a tiny, still-effectively-"immediate" 1ms floor to
    // the real epoll_wait() here instead of a literal 0 turns an
    // unthrottled spin into a bounded, low-CPU poll, with no observable
    // behavioral difference to any caller (still returns
    // ALOOPER_POLL_TIMEOUT correctly the moment nothing is ready; every
    // other caller of this function -- e.g. poll_orphaned_loopers_once()'s
    // own already-documented 0-timeout call, itself only run once per
    // Stud's own ~250ms outer loop iteration -- is unaffected by an extra
    // millisecond of latency here).
    const int effective_timeout = (timeoutMillis == 0) ? 1 : timeoutMillis;

    while (true) {
        epoll_event ev{};
        int n = ::epoll_wait(looper->epoll_fd, &ev, 1, effective_timeout);
        if (n == 0) return ALOOPER_POLL_TIMEOUT;
        if (n < 0) return ALOOPER_POLL_ERROR;

        int fd = ev.data.fd;
        auto it = looper->fd_callbacks.find(fd);
        if (it == looper->fd_callbacks.end()) continue;

        int events = 0;
        if (ev.events & EPOLLIN) events |= ALOOPER_EVENT_INPUT;
        if (ev.events & EPOLLOUT) events |= ALOOPER_EVENT_OUTPUT;
        if (ev.events & EPOLLERR) events |= ALOOPER_EVENT_ERROR;
        if (ev.events & EPOLLHUP) events |= ALOOPER_EVENT_HANGUP;

        if (it->second.callback != nullptr) {
            // Matches real ALooper semantics: a registered callback
            // handles the event internally and pollOnce keeps polling
            // rather than returning to the caller. Returning 0 from the
            // callback means "remove this fd," matching the real API.
            int result = it->second.callback(fd, events, it->second.data);
            if (result == 0) {
                ::epoll_ctl(looper->epoll_fd, EPOLL_CTL_DEL, fd, nullptr);
                looper->fd_callbacks.erase(fd);
            }
            continue;
        }

        if (outFd != nullptr) *outFd = fd;
        if (outEvents != nullptr) *outEvents = events;
        if (outData != nullptr) *outData = it->second.data;
        return it->second.ident;
    }
}
}  // namespace

extern "C" {
namespace {

// Every thread the engine spawns prepares a looper, so this is one line
// per thread -- 157 of them in one session, and they only matter while
// chasing which looper a call bound to (the duplicate-libandroid bug).
bool looper_trace_enabled() {
    static const bool enabled = std::getenv("STUD_LOOPER_TRACE") != nullptr;
    return enabled;
}

}  // namespace


ALooper* ALooper_prepare(int /*opts*/) {
    if (t_looper != nullptr) {
        if (looper_trace_enabled())
            std::fprintf(stderr, "stud: [ALooper] ALooper_prepare() tid=%ld already have t_looper=%p\n",
                     static_cast<long>(::syscall(SYS_gettid)), static_cast<void*>(t_looper));
        return t_looper;
    }
    int epfd = ::epoll_create1(0);
    t_looper = new ALooper{epfd};
    if (looper_trace_enabled())
        std::fprintf(stderr, "stud: [ALooper] ALooper_prepare() tid=%ld created t_looper=%p\n",
                 static_cast<long>(::syscall(SYS_gettid)), static_cast<void*>(t_looper));
    {
        std::lock_guard<std::mutex> lock(g_all_loopers_mutex);
        g_all_loopers.push_back(t_looper);
    }
    return t_looper;
}

ALooper* ALooper_forThread() { return t_looper; }

void ALooper_acquire(ALooper* looper) {
    if (looper == nullptr) return;
    int new_ref = looper->ref_count.fetch_add(1) + 1;
    std::fprintf(stderr, "stud: [ALooper] ALooper_acquire(%p) tid=%ld new_ref=%d\n",
                 static_cast<void*>(looper), static_cast<long>(::syscall(SYS_gettid)), new_ref);
}

void ALooper_release(ALooper* looper) {
    // Real, confirmed-live gap (this session): some thread Roblox's own
    // code creates without going through Stud's interposed
    // pthread_create() (bypassing the per-thread ALooper_prepare() that
    // adds, e.g. a raw clone()-based thread pool a statically-linked
    // dependency spawns) ends up calling ALooper_release(
    // ALooper_forThread()) with a genuinely-null looper (t_looper is
    // thread_local and was never prepared on that thread) -- confirmed
    // live: fault address 0x4 exactly matches nullptr + offsetof(
    // ALooper, ref_count). Guarding here, in Stud's own reimplementation
    // (not a patch to Roblox's code), is the correct fix regardless of
    // which specific thread/call-site is responsible -- a real,
    // well-behaved ALooper_release should tolerate this rather than
    // requiring every caller to null-check first.
    if (looper == nullptr) return;
    int prev_ref = looper->ref_count.fetch_sub(1);
    std::fprintf(stderr, "stud: [ALooper] ALooper_release(%p) tid=%ld prev_ref=%d t_looper=%p\n",
                 static_cast<void*>(looper), static_cast<long>(::syscall(SYS_gettid)), prev_ref,
                 static_cast<void*>(t_looper));
    if (prev_ref == 1) {
        ::close(looper->epoll_fd);
        if (t_looper == looper) t_looper = nullptr;
        {
            std::lock_guard<std::mutex> lock(g_all_loopers_mutex);
            auto& all = g_all_loopers;
            all.erase(std::remove(all.begin(), all.end(), looper), all.end());
        }
        delete looper;
    }
}

int ALooper_addFd(ALooper* looper, int fd, int ident, int events, ALooper_callbackFunc callback,
                  void* data) {
    epoll_event ev{};
    if (events & ALOOPER_EVENT_INPUT) ev.events |= EPOLLIN;
    if (events & ALOOPER_EVENT_OUTPUT) ev.events |= EPOLLOUT;
    ev.data.fd = fd;
    if (::epoll_ctl(looper->epoll_fd, EPOLL_CTL_ADD, fd, &ev) != 0) return -1;
    looper->fd_callbacks[fd] = {callback, data, ident};
    return 1;
}

int ALooper_removeFd(ALooper* looper, int fd) {
    ::epoll_ctl(looper->epoll_fd, EPOLL_CTL_DEL, fd, nullptr);
    return looper->fd_callbacks.erase(fd) != 0 ? 1 : 0;
}

int ALooper_pollOnce(int timeoutMillis, int* outFd, int* outEvents, void** outData) {
    return poll_once_on_looper(t_looper, timeoutMillis, outFd, outEvents, outData);
}

}  // extern "C"

namespace stud::android_glue {

bool poll_orphaned_loopers_once() {
    // Snapshot under the lock, then poll outside it -- process() below
    // (real native_app_glue code) can itself legitimately call back into
    // ALooper_prepare()/other real android_glue functions that also take
    // g_all_loopers_mutex, so holding it across the dispatch would risk
    // a real, avoidable self-deadlock.
    std::vector<ALooper*> loopers;
    {
        std::lock_guard<std::mutex> lock(g_all_loopers_mutex);
        loopers = g_all_loopers;
    }

    bool dispatched_any = false;
    ALooper* calling_thread_looper = t_looper;
    for (ALooper* looper : loopers) {
        if (looper == calling_thread_looper) continue;  // already polled by the caller itself

        int out_fd = -1;
        int out_events = 0;
        void* out_data = nullptr;
        // Zero timeout -- this function is meant to be called once per
        // iteration of Stud's own real event loop, alongside that loop's
        // own (blocking-with-a-real-timeout) poll of its own looper; a
        // blocking wait here would stall that loop for orphaned loopers
        // that may never receive another event again.
        int ident = poll_once_on_looper(looper, /*timeoutMillis=*/0, &out_fd, &out_events, &out_data);
        if (ident < 0 || out_data == nullptr) continue;  // TIMEOUT/ERROR, or a callback-based fd

        auto* source = static_cast<AndroidPollSourceShape*>(out_data);
        if (source->process != nullptr) {
            // This file is itself real bionic code now (compiled by the
            // NDK toolchain, linked into Process B) -- source->process
            // (native_app_glue's own process_cmd, statically linked in
            // libroblox.so) is just another same-ABI, same-process
            // function pointer, called directly like any other.
            source->process(source->app, source);
            dispatched_any = true;
        }
    }
    return dispatched_any;
}

}  // namespace stud::android_glue
