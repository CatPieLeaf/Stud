// gfxstream's ring transport (address space graphics) for Stud: Mesa's
// AddressSpaceStream, unchanged, on memory render-host maps too. The guest
// writes its commands into that memory and gfxstream's render thread reads
// them there: no socket in the way, no copy, no thread hand-off. The socket
// this opens only hands over the memory and then carries the doorbell, a
// byte sent when the stream finds the render thread asleep.
#include "StudAsgStream.h"

#include <sys/mman.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cstdint>
#include <cstring>

#include "AddressSpaceStream.h"
#include "ProcessPipe.h"
#include "StudStream.h"
#include "gfxstream/common/logging.h"

namespace {

// The device operations AddressSpaceStream uses: the handle is the socket.
address_space_handle_t studOpen() { return -1; }
void studClose(address_space_handle_t fd) {
    if (fd >= 0) ::close(fd);
}
bool studClaimShared(address_space_handle_t, uint64_t, uint64_t) { return true; }
bool studUnclaimShared(address_space_handle_t, uint64_t) { return true; }
void* studMap(address_space_handle_t, uint64_t, uint64_t, uint64_t) { return nullptr; }
void studUnmap(void* ptr, uint64_t size) {
    if (ptr != nullptr) ::munmap(ptr, size);
}
bool studSetSubdeviceType(address_space_handle_t, AddressSpaceSubdeviceType,
                          address_space_handle_t*) {
    return true;
}
bool studPing(address_space_handle_t fd, struct address_space_ping* ping) {
    if (ping->metadata != ASG_NOTIFY_AVAILABLE) return true;
    const char doorbell = 1;
    ssize_t n;
    do {
        n = ::send(fd, &doorbell, 1, MSG_NOSIGNAL);
    } while (n < 0 && errno == EINTR);
    return n == 1;
}

// The memory and its layout from render-host: (ring storage size, write
// buffer size) with the memfd.
bool receiveMemory(int fd, uint64_t layout[2], int* memory) {
    iovec iov{layout, 2 * sizeof(uint64_t)};
    alignas(cmsghdr) char control[CMSG_SPACE(sizeof(int))] = {};
    msghdr msg{};
    msg.msg_iov = &iov;
    msg.msg_iovlen = 1;
    msg.msg_control = control;
    msg.msg_controllen = sizeof(control);
    ssize_t n;
    do {
        n = ::recvmsg(fd, &msg, MSG_WAITALL | MSG_CMSG_CLOEXEC);
    } while (n < 0 && errno == EINTR);
    if (n != static_cast<ssize_t>(2 * sizeof(uint64_t))) return false;
    cmsghdr* cm = CMSG_FIRSTHDR(&msg);
    if (cm == nullptr || cm->cmsg_type != SCM_RIGHTS) return false;
    std::memcpy(memory, CMSG_DATA(cm), sizeof(int));
    return true;
}

}  // namespace

gfxstream::guest::IOStream* createStudAsgStream() {
    // The process id first: the host's render thread for this stream is
    // created as that process's.
    if (!processPipeInit(-1, HOST_CONNECTION_QEMU_PIPE, 0)) return nullptr;
    const uint64_t puid = getPuid();

    const int fd = StudStream::open_tagged('A');
    if (fd < 0) return nullptr;
    uint64_t layout[2] = {};
    int memory = -1;
    if (::send(fd, &puid, sizeof(puid), MSG_NOSIGNAL) != static_cast<ssize_t>(sizeof(puid)) ||
        !receiveMemory(fd, layout, &memory)) {
        ::close(fd);
        return nullptr;
    }
    const size_t ringSize = static_cast<size_t>(layout[0]);
    const size_t bufferSize = static_cast<size_t>(layout[1]);
    char* base = static_cast<char*>(
        ::mmap(nullptr, ringSize + bufferSize, PROT_READ | PROT_WRITE, MAP_SHARED, memory, 0));
    ::close(memory);
    if (base == MAP_FAILED) {
        ::close(fd);
        return nullptr;
    }

    // The view asg_context_create gives, without its initialisation: the
    // host has set the rings and their config up already, and is reading.
    char* ring = base;
    char* buffer = base + ringSize;
    struct asg_context context = {};
    context.to_host =
        reinterpret_cast<struct ring_buffer*>(ring + offsetof(struct asg_ring_storage, to_host));
    context.to_host_large_xfer.ring = reinterpret_cast<struct ring_buffer*>(
        ring + offsetof(struct asg_ring_storage, to_host_large_xfer));
    context.from_host_large_xfer.ring = reinterpret_cast<struct ring_buffer*>(
        ring + offsetof(struct asg_ring_storage, from_host_large_xfer));
    context.buffer = buffer;
    context.host_state = reinterpret_cast<asg_host_state*>(&context.to_host->state);
    context.ring_config = reinterpret_cast<asg_ring_config*>(context.to_host->config);
    ring_buffer_init_view_only(&context.to_host_large_xfer.view, reinterpret_cast<uint8_t*>(buffer),
                               static_cast<uint32_t>(bufferSize));
    ring_buffer_init_view_only(&context.from_host_large_xfer.view,
                               reinterpret_cast<uint8_t*>(buffer),
                               static_cast<uint32_t>(bufferSize));

    struct address_space_ops ops = {
        .open = studOpen,
        .close = studClose,
        .claim_shared = studClaimShared,
        .unclaim_shared = studUnclaimShared,
        .map = studMap,
        .unmap = studUnmap,
        .set_subdevice_type = studSetSubdeviceType,
        .ping = studPing,
    };
    return new AddressSpaceStream(fd, /*version=*/1, context, /*ringOffset=*/0,
                                  /*writeBufferOffset=*/ringSize, ops);
}
