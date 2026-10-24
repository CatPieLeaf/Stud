// The shared-memory ring the engine's Vulkan requests travel through (see
// stud/transport_ring.h), across two real processes as in use: a stream
// many times the ring's size, in uneven pieces, has to arrive byte for
// byte, through every wrap and every full and empty ring; and a reader
// whose peer has hung up has to return rather than sleep for good.

#include "stud/transport_ring.h"

#include <sys/socket.h>
#include <sys/wait.h>
#include <unistd.h>

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <vector>

namespace {

uint8_t pattern(uint64_t i) { return static_cast<uint8_t>((i * 2654435761u) >> 13); }

}  // namespace

int main() {
    int sockets[2];
    if (::socketpair(AF_UNIX, SOCK_STREAM, 0, sockets) != 0) return 1;
    stud::render_host::TransportRing producer;
    // The smallest ring a page can hold, so the stream wraps constantly.
    if (!producer.create(static_cast<size_t>(::sysconf(_SC_PAGESIZE)))) {
        std::printf("FAIL: could not create the ring\n");
        return 1;
    }
    const uint64_t total = producer.data_bytes() * 64;

    const pid_t child = ::fork();
    if (child == 0) {
        ::close(sockets[0]);
        stud::render_host::TransportRing consumer;
        if (!consumer.attach(::dup(producer.memory_fd()), ::dup(producer.wake_consumer_fd()),
                             ::dup(producer.wake_producer_fd()), producer.data_bytes())) {
            _exit(2);
        }
        std::vector<uint8_t> buf;
        uint64_t got = 0;
        for (size_t piece = 1; got < total; piece = piece * 3 % 997 + 1) {
            buf.resize(static_cast<size_t>(std::min<uint64_t>(piece, total - got)));
            if (!consumer.read(buf.data(), buf.size(), sockets[1])) _exit(3);
            for (uint8_t b : buf) {
                if (b != pattern(got++)) _exit(4);
            }
        }
        // The producer hangs up next: this read must return false.
        uint8_t extra = 0;
        _exit(consumer.read(&extra, 1, sockets[1]) ? 5 : 0);
    }
    ::close(sockets[1]);
    std::vector<uint8_t> buf;
    uint64_t sent = 0;
    for (size_t piece = 1; sent < total; piece = piece * 7 % 1499 + 1) {
        buf.resize(static_cast<size_t>(std::min<uint64_t>(piece, total - sent)));
        for (uint8_t& b : buf) b = pattern(sent++);
        if (!producer.write(buf.data(), buf.size(), sockets[0])) {
            std::printf("FAIL: the producer's write failed\n");
            return 1;
        }
    }
    ::close(sockets[0]);
    int status = 0;
    ::waitpid(child, &status, 0);
    const int code = WIFEXITED(status) ? WEXITSTATUS(status) : -1;
    const char* what[] = {"ok", "?", "attach failed", "read failed early", "bytes differ",
                          "a hung-up peer did not end the read"};
    if (code != 0) {
        std::printf("FAIL: %s\n", code > 0 && code < 6 ? what[code] : "the reader crashed");
        return 1;
    }
    std::printf("%llu bytes through a %zu-byte ring between two processes, intact\n",
                static_cast<unsigned long long>(total), producer.data_bytes());
    return 0;
}
