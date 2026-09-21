#include "log_collector.h"

#include <fcntl.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstring>
#include <mutex>
#include <string>
#include <thread>

namespace stud::ui {
namespace {

struct Collector {
    std::mutex mutex;
    std::condition_variable wake;
    std::string pending;
    int log_fd = -1;
    int listen_fd = -1;
    std::atomic<bool> running{false};
};

Collector& collector() {
    static Collector c;
    return c;
}

// How long the writer sleeps between batches when nothing is urgent.
constexpr int kFlushIntervalMs = 2000;

// Flush early rather than sit on a backlog this size. A game join
// produces more than this in a second, and holding it costs nothing but
// memory that a single write would free.
constexpr size_t kFlushThreshold = 256u * 1024u;

void write_all(int fd, const char* data, size_t len) {
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

// Takes whatever has accumulated and writes it. The lock is held only to
// swap the buffer out, never across the write, so a process throwing
// lines at the socket is never waiting on this file.
void flush_now() {
    Collector& c = collector();
    std::string batch;
    {
        std::lock_guard<std::mutex> lock(c.mutex);
        batch.swap(c.pending);
    }
    if (!batch.empty()) write_all(c.log_fd, batch.data(), batch.size());
}

// One per connected process. Reads until the process goes away, doing
// nothing but moving bytes into memory -- which is what lets the senders
// treat a write as free.
void reader_thread(int fd) {
    char buffer[64 * 1024];
    for (;;) {
        const ssize_t got = ::read(fd, buffer, sizeof(buffer));
        if (got < 0) {
            if (errno == EINTR) continue;
            break;
        }
        if (got == 0) break;
        Collector& c = collector();
        bool urgent = false;
        {
            std::lock_guard<std::mutex> lock(c.mutex);
            c.pending.append(buffer, static_cast<size_t>(got));
            urgent = c.pending.size() >= kFlushThreshold;
        }
        if (urgent) c.wake.notify_one();
    }
    ::close(fd);
}

void accept_thread() {
    Collector& c = collector();
    for (;;) {
        const int fd = ::accept(c.listen_fd, nullptr, nullptr);
        if (fd < 0) {
            if (errno == EINTR) continue;
            break;
        }
        std::thread(reader_thread, fd).detach();
    }
}

void writer_thread() {
    Collector& c = collector();
    while (c.running.load(std::memory_order_relaxed)) {
        {
            std::unique_lock<std::mutex> lock(c.mutex);
            c.wake.wait_for(lock, std::chrono::milliseconds(kFlushIntervalMs),
                            [&c] { return c.pending.size() >= kFlushThreshold; });
        }
        flush_now();
    }
    flush_now();
}

}  // namespace

bool start_log_collector(const std::string& socket_path, const std::string& log_path) {
    if (socket_path.empty() || log_path.empty()) return false;
    Collector& c = collector();
    if (c.running.load(std::memory_order_relaxed)) return true;

    c.log_fd = ::open(log_path.c_str(), O_WRONLY | O_CREAT | O_APPEND | O_CLOEXEC, 0600);
    if (c.log_fd < 0) return false;

    // A socket left by a session that did not shut down cleanly would
    // make bind() fail; nothing is listening on it by definition, because
    // this process is the only one that ever listens here.
    ::unlink(socket_path.c_str());

    c.listen_fd = ::socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (c.listen_fd < 0) {
        ::close(c.log_fd);
        c.log_fd = -1;
        return false;
    }
    sockaddr_un addr{};
    addr.sun_family = AF_UNIX;
    if (socket_path.size() + 1 > sizeof(addr.sun_path)) {
        ::close(c.listen_fd);
        ::close(c.log_fd);
        c.listen_fd = c.log_fd = -1;
        return false;
    }
    std::memcpy(addr.sun_path, socket_path.c_str(), socket_path.size() + 1);
    if (::bind(c.listen_fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0 ||
        ::listen(c.listen_fd, 8) != 0) {
        ::close(c.listen_fd);
        ::close(c.log_fd);
        c.listen_fd = c.log_fd = -1;
        return false;
    }

    c.running.store(true, std::memory_order_relaxed);
    std::thread(accept_thread).detach();
    std::thread(writer_thread).detach();
    return true;
}

void flush_log_collector() {
    if (!collector().running.load(std::memory_order_relaxed)) return;
    flush_now();
}

}  // namespace stud::ui
