#pragma once

#include <fcntl.h>
#include <poll.h>
#include <pthread.h>
#include <unistd.h>

#include <cerrno>
#include <cstdio>
#include <string>

// One log per session: stdout and stderr, which carry the client's output too, are
// teed into it so users can send a log without a terminal or journal. A tee keeps
// the output going where it already went.
namespace stud::logging {

namespace detail {

// The original stdout, saved before the tee replaced fd 1.
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

// A stalled reader (paused terminal, unread pipe) would block every print in the
// process, so it misses that chunk; the log doesn't.
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
        // A logger must never stall the process.
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

// Does nothing for an empty path or a file that can't be opened.
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

    // stdout is a pipe now, block-buffered by default, so a crash could lose the last lines.
    ::setvbuf(stdout, nullptr, _IOLBF, 0);
    ::setvbuf(stderr, nullptr, _IONBF, 0);
    return true;
}

// The original stdout, or -1 without a tee. Give it to children as stdout and stderr.
inline int passthrough_stdout_fd() { return detail::passthrough_fd_storage(); }

}  // namespace stud::logging
