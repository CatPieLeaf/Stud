// SocketReader (render-host/src/socket_reader.h): requests served out of
// one buffer must come out byte-exact and in order, whether they were
// buffered together, split across reads, or larger than the buffer.

#include "socket_reader.h"

#include <sys/socket.h>
#include <unistd.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <thread>
#include <vector>

namespace {

int failures = 0;

void check(bool ok, const char* what) {
    if (!ok) {
        std::fprintf(stderr, "FAIL: %s\n", what);
        ++failures;
    }
}

bool write_all(int fd, const void* data, size_t len) {
    const auto* p = static_cast<const char*>(data);
    while (len > 0) {
        const ssize_t n = ::write(fd, p, len);
        if (n <= 0) return false;
        p += n;
        len -= static_cast<size_t>(n);
    }
    return true;
}

}  // namespace

int main() {
    int sv[2];
    if (::socketpair(AF_UNIX, SOCK_STREAM, 0, sv) != 0) {
        std::perror("socketpair");
        return 1;
    }
    // A small receive buffer, so "larger than the buffer" is cheap to make.
    int small = 4096;
    ::setsockopt(sv[1], SOL_SOCKET, SO_RCVBUF, &small, sizeof(small));

    stud::render_host::SocketReader reader(sv[1]);

    // Several small messages in one burst: one fill, served in order.
    const char burst[] = "headerApayloadAheaderBpayloadB";
    check(write_all(sv[0], burst, sizeof(burst) - 1), "write burst");
    char out[16] = {};
    check(reader.read(out, 7) && std::memcmp(out, "headerA", 7) == 0, "first header");
    check(reader.buffered() == sizeof(burst) - 1 - 7, "buffered after a partial consume");
    check(reader.read(out, 8) && std::memcmp(out, "payloadA", 8) == 0, "first payload");
    check(reader.read(out, 7) && std::memcmp(out, "headerB", 7) == 0, "second header");
    check(reader.read(out, 8) && std::memcmp(out, "payloadB", 8) == 0, "second payload");
    check(reader.buffered() == 0, "drained");

    // A payload far larger than the buffer, written from another thread so
    // the socket's own buffer can fill and drain: must arrive whole.
    std::vector<uint8_t> big(1 << 20);
    for (size_t i = 0; i < big.size(); ++i) big[i] = static_cast<uint8_t>(i * 31 + 7);
    std::thread writer([&] { write_all(sv[0], big.data(), big.size()); });
    std::vector<uint8_t> got(big.size());
    check(reader.read(got.data(), static_cast<uint32_t>(got.size())), "read large payload");
    writer.join();
    check(got == big, "large payload byte-exact");

    // The peer going away is a false, not a hang.
    ::close(sv[0]);
    check(!reader.read(out, 1), "read after close fails");
    ::close(sv[1]);

    if (failures == 0) std::printf("socket_reader_test: all checks passed\n");
    return failures == 0 ? 0 : 1;
}
