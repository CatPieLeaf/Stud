#include "StudStream.h"

#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "gfxstream/common/logging.h"

namespace {

bool send_all(int fd, const void* data, size_t len) {
    const auto* p = static_cast<const char*>(data);
    while (len > 0) {
        const ssize_t n = ::send(fd, p, len, MSG_NOSIGNAL);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) return false;
        p += n;
        len -= static_cast<size_t>(n);
    }
    return true;
}

bool recv_all(int fd, void* data, size_t len) {
    auto* p = static_cast<char*>(data);
    while (len > 0) {
        const ssize_t n = ::recv(fd, p, len, 0);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) return false;
        p += n;
        len -= static_cast<size_t>(n);
    }
    return true;
}

}  // namespace

StudStream::StudStream(size_t bufsize) : IOStream(bufsize), m_bufsize(bufsize) {}

StudStream::~StudStream() {
    if (m_fd >= 0) ::close(m_fd);
    free(m_buf);
}

int StudStream::open_tagged(char tag) {
    // stderr as well as the log: outside Android nothing reads the log.
    const char* path = std::getenv("STUD_GFXSTREAM_SOCKET");
    if (path == nullptr || *path == '\0') {
        std::fprintf(stderr, "StudStream: STUD_GFXSTREAM_SOCKET is not set\n");
        GFXSTREAM_ERROR("StudStream: STUD_GFXSTREAM_SOCKET is not set");
        return -1;
    }
    const int fd = ::socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (fd < 0) return -1;
    sockaddr_un addr{};
    addr.sun_family = AF_UNIX;
    std::strncpy(addr.sun_path, path, sizeof(addr.sun_path) - 1);
    if (::connect(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0 ||
        !send_all(fd, &tag, 1)) {
        std::fprintf(stderr, "StudStream: cannot reach %s: %s\n", path, std::strerror(errno));
        GFXSTREAM_ERROR("StudStream: cannot reach %s: %s", path, std::strerror(errno));
        ::close(fd);
        return -1;
    }
    return fd;
}

int StudStream::connect(const char*) {
    m_fd = open_tagged('R');
    return m_fd < 0 ? -1 : 0;
}

uint64_t StudStream::processPipeInit() {
    const int fd = open_tagged('P');
    if (fd < 0) return 0;
    uint64_t puid = 0;
    if (!recv_all(fd, &puid, sizeof(puid))) puid = 0;
    // The host keeps the id for as long as this connection is open: it is
    // how it learns the process has gone. Deliberately left open.
    return puid;
}

void* StudStream::allocBuffer(size_t minSize) {
    const size_t size = m_bufsize < minSize ? minSize : m_bufsize;
    if (m_buf == nullptr || size > m_bufsize) {
        auto* p = static_cast<unsigned char*>(realloc(m_buf, size));
        if (p == nullptr) return nullptr;
        m_buf = p;
        m_bufsize = size;
    }
    return m_buf;
}

int StudStream::commitBuffer(size_t size) {
    if (size == 0) return 0;
    return writeFully(m_buf, size);
}

int StudStream::writeFully(const void* buf, size_t len) {
    return m_fd >= 0 && send_all(m_fd, buf, len) ? 0 : -1;
}

const unsigned char* StudStream::readFully(void* buf, size_t len) {
    if (len == 0) return static_cast<const unsigned char*>(buf);
    return m_fd >= 0 && recv_all(m_fd, buf, len) ? static_cast<const unsigned char*>(buf)
                                                 : nullptr;
}

const unsigned char* StudStream::commitBufferAndReadFully(size_t size, void* buf, size_t len) {
    if (commitBuffer(size) != 0) return nullptr;
    return readFully(buf, len);
}

const unsigned char* StudStream::read(void* buf, size_t* inout_len) {
    if (m_fd < 0 || buf == nullptr || inout_len == nullptr) return nullptr;
    ssize_t n;
    do {
        n = ::recv(m_fd, buf, *inout_len, 0);
    } while (n < 0 && errno == EINTR);
    if (n <= 0) return nullptr;
    *inout_len = static_cast<size_t>(n);
    return static_cast<const unsigned char*>(buf);
}
