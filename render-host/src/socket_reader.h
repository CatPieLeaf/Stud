#pragma once

// Reads a connection's requests out of one buffer instead of one read()
// per header and another per payload.
//
// The engine pipelines its calls, so by the time render-host reads a
// header the socket usually holds many whole requests. Pulled out a header
// and a payload at a time that was 51168 read() calls for 13.4 MB in three
// seconds of play -- 263 bytes each. Here one read() takes whatever the
// socket has, up to its own receive buffer's size, and the requests are
// served from memory.
//
// NOT for a socket that carries file descriptors: a read() into this
// buffer can take bytes that came with SCM_RIGHTS, and read() drops the
// descriptors. See serve_connection_thread() for how the shared-fd channel
// stays out of it.

#include <sys/socket.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <cstdint>
#include <cstring>
#include <vector>

namespace stud::render_host {

class SocketReader {
public:
    // Sized to the socket's own receive buffer, which is the most one read()
    // can return. A socket that will not say leaves this empty, and every
    // read then goes straight to the socket, as before.
    explicit SocketReader(int fd) : fd_(fd) {
        int rcvbuf = 0;
        socklen_t len = sizeof(rcvbuf);
        if (::getsockopt(fd, SOL_SOCKET, SO_RCVBUF, &rcvbuf, &len) == 0 && rcvbuf > 0) {
            buf_.resize(static_cast<size_t>(rcvbuf));
        }
    }

    // Exactly len bytes into dst, or false when the peer has gone.
    bool read(void* dst, uint32_t len) {
        auto* out = static_cast<uint8_t*>(dst);
        while (len > 0) {
            if (pos_ == end_) {
                // More than a buffer's worth still to come: straight into
                // dst, so a large upload is not copied twice.
                if (len >= buf_.size()) return read_direct(out, len);
                if (!fill()) return false;
            }
            const size_t n = std::min<size_t>(len, end_ - pos_);
            std::memcpy(out, buf_.data() + pos_, n);
            pos_ += n;
            out += n;
            len -= static_cast<uint32_t>(n);
        }
        return true;
    }

    // Bytes already read from the socket and not yet handed out. A caller
    // that asks the kernel whether a request is waiting (FIONREAD) has to
    // ask this first: a whole request can be sitting here with the socket
    // itself empty.
    size_t buffered() const { return end_ - pos_; }

private:
    bool fill() {
        for (;;) {
            const ssize_t n = ::read(fd_, buf_.data(), buf_.size());
            if (n < 0 && errno == EINTR) continue;
            if (n <= 0) return false;
            pos_ = 0;
            end_ = static_cast<size_t>(n);
            return true;
        }
    }

    bool read_direct(uint8_t* out, uint32_t len) {
        while (len > 0) {
            const ssize_t n = ::read(fd_, out, len);
            if (n < 0 && errno == EINTR) continue;
            if (n <= 0) return false;
            out += n;
            len -= static_cast<uint32_t>(n);
        }
        return true;
    }

    int fd_;
    std::vector<uint8_t> buf_;
    size_t pos_ = 0;
    size_t end_ = 0;
};

}  // namespace stud::render_host
