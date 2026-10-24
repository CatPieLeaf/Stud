#pragma once

// The request direction of the engine's Vulkan connection, through shared
// memory instead of the socket.
//
// Every recorded command used to cross by write() on Process B's writer
// thread and read() on render-host's connection thread: two system calls
// per batch and a copy into and out of the kernel, for a stream that runs
// to thousands of commands a frame. Here the writer copies into a ring
// both processes map and the host reads it from there. Replies still come
// back on the socket; they are few and small.
//
// The ring itself is gfxstream's (see cmake/StudGfxstreamRing.cmake): one
// producer, one consumer, positions on separate cache lines. Its own waits
// spin, so the sleeping is done here: a side that finds the ring empty (or
// full) says so in shared memory, checks once more, and sleeps in poll()
// on an eventfd the other side writes only when it has seen that. The same
// poll() watches the socket, so a peer that dies wakes the sleeper rather
// than leaving it asleep for good.

#include <cstddef>
#include <cstdint>

namespace stud::render_host {

class TransportRing {
public:
    TransportRing() = default;
    TransportRing(const TransportRing&) = delete;
    TransportRing& operator=(const TransportRing&) = delete;
    ~TransportRing();

    // The producer's side: the shared memory, holding at least
    // `min_bytes` of data, and the two eventfds.
    bool create(size_t min_bytes);
    // The consumer's side: maps what create() made, from the descriptors
    // it was handed and the size create() chose.
    bool attach(int memory_fd, int wake_consumer_fd, int wake_producer_fd, size_t data_bytes);

    int memory_fd() const { return memory_fd_; }
    int wake_consumer_fd() const { return wake_consumer_fd_; }
    int wake_producer_fd() const { return wake_producer_fd_; }
    size_t data_bytes() const { return data_bytes_; }

    // Blocking, like write_all()/read_all() on the socket. `peer` is that
    // socket; false once it has hung up and nothing more can come.
    bool write(const void* data, size_t len, int peer);
    bool read(void* dst, size_t len, int peer);

    // What both processes map; defined in transport_ring.cpp.
    struct Shared;

private:
    bool map(size_t data_bytes);

    Shared* shared_ = nullptr;
    void* view_ = nullptr;  // gfxstream's ring_buffer_view, kept opaque here
    size_t map_bytes_ = 0;
    size_t data_bytes_ = 0;
    int memory_fd_ = -1;
    int wake_consumer_fd_ = -1;
    int wake_producer_fd_ = -1;
};

}  // namespace stud::render_host
