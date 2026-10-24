#include "render_client_common.h"

#include "stud/transport_ring.h"

#include <atomic>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <mutex>
#include <string>

#include <fcntl.h>
#include <linux/memfd.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/syscall.h>
#include <sys/un.h>
#include <unistd.h>

// Which shared library this copy was compiled into.
//
// render_client_common.cpp is listed in five separate SHARED targets, so
// each of libandroid, libaaudio, libEGL, libGLESv2 and libvulkan gets its
// OWN copy of the statics below, up to fifteen sockets, not the three
// this file's header describes. STUD_IPC_OWNERS=1 names them, so a host
// connection can be matched to the library that opened it.
#ifndef STUD_CLIENT_OWNER
#define STUD_CLIENT_OWNER "unknown"
#endif

namespace {
void announce(const char* kind, const stud::render_host::Client& c) {
    static const bool on = std::getenv("STUD_IPC_OWNERS") != nullptr;
    if (!on) return;
    std::fprintf(stderr, "stud: render-client: %s opened the %s connection (fd %d)\n",
                 STUD_CLIENT_OWNER, kind, c.fd());
}
}  // namespace

namespace stud::render_client {

stud::render_host::Client& connection() {
    static stud::render_host::Client client;
    // The connect has to finish before any other thread is handed this
    // client. A plain `tried` flag is set before connect_to() runs, so a
    // second thread arriving in that window gets a Client whose socket is
    // not open yet. A function-local static's initialiser is the one
    // construct the language already serialises for exactly this.
    static const bool tried = [&] {
        std::string path = stud::render_host::default_socket_path();
        if (!client.connect_to(path)) {
            std::fprintf(stderr,
                         "stud: render-client: failed to connect to stud-render-host at %s "
                         "(is it running?)\n",
                         path.c_str());
        } else if (std::getenv("STUD_IPC_NO_WRITER") == nullptr) {
            // One thread owns this socket from here on.
            //
            // This is the connection the engine drives from many threads
            // at once, and the only one that had a second producer
            // (submit and present ran on a queue of their own). That is
            // what made "is my work on the wire yet" a question worth
            // blocking on: measured in a real game, the engine's Main
            // thread spent 13.1% of its wall clock waiting for that other
            // producer to drain. With one writer the question cannot be
            // asked, so it cannot be waited on. See Client::start_writer.
            //
            // Audio and input keep the plain synchronous path below: each
            // is driven by one thread and would gain nothing but a thread.
            client.start_writer();
        }
        announce("render", client);
        return true;
    }();
    (void)tried;
    return client;
}

stud::render_host::Client& audio_connection() {
    static stud::render_host::Client client;
    // The connect has to finish before any other thread is handed this
    // client. A plain `tried` flag is set before connect_to() runs, so a
    // second thread arriving in that window gets a Client whose socket is
    // not open yet. A function-local static's initialiser is the one
    // construct the language already serialises for exactly this.
    static const bool tried = [&] {
        std::string path = stud::render_host::default_socket_path();
        if (!client.connect_to(path)) {
            // Not fatal: the caller falls back to the shared connection,
            // which works but competes with GL for it.
            std::fprintf(stderr,
                         "stud: render-client: audio could not open its own connection to %s, "
                         "sharing the render connection instead\n",
                         path.c_str());
        }
        announce("audio", client);
        return true;
    }();
    (void)tried;
    return client;
}

stud::render_host::Client& readback_connection() {
    static stud::render_host::Client client;
    static const bool tried = [&] {
        std::string path = stud::render_host::default_socket_path();
        if (!client.connect_to(path)) {
            std::fprintf(stderr,
                         "stud: render-client: readback could not open its own connection to %s\n",
                         path.c_str());
        }
        announce("readback", client);
        return true;
    }();
    (void)tried;
    return client;
}

stud::render_host::Client& input_connection() {
    static stud::render_host::Client client;
    // The connect has to finish before any other thread is handed this
    // client. A plain `tried` flag is set before connect_to() runs, so a
    // second thread arriving in that window gets a Client whose socket is
    // not open yet. A function-local static's initialiser is the one
    // construct the language already serialises for exactly this.
    static const bool tried = [&] {
        std::string path = stud::render_host::default_socket_path();
        if (!client.connect_to(path)) {
            std::fprintf(stderr,
                         "stud: render-client: input could not open its own connection to %s, "
                         "sharing the render connection instead\n",
                         path.c_str());
        }
        announce("input", client);
        return true;
    }();
    (void)tried;
    return client;
}


namespace {

using stud::render_host::CallId;

// The shared-memory fd channel: a connection of the render host's own
// that carries nothing but memfds (see CallId::SharedMemoryFdChannel).
//
// A memfd has no name the host could open, so the descriptor itself is
// handed over, attached to the allocation id as SCM_RIGHTS. It needs a
// connection to itself: ancillary data rides one particular byte of the
// stream, and the render connection is written by a writer thread and
// several engine threads at once.
//
// Opened after the render connection, deliberately. The host serves its
// FIRST connection on the main loop, which knows nothing of this channel;
// every later one gets a thread of its own, and that is where the
// channel is recognised.
class SharedFdChannel {
public:
    // Hands `fd` to the host under `id`, and waits until the host has it,
    // so the call that names the id next cannot arrive first.
    bool send(uint64_t id, int fd) {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!ensure_open_locked()) return false;
        char control[CMSG_SPACE(sizeof(int))] = {};
        iovec iov{&id, sizeof(id)};
        msghdr msg{};
        msg.msg_iov = &iov;
        msg.msg_iovlen = 1;
        msg.msg_control = control;
        msg.msg_controllen = sizeof(control);
        cmsghdr* c = CMSG_FIRSTHDR(&msg);
        c->cmsg_level = SOL_SOCKET;
        c->cmsg_type = SCM_RIGHTS;
        c->cmsg_len = CMSG_LEN(sizeof(int));
        std::memcpy(CMSG_DATA(c), &fd, sizeof(fd));
        ssize_t n;
        do {
            n = ::sendmsg(fd_, &msg, MSG_NOSIGNAL);
        } while (n < 0 && errno == EINTR);
        if (n != static_cast<ssize_t>(sizeof(id))) return fail_locked();
        uint8_t kept = 0;
        do {
            n = ::recv(fd_, &kept, sizeof(kept), 0);
        } while (n < 0 && errno == EINTR);
        if (n != 1) return fail_locked();
        return kept == 1;
    }

    // The dma-buf the host exported for allocation `id` (design C,
    // STUD_VK_BAR_EXPORT): the id goes over with no fd attached, and the
    // answer byte comes back carrying one. -1 if the host has none.
    int fetch(uint64_t id) {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!ensure_open_locked()) return -1;
        ssize_t n;
        do {
            n = ::send(fd_, &id, sizeof(id), MSG_NOSIGNAL);
        } while (n < 0 && errno == EINTR);
        if (n != static_cast<ssize_t>(sizeof(id))) {
            fail_locked();
            return -1;
        }
        uint8_t found = 0;
        char control[CMSG_SPACE(sizeof(int))] = {};
        iovec iov{&found, sizeof(found)};
        msghdr msg{};
        msg.msg_iov = &iov;
        msg.msg_iovlen = 1;
        msg.msg_control = control;
        msg.msg_controllen = sizeof(control);
        do {
            n = ::recvmsg(fd_, &msg, MSG_CMSG_CLOEXEC);
        } while (n < 0 && errno == EINTR);
        if (n != 1) {
            fail_locked();
            return -1;
        }
        int received = -1;
        for (cmsghdr* c = CMSG_FIRSTHDR(&msg); c != nullptr; c = CMSG_NXTHDR(&msg, c)) {
            if (c->cmsg_level == SOL_SOCKET && c->cmsg_type == SCM_RIGHTS &&
                c->cmsg_len >= CMSG_LEN(sizeof(int))) {
                std::memcpy(&received, CMSG_DATA(c), sizeof(received));
            }
        }
        if (found != 1 && received >= 0) {
            ::close(received);
            received = -1;
        }
        return received;
    }

private:
    bool ensure_open_locked() {
        if (fd_ >= 0) return true;
        if (broken_) return false;
        // The render connection first, so this one is never the host's
        // first connection.
        (void)stud::render_client::connection();
        const std::string path = stud::render_host::default_socket_path();
        const int s = ::socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
        if (s < 0) return fail_locked();
        sockaddr_un addr{};
        addr.sun_family = AF_UNIX;
        std::strncpy(addr.sun_path, path.c_str(), sizeof(addr.sun_path) - 1);
        if (::connect(s, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) {
            ::close(s);
            return fail_locked();
        }
        stud::render_host::Header hdr{};
        hdr.call_id = CallId::SharedMemoryFdChannel;
        const char* p = reinterpret_cast<const char*>(&hdr);
        size_t left = sizeof(hdr);
        while (left > 0) {
            const ssize_t n = ::send(s, p, left, MSG_NOSIGNAL);
            if (n < 0 && errno == EINTR) continue;
            if (n <= 0) {
                ::close(s);
                return fail_locked();
            }
            p += n;
            left -= static_cast<size_t>(n);
        }
        fd_ = s;
        return true;
    }

    // A broken channel stays broken: every allocation from then on takes
    // the copying path, which is slower and correct.
    bool fail_locked() {
        if (fd_ >= 0) ::close(fd_);
        fd_ = -1;
        if (!broken_) {
            broken_ = true;
            std::fprintf(stderr,
                         "stud: render-client (%s): the shared-memory channel to the render "
                         "host failed (%s); shared pages fall back to copying\n",
                         STUD_CLIENT_OWNER, std::strerror(errno));
            std::fflush(stderr);
        }
        return false;
    }

    std::mutex mutex_;
    int fd_ = -1;
    bool broken_ = false;
};

SharedFdChannel& shared_fd_channel() {
    static SharedFdChannel c;
    return c;
}

uint64_t random_id_start() {
    uint64_t v = 0;
    if (::syscall(SYS_getrandom, &v, sizeof(v), 0) != static_cast<long>(sizeof(v))) {
        v = static_cast<uint64_t>(::getpid()) << 32;
    }
    return v != 0 ? v : 1;
}

// An id for an fd handed over the shared-memory fd channel. From a random
// start: each library linking this file has its own counter, and
// render-host keeps every fd it is handed by id.
uint64_t next_shared_id() {
    static std::atomic<uint64_t> next_id{random_id_start()};
    return next_id.fetch_add(1, std::memory_order_relaxed);
}

}  // namespace

bool send_shared_fd(uint64_t id, int fd) { return shared_fd_channel().send(id, fd); }

int fetch_shared_fd(uint64_t id) { return shared_fd_channel().fetch(id); }

void move_requests_into_ring() {
    static std::once_flag once;
    std::call_once(once, [] {
        stud::render_host::Client& client = connection();
        // As much in flight as the socket itself held.
        int sndbuf = 0;
        socklen_t len = sizeof(sndbuf);
        if (!client.connected() ||
            ::getsockopt(client.fd(), SOL_SOCKET, SO_SNDBUF, &sndbuf, &len) != 0 || sndbuf <= 0) {
            return;
        }
        auto ring = std::make_shared<stud::render_host::TransportRing>();
        const uint64_t memory_id = next_shared_id();
        const uint64_t consumer_id = next_shared_id();
        const uint64_t producer_id = next_shared_id();
        const bool ready = ring->create(static_cast<size_t>(sndbuf)) &&
                           send_shared_fd(memory_id, ring->memory_fd()) &&
                           send_shared_fd(consumer_id, ring->wake_consumer_fd()) &&
                           send_shared_fd(producer_id, ring->wake_producer_fd());
        const size_t bytes = ring->data_bytes();
        const uint64_t a[8] = {memory_id, consumer_id, producer_id, bytes};
        stud::render_host::TransportRing* r = ring.get();
        const bool moved =
            ready && client.switch_to_ring(
                         ring, [r](const void* data, uint32_t n, int peer) {
                             return r->write(data, n, peer);
                         },
                         a);
        std::printf("stud: render-client (%s): requests travel %s\n", STUD_CLIENT_OWNER,
                    moved ? "through a shared-memory ring" : "over the socket");
        std::fflush(stdout);
    });
}

void release_shared_allocation(SharedAllocation& a) {
    if (a.address != nullptr) ::munmap(a.address, a.length);
    if (a.fd >= 0) ::close(a.fd);
    a = SharedAllocation{};
}

// Backs a host-visible allocation with a memfd and hands it to the host.
//
// A memfd, not a file in the runtime directory. These used to be files
// in $XDG_RUNTIME_DIR, a tmpfs usually sized at a tenth of RAM that also
// holds the Wayland socket, D-Bus and PipeWire's; a heavy game's
// host-visible memory alone was measured at 1.3 GB of its 3.1 GB, and
// when it filled the whole desktop went with it. A memfd is the same
// kind of page (shmem, same huge-page policy), mapped and imported
// exactly the same way, but it is charged to no mount's size limit, has
// no name that can be left behind, and dies with its last reference.
//
// `out.fd` stays open for the caller, which may need it once more (the
// write barrier maps the same pages); release_shared_allocation() or the
// caller closes it. `hand_to_host` false leaves the handover to the
// caller, for one that must set something else up first and would
// otherwise leave the host holding an fd nothing ever claims.
bool create_shared_allocation(uint64_t size, SharedAllocation& out, bool hand_to_host) {
    const uint64_t id = next_shared_id();
    // Through syscall(): bionic only declares memfd_create() from API 30.
    const int fd = static_cast<int>(::syscall(SYS_memfd_create, "stud-vk-memory", MFD_CLOEXEC));
    if (fd < 0) return false;
    // Whole pages: the import on the other side needs a page-aligned
    // mapping of a whole number of pages.
    const size_t page = static_cast<size_t>(::sysconf(_SC_PAGESIZE));
    const size_t length = (static_cast<size_t>(size) + page - 1) & ~(page - 1);
    // Reserve the pages now, rather than letting them be filled in on
    // first touch: running out of memory is then an ordinary error here,
    // where the caller can fall back to copying, instead of a SIGBUS on
    // whichever process touches the page first. posix_fallocate returns
    // the error and does not set errno.
    const int reserved = ::posix_fallocate(fd, 0, static_cast<off_t>(length));
    if (reserved != 0) {
        static bool said = false;
        if (!said) {
            said = true;
            std::printf("stud: could not reserve a %zu KB shared mapping (%s); the engine's "
                        "writes will be copied instead of shared\n",
                        length / 1024, std::strerror(reserved));
            std::fflush(stdout);
        }
        ::close(fd);
        return false;
    }
    void* p = ::mmap(nullptr, length, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (p == MAP_FAILED) {
        ::close(fd);
        return false;
    }
    if (hand_to_host && !send_shared_fd(id, fd)) {
        ::munmap(p, length);
        ::close(fd);
        return false;
    }
    out.address = p;
    out.length = length;
    out.id = id;
    out.fd = fd;
    return true;
}

}  // namespace stud::render_client
