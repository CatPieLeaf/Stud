#include "flight_recorder.h"

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <vector>

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
uint32_t this_thread() {
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

}  // namespace

bool enabled() {
    static const bool on = std::getenv("STUD_FLIGHT_RECORDER") != nullptr;
    return on;
}

void record(Event e, uint64_t a, uint64_t b, uint64_t c) {
    if (!enabled()) return;
    const uint64_t slot = g_next.fetch_add(1, std::memory_order_relaxed);
    Entry& t = ring()[slot % kCapacity];
    t.when = std::chrono::steady_clock::now();
    t.a = a;
    t.b = b;
    t.c = c;
    t.thread = this_thread();
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
