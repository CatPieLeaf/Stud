#include "stud/transport_ring.h"

#include "aemu/base/ring_buffer.h"

#include <poll.h>
#include <linux/memfd.h>
#include <sys/eventfd.h>
#include <sys/mman.h>
#include <sys/syscall.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <fcntl.h>
#include <cstring>

namespace stud::render_host {

// What both processes map: gfxstream's control block, then a flag per side
// saying it is about to sleep or asleep. The data follows on a page of its
// own.
struct TransportRing::Shared {
    ring_buffer ring;
    uint32_t consumer_sleeping;
    uint32_t producer_sleeping;
};

namespace {

ring_buffer_view* as_view(void* v) { return static_cast<ring_buffer_view*>(v); }

size_t data_offset() {
    const size_t page = static_cast<size_t>(::sysconf(_SC_PAGESIZE));
    return (sizeof(TransportRing::Shared) + page - 1) / page * page;
}

void wake(int fd) {
    const uint64_t one = 1;
    ssize_t n;
    do {
        n = ::write(fd, &one, sizeof(one));
    } while (n < 0 && errno == EINTR);
}

// Sleeps until the other side writes `fd` or `peer` hangs up. False on the
// hang-up.
bool sleep_on(int fd, int peer) {
    pollfd p[2] = {{fd, POLLIN, 0}, {peer, POLLRDHUP, 0}};
    for (;;) {
        const int n = ::poll(p, 2, -1);
        if (n < 0 && errno == EINTR) continue;
        if (n < 0) return false;
        if (p[0].revents & POLLIN) {
            uint64_t count = 0;
            (void)::read(fd, &count, sizeof(count));
            return true;
        }
        if (p[1].revents & (POLLRDHUP | POLLHUP | POLLERR | POLLNVAL)) return false;
    }
}

}  // namespace

TransportRing::~TransportRing() {
    if (shared_ != nullptr) ::munmap(shared_, map_bytes_);
    delete as_view(view_);
    for (int fd : {memory_fd_, wake_consumer_fd_, wake_producer_fd_}) {
        if (fd >= 0) ::close(fd);
    }
}

bool TransportRing::map(size_t data_bytes) {
    map_bytes_ = data_offset() + data_bytes;
    void* p = ::mmap(nullptr, map_bytes_, PROT_READ | PROT_WRITE, MAP_SHARED, memory_fd_, 0);
    if (p == MAP_FAILED) return false;
    shared_ = static_cast<Shared*>(p);
    data_bytes_ = data_bytes;
    view_ = new ring_buffer_view{};
    ring_buffer_init_view_only(as_view(view_), static_cast<uint8_t*>(p) + data_offset(),
                               static_cast<uint32_t>(data_bytes));
    return true;
}

bool TransportRing::create(size_t min_bytes) {
    // A power of two, as the ring's position arithmetic needs.
    size_t bytes = 1;
    while (bytes < min_bytes) bytes <<= 1;
    // Through syscall(): bionic only declares memfd_create() from API 30.
    memory_fd_ = static_cast<int>(::syscall(SYS_memfd_create, "stud-transport-ring", MFD_CLOEXEC));
    wake_consumer_fd_ = ::eventfd(0, EFD_CLOEXEC);
    wake_producer_fd_ = ::eventfd(0, EFD_CLOEXEC);
    if (memory_fd_ < 0 || wake_consumer_fd_ < 0 || wake_producer_fd_ < 0) return false;
    // Reserved now, so running out of memory fails here rather than as a
    // SIGBUS in whichever process first touches a page.
    if (::posix_fallocate(memory_fd_, 0, static_cast<off_t>(data_offset() + bytes)) != 0) {
        return false;
    }
    if (!map(bytes)) return false;
    ring_buffer_init(&shared_->ring);
    shared_->consumer_sleeping = 0;
    shared_->producer_sleeping = 0;
    return true;
}

bool TransportRing::attach(int memory_fd, int wake_consumer_fd, int wake_producer_fd,
                           size_t data_bytes) {
    memory_fd_ = memory_fd;
    wake_consumer_fd_ = wake_consumer_fd;
    wake_producer_fd_ = wake_producer_fd;
    // Anything but the power of two create() makes is not this protocol.
    if (data_bytes == 0 || (data_bytes & (data_bytes - 1)) != 0) return false;
    return map(data_bytes);
}

bool TransportRing::write(const void* data, size_t len, int peer) {
    ring_buffer* r = &shared_->ring;
    ring_buffer_view* v = as_view(view_);
    const auto* p = static_cast<const uint8_t*>(data);
    while (len > 0) {
        const uint32_t read_pos = __atomic_load_n(&r->read_pos, __ATOMIC_SEQ_CST);
        const uint32_t room = ring_buffer_view_get_ring_pos(v, read_pos - r->write_pos - 1);
        if (room == 0) {
            // Full. Said before the last look, so the consumer either sees
            // the flag after it reads or this look sees what it read.
            __atomic_store_n(&shared_->producer_sleeping, 1, __ATOMIC_SEQ_CST);
            const bool still_full = !ring_buffer_view_can_write(r, v, 1);
            const bool ok = !still_full || sleep_on(wake_producer_fd_, peer);
            __atomic_store_n(&shared_->producer_sleeping, 0, __ATOMIC_SEQ_CST);
            if (!ok) return false;
            continue;
        }
        const auto n = static_cast<uint32_t>(std::min<size_t>(room, len));
        ring_buffer_view_write(r, v, p, n, 1);
        p += n;
        len -= n;
        if (__atomic_load_n(&shared_->consumer_sleeping, __ATOMIC_SEQ_CST) != 0) {
            wake(wake_consumer_fd_);
        }
    }
    return true;
}

bool TransportRing::read(void* dst, size_t len, int peer) {
    ring_buffer* r = &shared_->ring;
    ring_buffer_view* v = as_view(view_);
    auto* p = static_cast<uint8_t*>(dst);
    while (len > 0) {
        const uint32_t ready = ring_buffer_available_read(r, v);
        if (ready == 0) {
            // Empty: the mirror of the producer's full case.
            __atomic_store_n(&shared_->consumer_sleeping, 1, __ATOMIC_SEQ_CST);
            const bool still_empty = ring_buffer_available_read(r, v) == 0;
            const bool ok = !still_empty || sleep_on(wake_consumer_fd_, peer);
            __atomic_store_n(&shared_->consumer_sleeping, 0, __ATOMIC_SEQ_CST);
            // A hung-up peer may still have left bytes behind; they are
            // read before the hang-up is believed.
            if (!ok && ring_buffer_available_read(r, v) == 0) return false;
            continue;
        }
        const auto n = static_cast<uint32_t>(std::min<size_t>(ready, len));
        ring_buffer_view_read(r, v, p, n, 1);
        p += n;
        len -= n;
        if (__atomic_load_n(&shared_->producer_sleeping, __ATOMIC_SEQ_CST) != 0) {
            wake(wake_producer_fd_);
        }
    }
    return true;
}

}  // namespace stud::render_host
