#include "flight_recorder.h"

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <thread>
#include <vector>

#include <dirent.h>
#include <sys/syscall.h>
#include <unistd.h>

namespace stud::render_host::fr {

namespace {

struct Entry {
    std::chrono::steady_clock::time_point when;
    uint64_t a = 0;
    uint64_t b = 0;
    uint64_t c = 0;
    uint32_t thread = 0;
    Event event = Event::Note;
};

// 256K entries, about 12MB. Large on purpose: at a few thousand events a
// second that is minutes of history, and the interesting question is
// always "what was happening BEFORE", sometimes well before. A debug
// build can afford the memory; a lost run cannot be repeated.
constexpr size_t kCapacity = 1u << 18;

std::vector<Entry>& ring() {
    static std::vector<Entry> r(kCapacity);
    return r;
}

std::atomic<uint64_t> g_next{0};

// Writing is lock-free by construction: each writer claims one slot with
// a fetch_add and touches nothing else. A dump can therefore race a
// writer and read a half-written entry -- accepted deliberately, because
// the alternative is a lock on the path being measured. At worst one
// line of a dump is nonsense.
uint32_t current_tid() {
    static thread_local const uint32_t id =
        static_cast<uint32_t>(::syscall(SYS_gettid));
    return id;
}

const char* name_of(Event e) {
    switch (e) {
        case Event::Submit: return "submit";
        case Event::WaitBegin: return "wait-begin";
        case Event::WaitEnd: return "wait-end";
        case Event::ResetFences: return "reset-fences";
        case Event::AcquireBegin: return "acquire-begin";
        case Event::AcquireEnd: return "acquire-end";
        case Event::PresentBegin: return "present-begin";
        case Event::PresentEnd: return "present-end";
        case Event::Barrier: return "barrier";
        case Event::SwapchainNew: return "swapchain-new";
        case Event::SwapchainGone: return "swapchain-gone";
        case Event::DeviceNew: return "device-new";
        case Event::DeviceLost: return "DEVICE-LOST";
        case Event::UpscaleSubmit: return "upscale-submit";
        case Event::MappedFlush: return "mapped-flush";
        case Event::Note: return "note";
    }
    return "?";
}

std::mutex& dump_mutex() {
    static std::mutex m;
    return m;
}

// Copies a small /proc file into the dump.
void quote_proc(std::FILE* out, const char* path, const char* label, int max_lines) {
    std::FILE* in = std::fopen(path, "r");
    if (in == nullptr) return;
    std::fprintf(out, "  [%s]\n", label);
    char line[512];
    int n = 0;
    while (n < max_lines && std::fgets(line, sizeof(line), in) != nullptr) {
        std::fprintf(out, "    %s", line);
        ++n;
    }
    std::fclose(in);
}

// WHERE EVERY THREAD IS SLEEPING, at the moment of the trigger.
//
// This is the one measurement the system lag has always needed and never
// got. A fence wait that takes 300ms and then SUCCEEDS, with the GPU at
// 9%, means the work finished and the wakeup was late -- and the only
// thing that distinguishes a late wakeup in the driver from one in the
// scheduler, the compositor socket or a futex is which kernel function
// each thread is parked in.
//
// It was going to be a script the user ran during an episode. That is a
// bad plan: the episode is unpredictable, the evidence dies with the
// process, and asking someone to catch it by hand is what made this
// investigation unbearable. The process can read its own /proc, so it
// does, automatically, at exactly the right instant.
void dump_system_state(std::FILE* out) {
    std::fprintf(out, "  ---- system state at the trigger ----\n");
    quote_proc(out, "/proc/loadavg", "loadavg", 1);
    quote_proc(out, "/proc/pressure/cpu", "pressure/cpu", 2);
    quote_proc(out, "/proc/pressure/memory", "pressure/memory", 2);
    quote_proc(out, "/proc/pressure/io", "pressure/io", 2);

    // Context switches say whether this process is being descheduled a
    // lot, which a wakeup-latency problem would show and a busy-GPU one
    // would not.
    std::FILE* st = std::fopen("/proc/self/status", "r");
    if (st != nullptr) {
        char line[512];
        while (std::fgets(line, sizeof(line), st) != nullptr) {
            if (std::strncmp(line, "Threads:", 8) == 0 ||
                std::strncmp(line, "VmRSS:", 6) == 0 ||
                std::strncmp(line, "VmSwap:", 7) == 0 ||
                std::strncmp(line, "voluntary_ctxt", 14) == 0 ||
                std::strncmp(line, "nonvoluntary_ctxt", 17) == 0) {
                std::fprintf(out, "    %s", line);
            }
        }
        std::fclose(st);
    }

    std::fprintf(out, "  [threads: tid state wchan name]\n");
    // opendir, not popen: this runs while the device is lost or a fence
    // is stuck, and forking a shell from a process in that state is a
    // way to turn a diagnostic into a second failure.
    DIR* dir = ::opendir("/proc/self/task");
    if (dir == nullptr) return;
    int shown = 0;
    while (shown < 96) {
        const dirent* ent = ::readdir(dir);
        if (ent == nullptr) break;
        if (ent->d_name[0] == '.') continue;
        const char* tid = ent->d_name;
        char path[256];
        char wchan[128] = "?";
        char comm[128] = "?";
        char state[8] = "?";
        std::snprintf(path, sizeof(path), "/proc/self/task/%s/wchan", tid);
        if (std::FILE* f = std::fopen(path, "r")) {
            if (std::fgets(wchan, sizeof(wchan), f) == nullptr) std::strcpy(wchan, "?");
            std::fclose(f);
        }
        std::snprintf(path, sizeof(path), "/proc/self/task/%s/comm", tid);
        if (std::FILE* f = std::fopen(path, "r")) {
            if (std::fgets(comm, sizeof(comm), f) != nullptr) {
                if (char* c = std::strchr(comm, '\n')) *c = '\0';
            }
            std::fclose(f);
        }
        // The state is the field after the LAST ')': a thread name can
        // contain spaces and brackets, and Stud's do, which shifts every
        // column of /proc/<tid>/stat and makes field 3 the wrong thing.
        std::snprintf(path, sizeof(path), "/proc/self/task/%s/stat", tid);
        if (std::FILE* f = std::fopen(path, "r")) {
            char buf[1024];
            if (std::fgets(buf, sizeof(buf), f) != nullptr) {
                const char* close = std::strrchr(buf, ')');
                if (close != nullptr && close[1] != '\0' && close[2] != '\0') {
                    state[0] = close[2];
                    state[1] = '\0';
                }
            }
            std::fclose(f);
        }
        std::fprintf(out, "    %-8s %-2s %-26s %s\n", tid, state, wchan, comm);
        ++shown;
    }
    ::closedir(dir);
}

}  // namespace

bool enabled() {
    static const bool on = std::getenv("STUD_FLIGHT_RECORDER") != nullptr;
    return on;
}

namespace {

// NOTICES A STALL THAT IS NOT INSIDE ANY VULKAN CALL.
//
// Every other trigger here measures the duration of one call: a fence
// wait, a present, a submit. That misses an entire shape of failure, and
// it did so immediately -- a real session hung for several seconds while
// loading a game and not one threshold fired, because the time was going
// into write() from a too-chatty diagnostic, between calls rather than
// inside one. The freeze being hunted may have the same shape: if the
// whole render host stops, nothing that times individual calls will ever
// see it.
//
// So this watches progress itself. Events stop arriving, the watchdog
// notices, and the dump captures the history and every thread's kernel
// wait state WHILE THE STALL IS STILL HAPPENING -- which is the one
// moment that has never been observed.
void watchdog_loop() {
    using namespace std::chrono_literals;
    uint64_t last_seen = 0;
    auto last_change = std::chrono::steady_clock::now();
    bool reported = false;
    for (;;) {
        std::this_thread::sleep_for(100ms);
        const uint64_t now_count = g_next.load(std::memory_order_relaxed);
        const auto now = std::chrono::steady_clock::now();
        if (now_count != last_seen) {
            last_seen = now_count;
            last_change = now;
            reported = false;
            continue;
        }
        // A window that is minimised or a session sitting on a menu can
        // legitimately go quiet, so this must not cry wolf: it reports
        // once per stall, and only after the gap is long enough to be a
        // freeze rather than a pause between frames.
        const double still = std::chrono::duration<double>(now - last_change).count();
        if (!reported && last_seen > 0 && still > 1.0) {
            reported = true;
            std::printf("stud-render-host: NO PROGRESS for %.1fs -- the render host has stopped "
                        "doing anything at all, which no per-call timer can see\n",
                        still);
            std::fflush(stdout);
            dump("the render host stopped making progress");
        }
    }
}

void start_watchdog_once() {
    static std::once_flag once;
    std::call_once(once, [] {
        std::thread(watchdog_loop).detach();
    });
}

}  // namespace

void record(Event e, uint64_t a, uint64_t b, uint64_t c) {
    if (!enabled()) return;
    // STUD_FLIGHT_RECORDER_SELFTEST=1 forces one dump early in the run.
    //
    // The whole point of this machinery is a session that cannot be
    // repeated, and a recorder that turns out to be broken at the moment
    // it was needed is worse than none: the run is spent and the user
    // has been asked for the one thing they said they would not give
    // again. So it proves itself on every startup it is asked to --
    // file creation, thread enumeration, formatting and all -- while
    // there is still time to fix it.
    static const bool selftest = std::getenv("STUD_FLIGHT_RECORDER_SELFTEST") != nullptr;
    if (selftest) {
        static std::atomic<bool> done{false};
        if (g_next.load(std::memory_order_relaxed) > 200 &&
            !done.exchange(true, std::memory_order_relaxed)) {
            dump("self test: proving the recorder works before it is needed");
        }
    }
    start_watchdog_once();
    const uint64_t slot = g_next.fetch_add(1, std::memory_order_relaxed);
    Entry& t = ring()[slot % kCapacity];
    t.when = std::chrono::steady_clock::now();
    t.a = a;
    t.b = b;
    t.c = c;
    t.thread = current_tid();
    t.event = e;
}

void dump(const char* why) {
    if (!enabled()) return;
    // One at a time, and not too many. A device loss arrives with a
    // crowd of consequent failures, and each of them would otherwise
    // dump the same history and push the first one out of the terminal.
    std::lock_guard<std::mutex> lock(dump_mutex());
    static int dumps = 0;
    if (dumps >= 12) return;
    ++dumps;

    const uint64_t total = g_next.load(std::memory_order_relaxed);
    if (total == 0) return;
    // How far back to go. Enough to cover several seconds of frames
    // without producing a file nobody will read.
    constexpr uint64_t kWindow = 6000;
    const uint64_t count = total < kWindow ? total : kWindow;
    const uint64_t first = total - count;
    const auto now = std::chrono::steady_clock::now();

    std::FILE* file = nullptr;
    const char* path = std::getenv("STUD_FLIGHT_RECORDER_PATH");
    if (path != nullptr) {
        // Appended, never truncated: the second dump matters as much as
        // the first, and a freeze is not the moment to discover the file
        // was overwritten.
        file = std::fopen(path, "a");
    }

    auto emit = [&](std::FILE* out) {
        std::fprintf(out,
                     "\n==== stud flight recorder: %s ====\n"
                     "the last %llu of %llu events, oldest first, ms BEFORE the trigger\n",
                     why, static_cast<unsigned long long>(count),
                     static_cast<unsigned long long>(total));
        for (uint64_t i = first; i < total; ++i) {
            const Entry& t = ring()[i % kCapacity];
            const double ago =
                std::chrono::duration<double, std::milli>(now - t.when).count();
            std::fprintf(out, "  -%9.3fms t%-7u %-15s %llx %llx %llx\n", ago, t.thread,
                         name_of(t.event), static_cast<unsigned long long>(t.a),
                         static_cast<unsigned long long>(t.b),
                         static_cast<unsigned long long>(t.c));
        }
        dump_system_state(out);
        std::fprintf(out, "==== end of flight recorder (%s) ====\n", why);
        std::fflush(out);
    };

    emit(stdout);
    if (file != nullptr) {
        emit(file);
        std::fclose(file);
    }
}

}  // namespace stud::render_host::fr
