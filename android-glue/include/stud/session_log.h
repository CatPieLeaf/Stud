#pragma once

#include <fcntl.h>
#include <pthread.h>
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
// (by design -- see its own "hand off and get out of the way" note), so
// a pipe it owned would be dead for the rest of the session. O_APPEND
// makes concurrent writers safe -- the kernel places each write at the
// current end of file, so lines from three processes interleave but
// never overwrite each other.
namespace stud::logging {

namespace detail {

// The real stdout, saved before the tee replaced fd 1. Children must be
// given THIS rather than inheriting the pipe: the pipe's only reader is
// this process's tee thread, and Process A exits within a second of
// spawning the other two -- after which their writes would hit a pipe
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
        // one is not worth losing the other over -- this is a logger,
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

// Tees this process's stdout and stderr into `path`. Safe to call with an
// empty path (does nothing) and safe to call when the file cannot be
// opened -- output simply keeps going where it already went.
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
    // lines still sitting in libc's buffer -- stdout is a pipe now, and
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
// for the first one -- a mistake this project has already made once, in
// its own backtrace walker.
inline void crash_handler(int sig) {
    const char* name = crash_process_name();
    char digits[3] = {static_cast<char>('0' + (sig / 10) % 10),
                      static_cast<char>('0' + sig % 10), '\n'};
    const char* prefix = "stud: CRASH in ";
    const char* middle = ", signal ";
    ::write(STDERR_FILENO, prefix, ::strlen(prefix));
    ::write(STDERR_FILENO, name, ::strlen(name));
    ::write(STDERR_FILENO, middle, ::strlen(middle));
    ::write(STDERR_FILENO, digits, sizeof(digits));
    // The real disposition, then re-raise, so the exit status and any
    // core dump are exactly what the signal would have produced.
    ::signal(sig, SIG_DFL);
    ::raise(sig);
}

}  // namespace detail

// Names this process in the session log if it dies of a fatal signal.
// The engine's own process has a far more capable trap handler
// (trap_recovery.h, which recovers rather than reports); this is for the
// two glibc processes, where a crash otherwise left nothing in the log
// at all and the window simply vanished.
inline void install_crash_reporter(const char* process_name) {
    detail::crash_process_name() = process_name;
    for (int sig : {SIGSEGV, SIGBUS, SIGILL, SIGFPE, SIGABRT}) {
        ::signal(sig, detail::crash_handler);
    }
}

inline bool start_session_log_from_env() {
    return start_session_log(session_log_path_from_env());
}

}  // namespace stud::logging
