#pragma once

#include <fcntl.h>
#include <pthread.h>
#include <signal.h>
// execinfo.h is glibc's. bionic has no backtrace(), and Process B
// does not use this reporter anyway; it has trap_recovery, which
// recovers rather than reports.
#if defined(__GLIBC__)
#include <execinfo.h>
#endif
#include <unistd.h>

#include <cerrno>
#include <chrono>
#include <condition_variable>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <string>

// One log file per Stud session, written by all three processes at once.
//
// Stud's diagnostics have only ever existed in two places: a terminal, if
// the user happened to start it from one, or the systemd journal, if the
// launch came from the desktop entry. Neither is something a user can be
// asked to produce when something goes wrong, and the journal keeps no
// record at all on a system without persistent journald.
//
// So each process tees its own stdout and stderr into the file named by
// STUD_LOG_FILE, and keeps writing to wherever they already went. The
// terminal still shows everything; the journal still gets everything.
//
// Why a tee and not a plain redirect: redirecting would take the output
// away from whoever was already reading it, which is exactly the
// behaviour every debugging session in this project depends on.
//
// Why each process opens the file rather than inheriting one pipe from
// Process A: Process A exits within a second of launching the other two
// (by design; see its own "hand off and get out of the way" note), so
// a pipe it owned would be dead for the rest of the session. O_APPEND
// makes concurrent writers safe, the kernel places each write at the
// current end of file, so lines from three processes interleave but
// never overwrite each other.
namespace stud::logging {

namespace detail {

// The session log file itself, so the crash path can write it one last
// time without going through the pipe and its timer. See final_flush().
inline int& log_fd_storage() {
    static int fd = -1;
    return fd;
}

// The real stdout, saved before the tee replaced fd 1. Children must be
// given THIS rather than inheriting the pipe: the pipe's only reader is
// this process's tee thread, and Process A exits within a second of
// spawning the other two, after which their writes would hit a pipe
// nobody reads, and their output would vanish from the terminal and the
// journal. They open the log file themselves.
inline int& passthrough_fd_storage() {
    static int fd = -1;
    return fd;
}

struct TeeState {
    int read_fd = -1;
    int log_fd = -1;
    int passthrough_fd = -1;
};

// Everything read out of the pipe and not yet written to a file.
//
// This exists because the obvious tee -- read a block, write it to both
// destinations, repeat -- puts file I/O on the only path that drains the
// pipe. When a destination is slow (a terminal doing its own scrolling, a
// disk busy elsewhere), the drain stalls, the pipe fills at 64 KiB, and
// then every printf in the process BLOCKS, because a write to a full
// pipe waits. Live-caught by bisect: commit 3a71e33 added one printf on
// the swapchain-rebuild path, which runs on the engine's render thread,
// and the result was seconds of total freeze on a game join -- no
// presents, the window and MangoHud both frozen on the last frame, while
// the game's audio carried on. Nothing about that commit was wrong
// except where it logged, which is the proof that the logger must never
// be able to block its writers.
//
// So the reader does nothing but move bytes into memory, and a second
// thread writes them out on a timer. A writer can now only ever be
// delayed by a memcpy.
struct Buffered {
    std::mutex mutex;
    std::condition_variable wake;
    std::string pending;
    unsigned long long dropped = 0;
    bool reader_done = false;
};

inline Buffered& buffered() {
    static Buffered b;
    return b;
}

// The cap on what is held in memory. Reached only if a destination stops
// accepting writes for seconds at a time, and the alternative to dropping
// is growing without limit until the process is killed, which is worse
// than losing log lines. The oldest go first and the count is reported in
// the file, so a gap is never silent.
inline constexpr size_t kMaxBuffered = 8u * 1024u * 1024u;

// How long the writer sleeps between flushes when nothing is urgent.
inline constexpr int kFlushIntervalMs = 2000;

// Flush early rather than sit on a backlog this size.
inline constexpr size_t kFlushThreshold = 256u * 1024u;

inline void write_all(int fd, const char* data, size_t len) {
    if (fd < 0) return;
    size_t written = 0;
    while (written < len) {
        const ssize_t n = ::write(fd, data + written, len - written);
        if (n <= 0) {
            if (n < 0 && errno == EINTR) continue;
            return;
        }
        written += static_cast<size_t>(n);
    }
}

// Moves bytes from the pipe into memory, and does nothing else. No file
// I/O belongs here; see Buffered.
inline void* tee_thread(void* arg) {
    auto* state = static_cast<TeeState*>(arg);
    // Bigger than the old 4 KiB: fewer wakeups for the same bytes, and
    // the pipe is drained in larger bites during a burst.
    char buffer[64 * 1024];
    for (;;) {
        const ssize_t got = ::read(state->read_fd, buffer, sizeof(buffer));
        if (got < 0) {
            if (errno == EINTR) continue;
            break;
        }
        if (got == 0) break;
        Buffered& b = buffered();
        {
            std::lock_guard<std::mutex> lock(b.mutex);
            if (b.pending.size() + static_cast<size_t>(got) > kMaxBuffered) {
                // Oldest first. Whole buffer rather than a partial line
                // boundary search: this path means the log is already
                // losing, and spending time on tidiness here would make
                // it lose more.
                b.dropped += b.pending.size();
                b.pending.clear();
            }
            b.pending.append(buffer, static_cast<size_t>(got));
        }
        b.wake.notify_one();
    }
    {
        Buffered& b = buffered();
        std::lock_guard<std::mutex> lock(b.mutex);
        b.reader_done = true;
    }
    buffered().wake.notify_one();
    ::close(state->read_fd);
    return nullptr;
}

// Everything still in memory, written out now instead of at the next
// tick of the timer.
//
// The crash path's whole problem is that it has no next tick: the
// handler re-raises and the process is gone in microseconds, taking
// however many seconds of buffered log with it. So the handler calls
// this, and the buffer reaches the file before the process dies.
//
// try_lock rather than lock, because a signal can arrive on the very
// thread already holding the mutex, and waiting for a lock that the
// interrupted thread can no longer release would hang the crash handler
// forever -- turning a crash into a freeze. Losing the tail of a log is
// the better failure.
inline void final_flush() {
    const int fd = log_fd_storage();
    if (fd < 0) return;
    Buffered& b = buffered();
    if (!b.mutex.try_lock()) return;
    if (!b.pending.empty()) {
        write_all(fd, b.pending.data(), b.pending.size());
        b.pending.clear();
    }
    b.mutex.unlock();
}

// Writes what the reader collected, on a timer. Slow destinations delay
// only this thread.
inline void* writer_thread(void* arg) {
    auto* state = static_cast<TeeState*>(arg);
    Buffered& b = buffered();
    for (;;) {
        std::string batch;
        unsigned long long dropped = 0;
        bool finished = false;
        {
            std::unique_lock<std::mutex> lock(b.mutex);
            b.wake.wait_for(lock, std::chrono::milliseconds(kFlushIntervalMs), [&b] {
                return b.reader_done || b.pending.size() >= kFlushThreshold;
            });
            batch.swap(b.pending);
            dropped = b.dropped;
            b.dropped = 0;
            finished = b.reader_done && batch.empty();
        }
        if (dropped > 0) {
            char note[160];
            const int n = std::snprintf(
                note, sizeof(note),
                "stud: session log: %llu byte(s) dropped, the log was being written slower "
                "than it was produced\n",
                dropped);
            if (n > 0) {
                write_all(state->passthrough_fd, note, static_cast<size_t>(n));
                write_all(state->log_fd, note, static_cast<size_t>(n));
            }
        }
        if (!batch.empty()) {
            // Both destinations get every byte. A short or failed write
            // to one is not worth losing the other over; this is a
            // logger, and it must never be the reason a process stalls
            // or dies.
            write_all(state->passthrough_fd, batch.data(), batch.size());
            write_all(state->log_fd, batch.data(), batch.size());
        }
        if (finished) break;
    }
    delete state;
    return nullptr;
}

}  // namespace detail


// Tees this process's stdout and stderr into `path`. Safe to call with an
// empty path (does nothing) and safe to call when the file cannot be
// opened, output simply keeps going where it already went.
inline bool start_session_log(const std::string& path) {
    if (path.empty()) return false;
    const int log_fd = ::open(path.c_str(), O_WRONLY | O_CREAT | O_APPEND | O_CLOEXEC, 0600);
    if (log_fd < 0) return false;

    int pipe_fds[2] = {-1, -1};
    if (::pipe(pipe_fds) != 0) {
        ::close(log_fd);
        return false;
    }

    // Keep the real stdout so the tee can still write to it after fd 1
    // has been replaced by the pipe.
    const int passthrough = ::dup(STDOUT_FILENO);
    detail::passthrough_fd_storage() = passthrough;
    // What final_flush() writes to when the process is crashing.
    detail::log_fd_storage() = log_fd;

    // A bigger pipe, so an ordinary burst never even reaches the memory
    // buffer. 64 KiB is the default and a game join produces more than
    // that between two reads. Best-effort: the request is capped by
    // /proc/sys/fs/pipe-max-size and failing it costs nothing, the
    // buffer behind it is what actually guarantees the writer never
    // blocks.
    ::fcntl(pipe_fds[1], F_SETPIPE_SZ, 1024 * 1024);

    // Two threads, and the split is the whole point: `tee_thread` only
    // moves bytes out of the pipe, `writer_thread` does every write that
    // can be slow. See detail::Buffered.
    auto* reader_state = new detail::TeeState{pipe_fds[0], log_fd, passthrough};
    auto* writer_state = new detail::TeeState{-1, log_fd, passthrough};
    pthread_t writer{};
    if (::pthread_create(&writer, nullptr, detail::writer_thread, writer_state) != 0) {
        delete reader_state;
        delete writer_state;
        ::close(pipe_fds[0]);
        ::close(pipe_fds[1]);
        ::close(log_fd);
        if (passthrough >= 0) ::close(passthrough);
        return false;
    }
    ::pthread_detach(writer);

    pthread_t thread{};
    if (::pthread_create(&thread, nullptr, detail::tee_thread, reader_state) != 0) {
        // The writer is already running and owns writer_state; it stops
        // on its own once the read end closes and the reader marks it
        // done. Marking that here is what lets it exit rather than
        // waiting out its timer forever on a log that will never start.
        {
            std::lock_guard<std::mutex> lock(detail::buffered().mutex);
            detail::buffered().reader_done = true;
        }
        detail::buffered().wake.notify_one();
        delete reader_state;
        ::close(pipe_fds[0]);
        ::close(pipe_fds[1]);
        if (passthrough >= 0) ::close(passthrough);
        return false;
    }
    ::pthread_detach(thread);

    ::dup2(pipe_fds[1], STDOUT_FILENO);
    ::dup2(pipe_fds[1], STDERR_FILENO);
    ::close(pipe_fds[1]);

    // Line buffering, so a crash cannot swallow the last few hundred
    // lines still sitting in libc's buffer, stdout is a pipe now, and
    // a pipe is block-buffered by default. This project has already been
    // misled once by a long-lived process whose diagnostics never
    // appeared because of exactly that.
    ::setvbuf(stdout, nullptr, _IOLBF, 0);
    ::setvbuf(stderr, nullptr, _IONBF, 0);
    return true;
}

// The real stdout this process had before start_session_log() replaced
// it, or -1 if no tee is running. Hand this to a child as its stdout and
// stderr so it writes where this process's output really goes.
inline int passthrough_stdout_fd() { return detail::passthrough_fd_storage(); }

// The path Process A put in the environment for this session, if any.
inline std::string session_log_path_from_env() {
    const char* value = std::getenv("STUD_LOG_FILE");
    return (value != nullptr && *value != '\0') ? value : std::string();
}

namespace detail {

inline const char*& crash_process_name() {
    static const char* name = "stud";
    return name;
}

// Async-signal-safe: write(2) and _exit/raise only, no allocation, no
// printf. Anything else here would be a second crash inside the handler
// for the first one, a mistake this project has already made once, in
// its own backtrace walker.
inline void write_hex(unsigned long long value) {
    char buf[19] = {'0', 'x'};
    int at = 2;
    bool started = false;
    for (int shift = 60; shift >= 0; shift -= 4) {
        const int nibble = static_cast<int>((value >> shift) & 0xf);
        if (nibble == 0 && !started && shift != 0) continue;
        started = true;
        buf[at++] = static_cast<char>(nibble < 10 ? '0' + nibble : 'a' + nibble - 10);
    }
    ::write(STDERR_FILENO, buf, static_cast<size_t>(at));
}

// A one-line breadcrumb the crash handler prints, if anything has set
// one. Plain storage rather than a std::string: the handler runs in a
// signal context, where allocating is not allowed and the value has to
// be readable however broken the process is.
inline char* crash_note_storage() {
    static char note[192] = {0};
    return note;
}

// The same breadcrumb, without formatting anything until it is needed.
//
// set_crash_note() costs two snprintf calls -- the caller's, and the
// "%s" copy inside it -- and vk_cmd_record was calling it for EVERY
// Vulkan command the engine records, thousands per frame. Formatted
// string work on that path is not a breadcrumb, it is a frame budget.
//
// So the values are stored raw, which is four plain stores, and the
// formatting happens in the handler, where it is paid once and only if
// the process is dying anyway. `what` must be a string literal: nothing
// is copied.
struct CrashFields {
    const char* what;
    unsigned long long a;
    unsigned long long b;
    unsigned long long c;
};

inline CrashFields* crash_fields_storage() {
    static CrashFields fields{nullptr, 0, 0, 0};
    return &fields;
}

inline void crash_handler(int sig, siginfo_t* info, void*) {
    const char* name = crash_process_name();
    char digits[3] = {static_cast<char>('0' + (sig / 10) % 10),
                      static_cast<char>('0' + sig % 10), '\n'};
    const char* prefix = "stud: CRASH in ";
    const char* middle = ", signal ";
    ::write(STDERR_FILENO, prefix, ::strlen(prefix));
    ::write(STDERR_FILENO, name, ::strlen(name));
    ::write(STDERR_FILENO, middle, ::strlen(middle));
    ::write(STDERR_FILENO, digits, sizeof(digits));

    // Where, not just that.
    //
    // A report that says only "signal 11" costs a round trip to the person
    // who hit it, and they may not be able to reproduce it on demand. The
    // faulting address and the stack are what turn a crash log into a
    // diagnosis, and they are cheap enough to always print.
    if (info != nullptr) {
        const char* at = "stud: fault address ";
        ::write(STDERR_FILENO, at, ::strlen(at));
        write_hex(reinterpret_cast<unsigned long long>(info->si_addr));
        ::write(STDERR_FILENO, "\n", 1);
    }

    if (crash_note_storage()[0] != '\0') {
        const char* label = "stud: last: ";
        ::write(STDERR_FILENO, label, ::strlen(label));
        ::write(STDERR_FILENO, crash_note_storage(), ::strlen(crash_note_storage()));
        ::write(STDERR_FILENO, "\n", 1);
    }

    // The unformatted form, printed with the same write/hex primitives
    // the rest of this handler uses, since snprintf is not safe here.
    if (const CrashFields* f = crash_fields_storage(); f->what != nullptr) {
        const char* label = "stud: last: ";
        ::write(STDERR_FILENO, label, ::strlen(label));
        ::write(STDERR_FILENO, f->what, ::strlen(f->what));
        const char* sep = " ";
        ::write(STDERR_FILENO, sep, 1);
        write_hex(f->a);
        ::write(STDERR_FILENO, sep, 1);
        write_hex(f->b);
        ::write(STDERR_FILENO, sep, 1);
        write_hex(f->c);
        ::write(STDERR_FILENO, "\n", 1);
    }

    // backtrace() can allocate the first time it runs, which is not
    // allowed here, install_crash_reporter() calls it once up front so
    // that by now it cannot. backtrace_symbols_fd writes with write(2)
    // and allocates nothing, unlike backtrace_symbols.
#if defined(__GLIBC__)
    void* frames[32];
    const int count = ::backtrace(frames, 32);
    if (count > 0) {
        const char* header = "stud: backtrace:\n";
        ::write(STDERR_FILENO, header, ::strlen(header));
        ::backtrace_symbols_fd(frames, count, STDERR_FILENO);
    }
#endif

    // Everything above went to stderr, which is the pipe, so none of it
    // is in the log file yet -- nor is whatever the session had buffered
    // before the crash. Write it all now, because after the re-raise
    // below there is no later.
    //
    // The pause first is for the reader thread, which sits blocked in
    // read() and wakes the moment the report above is written: it needs
    // a moment to move those bytes into the buffer that final_flush()
    // writes out. nanosleep is one of the few sleeps that is
    // async-signal-safe, which is why it rather than anything friendlier.
    {
        timespec settle{};
        settle.tv_nsec = 5 * 1000 * 1000;  // 5ms
        ::nanosleep(&settle, nullptr);
    }
    final_flush();

    // The real disposition, then re-raise, so the exit status and any
    // core dump are exactly what the signal would have produced.
    ::signal(sig, SIG_DFL);
    ::raise(sig);
}

}  // namespace detail
// Leaves a breadcrumb for the crash handler to print. Overwritten each
// time, so it names the last thing attempted rather than keeping a
// history nothing would read.
//
// NOT for a hot path, whatever this comment used to say. It formats
// twice -- once in the caller and once here -- and it was being called
// per Vulkan command. Use set_crash_fields() there instead.
inline void set_crash_note(const char* text) {
    char* note = detail::crash_note_storage();
    std::snprintf(note, 192, "%s", text);
}

// The hot-path form: four stores, no formatting. `what` must outlive the
// call, so pass a string literal. See detail::CrashFields.
inline void set_crash_fields(const char* what, unsigned long long a = 0,
                             unsigned long long b = 0, unsigned long long c = 0) {
    detail::CrashFields* f = detail::crash_fields_storage();
    f->what = what;
    f->a = a;
    f->b = b;
    f->c = c;
}

// Names this process in the session log if it dies of a fatal signal.
// The engine's own process has a far more capable trap handler
// (trap_recovery.h, which recovers rather than reports); this is for the
// two glibc processes, where a crash otherwise left nothing in the log
// at all and the window simply vanished.
inline void install_crash_reporter(const char* process_name) {
    detail::crash_process_name() = process_name;
    // Warm backtrace() up now, while allocating is still allowed: its
    // first call resolves and may allocate, and doing that inside a
    // signal handler is how a crash reporter becomes a second crash.
#if defined(__GLIBC__)
    void* warmup[4];
    (void)::backtrace(warmup, 4);
#endif

    struct sigaction sa {};
    sa.sa_sigaction = detail::crash_handler;
    sa.sa_flags = SA_SIGINFO | SA_RESETHAND;
    ::sigemptyset(&sa.sa_mask);
    for (int sig : {SIGSEGV, SIGBUS, SIGILL, SIGFPE, SIGABRT}) {
        ::sigaction(sig, &sa, nullptr);
    }
}

inline bool start_session_log_from_env() {
    return start_session_log(session_log_path_from_env());
}

}  // namespace stud::logging
