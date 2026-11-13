#pragma once

#include <fcntl.h>
#include <poll.h>
#include <pthread.h>
#include <unistd.h>

#include <cerrno>
#include <cstdio>
#include <string>

// One log file per Stud session.
//
// A terminal, if the user happened to start Stud from one, and the systemd
// journal, if the launch came from the desktop entry, are not things a user
// can be asked to produce when something goes wrong. So the launcher tees its
// stdout and stderr, which carry the client's output too, into the session
// log, and keeps writing to wherever they already went.
//
// A tee and not a plain redirect: redirecting would take the output away from
// whoever was already reading it.
namespace stud::logging {

namespace detail {

// The real stdout, saved before the tee replaced fd 1.
inline int& passthrough_fd_storage() {
    static int fd = -1;
    return fd;
}

struct TeeState {
    int read_fd = -1;
    int log_fd = -1;
    int passthrough_fd = -1;
};

inline void write_fully(int fd, const char* data, size_t size) {
    size_t written = 0;
    while (written < size) {
        const ssize_t n = ::write(fd, data + written, size - written);
        if (n <= 0) {
            if (n < 0 && errno == EINTR) continue;
            return;
        }
        written += static_cast<size_t>(n);
    }
}

// Whether a chunk can go to the real stdout right now. Whoever reads it may
// have stopped (a terminal paused with Ctrl+S, a pipe nobody reads), and
// waiting for them would fill the pipe behind this and block every print in
// the process. They miss that chunk; the log does not.
inline bool passthrough_ready(int fd) {
    pollfd p{fd, POLLOUT, 0};
    return ::poll(&p, 1, 0) > 0 && (p.revents & POLLOUT) != 0;
}

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
        // Both destinations get every byte they can take; this is a logger,
        // and it must never be the reason a process stalls.
        if (state->passthrough_fd >= 0 && passthrough_ready(state->passthrough_fd)) {
            write_fully(state->passthrough_fd, buffer, static_cast<size_t>(got));
        }
        write_fully(state->log_fd, buffer, static_cast<size_t>(got));
    }
    ::close(state->read_fd);
    delete state;
    return nullptr;
}

}  // namespace detail

// Tees this process's stdout and stderr into `path`. Safe to call with an
// empty path (does nothing) and when the file cannot be opened: output simply
// keeps going where it already went.
inline bool start_session_log(const std::string& path) {
    if (path.empty()) return false;
    const int log_fd =
        ::open(path.c_str(), O_WRONLY | O_CREAT | O_APPEND | O_CLOEXEC | O_NOFOLLOW, 0600);
    if (log_fd < 0) return false;

    int pipe_fds[2] = {-1, -1};
    if (::pipe2(pipe_fds, O_CLOEXEC) != 0) {
        ::close(log_fd);
        return false;
    }

    // Keep the real stdout so the tee can still write to it after fd 1 has
    // been replaced by the pipe.
    const int passthrough = ::fcntl(STDOUT_FILENO, F_DUPFD_CLOEXEC, 0);
    detail::passthrough_fd_storage() = passthrough;

    auto* state = new detail::TeeState{pipe_fds[0], log_fd, passthrough};
    pthread_t thread{};
    if (::pthread_create(&thread, nullptr, detail::tee_thread, state) != 0) {
        delete state;
        detail::passthrough_fd_storage() = -1;
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

    // Line buffering, so a crash cannot swallow the last lines still sitting
    // in libc's buffer: stdout is a pipe now, and a pipe is block-buffered by
    // default.
    ::setvbuf(stdout, nullptr, _IOLBF, 0);
    ::setvbuf(stderr, nullptr, _IONBF, 0);
    return true;
}

// The real stdout this process had before start_session_log() replaced it,
// or -1 if no tee is running. Hand this to a child as its stdout and stderr
// so it writes where this process's output really goes.
inline int passthrough_stdout_fd() { return detail::passthrough_fd_storage(); }

}  // namespace stud::logging
