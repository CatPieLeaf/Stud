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
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

#include <cerrno>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
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

inline void* tee_thread(void* arg) {
    auto* state = static_cast<TeeState*>(arg);
    char buffer[4096];
    for (;;) {
        const ssize_t got = ::read(state->read_fd, buffer, sizeof(buffer));
        if (got < 0) {
            if (errno == EINTR) continue;
            break;
        }
        if (got == 0) break;
        // Both destinations get every byte. A short or failed write to
        // one is not worth losing the other over; this is a logger,
        // and it must never be the reason a process stalls or dies.
        for (int fd : {state->passthrough_fd, state->log_fd}) {
            if (fd < 0) continue;
            ssize_t written = 0;
            while (written < got) {
                const ssize_t n = ::write(fd, buffer + written, static_cast<size_t>(got - written));
                if (n <= 0) {
                    if (n < 0 && errno == EINTR) continue;
                    break;
                }
                written += n;
            }
        }
    }
    ::close(state->read_fd);
    delete state;
    return nullptr;
}

}  // namespace detail


// Where this process's output goes, once it has been teed.
//
// Two destinations exist and they differ only in the fd: Process A opens
// the log FILE, while B and C open a SOCKET to Process A. The tee thread
// below does not care which: it write()s a chunk and moves on.
//
// A write to that socket is a copy into a kernel buffer, with no disk
// behind it, so a process is never made to wait on file I/O to say
// something. Batching happens once, in Process A, on its way to the
// file. Nothing is held back on this side, so nothing can be lost in
// transit: by the time a process dies, every line it ever produced is
// already in A's hands.
inline bool start_session_log_to_fd(int destination_fd);

// Tees this process's stdout and stderr into `path`. Safe to call with an
// empty path (does nothing) and safe to call when the file cannot be
// opened, output simply keeps going where it already went.
inline bool start_session_log(const std::string& path) {
    if (path.empty()) return false;
    const int log_fd = ::open(path.c_str(), O_WRONLY | O_CREAT | O_APPEND | O_CLOEXEC, 0600);
    if (log_fd < 0) return false;
    return start_session_log_to_fd(log_fd);
}

// The socket Process A collects on. Named beside the log it feeds.
inline std::string session_log_socket_path_from_env() {
    const char* value = std::getenv("STUD_LOG_SOCKET");
    return (value != nullptr && *value != '\0') ? value : std::string();
}

// Connects to Process A's collector and tees into it. Used by B and C.
inline bool start_session_log_dispatch(const std::string& socket_path) {
    if (socket_path.empty()) return false;
    const int fd = ::socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (fd < 0) return false;
    sockaddr_un addr{};
    addr.sun_family = AF_UNIX;
    if (socket_path.size() + 1 > sizeof(addr.sun_path)) {
        ::close(fd);
        return false;
    }
    std::memcpy(addr.sun_path, socket_path.c_str(), socket_path.size() + 1);
    if (::connect(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) {
        ::close(fd);
        return false;
    }
    return start_session_log_to_fd(fd);
}

inline bool start_session_log_to_fd(int log_fd) {
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

    auto* state = new detail::TeeState{pipe_fds[0], log_fd, passthrough};
    pthread_t thread{};
    if (::pthread_create(&thread, nullptr, detail::tee_thread, state) != 0) {
        delete state;
        ::close(pipe_fds[0]);
        ::close(pipe_fds[1]);
        ::close(log_fd);
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

// Where a crash leaves a note for Process A to find.
//
// Next to the session log, so the two travel together. Built before the
// crash rather than inside the handler: nothing in a signal handler may
// allocate or call snprintf, so the path has to already exist.
inline char* crash_marker_path_storage() {
    static char path[4096] = {0};
    return path;
}

// And the note a NORMAL shutdown leaves.
//
// The crash marker alone was the wrong way round: it can only report the
// faults it was taught to catch, and a process that is killed outright --
// SIGKILL, the OOM killer, a driver taking it down -- writes nothing at
// all and looks exactly like a clean exit. So the rule is inverted. A
// shutdown the user asked for says so; anything else that ends the
// session is a crash by default, whether or not a handler ever ran.
inline char* clean_exit_marker_path_storage() {
    static char path[4096] = {0};
    return path;
}

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

    // The note Process A is watching for, so it can say Stud crashed
    // while the crash is happening rather than at the next launch.
    //
    // open/write/close only, and the path was built before the crash.
    if (crash_marker_path_storage()[0] != '\0') {
        const int fd = ::open(crash_marker_path_storage(),
                              O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0600);
        if (fd >= 0) {
            const char* who = crash_process_name();
            ::write(fd, who, ::strlen(who));
            ::write(fd, " ", 1);
            char num[3];
            int at = 0;
            if (sig >= 10) num[at++] = static_cast<char>('0' + (sig / 10) % 10);
            num[at++] = static_cast<char>('0' + sig % 10);
            num[at] = '\n';
            ::write(fd, num, static_cast<size_t>(at) + 1);
            ::close(fd);
        }
    }

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
// The crash marker this session would write, for whoever watches it.
inline std::string crash_marker_path() {
    return std::string(detail::crash_marker_path_storage());
}

// Where a clean shutdown records itself. Process A checks for this when
// the session ends: present means the user asked for it, absent means
// whatever happened was not their doing.
inline std::string clean_exit_marker_path() {
    return std::string(detail::clean_exit_marker_path_storage());
}

// Says this shutdown was wanted. write(2) only, so it is equally usable
// from a signal handler (the render host's SIGTERM path) and from an
// ordinary return out of main().
inline void note_clean_exit() {
    const char* path = detail::clean_exit_marker_path_storage();
    if (path[0] == '\0') return;
    const int fd = ::open(path, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0600);
    if (fd < 0) return;
    ::write(fd, "clean\n", 6);
    ::close(fd);
}

inline void install_crash_reporter(const char* process_name) {
    detail::crash_process_name() = process_name;
    if (detail::crash_marker_path_storage()[0] == '\0') {
        const std::string log = session_log_path_from_env();
        // Bounded, and both must fit whole: a truncated path would name
        // the wrong file, which is worse than naming none.
        if (!log.empty() && log.size() + 6 < 4096) {
            const std::string crash = log + ".crash";
            const std::string clean = log + ".clean";
            std::memcpy(detail::crash_marker_path_storage(), crash.c_str(), crash.size() + 1);
            std::memcpy(detail::clean_exit_marker_path_storage(), clean.c_str(), clean.size() + 1);
        }
    }
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
    // The socket wins when Process A offers one: B and C should be
    // dispatching to it rather than each writing the same file. Falling
    // back to the file keeps a process started on its own still able to
    // log.
    const std::string socket_path = session_log_socket_path_from_env();
    if (!socket_path.empty() && start_session_log_dispatch(socket_path)) return true;
    return start_session_log(session_log_path_from_env());
}

}  // namespace stud::logging
