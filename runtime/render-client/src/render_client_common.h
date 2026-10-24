#pragma once

#include "stud/render_host_protocol.h"

// Shared connection singleton for both libEGL.so/libGLESv2.so client
// bodies, lazily connects to stud-render-host (Process C) on first
// real EGL/GLES call Roblox makes, once, for the process's whole life.
// One real glGetError per frame, called by the swap path. See
// glGetError() in gles_client.cpp for why it is not polled per call. Lives
// in the GL client, which is a separate shared library from the EGL one,
// so it is resolved the same way the engine resolves everything else
// here, by name at runtime, rather than linked.
extern "C" void stud_refresh_gl_error_cache();

namespace stud::render_client {


stud::render_host::Client& connection();

// A second, independent connection to the same render host, for audio.
//
// Audio has a hard deadline that GL does not: a burst that arrives late is
// a gap in the waveform, and the engine issues hundreds of GL calls per
// frame through the one connection, every one of which holds its mutex for
// a round trip. Sharing meant the feeder was regularly starved, measured
// at 16312 host-side underruns in a single session, which is what the
// "bad audio quality" actually was. The host already serves more than one
// client connection.
stud::render_host::Client& audio_connection();

// A fourth connection, for reading exported memory back (design C): the
// texture decoder asks for those bytes from the render connection's own
// writer thread, which would wait on itself if it asked down that one.
stud::render_host::Client& readback_connection();

// A third connection, for input, for the same reason audio has one and
// with a sharper deadline still.
//
// Input is the only path a human watches directly: every millisecond
// between the hand moving and the engine hearing about it is a
// millisecond the engine's own painted cursor is visibly behind, and
// there is no hardware cursor here to hide it the way there is on
// Windows. Sharing the render connection meant input could only be
// ASKED for between GL calls, and asking cost a round trip that every
// GL call then queued behind, so it was polled on a timer instead,
// which is latency by construction.
//
// With its own connection the host can simply hold the request until an
// event actually arrives, and hand it over the instant it does.
stud::render_host::Client& input_connection();

// A host-visible allocation backed by a file both processes map.
//
// The engine writes vertices, uniforms and staging data through the
// pointer vkMapMemory hands back. Stud used to give it a private buffer
// and copy whatever changed to the host on every submit, measured at
// 197 MB/s through the socket in a real game, which was most of what the
// render thread was doing. Backing the allocation with memory the host
// imports as device memory (VK_EXT_external_memory_host) makes those
// writes land in the device's memory directly, and the copy stops
// existing. It is a memfd handed to the host over a channel of its own
// (see create_shared_allocation), so these are ordinary shared pages with
// no name and no mount behind them; nothing reaches a disk.
struct SharedAllocation {
    void* address = nullptr;
    size_t length = 0;
    uint64_t id = 0;
    // The memfd, while the caller still needs it; -1 once closed.
    int fd = -1;
    // Mapped from a dma-buf of the GPU's own memory (design C): fast to
    // write, and about 600x slower than RAM for the CPU to read.
    bool vram = false;
};

// Hands `fd` to render-host under `id` over the shared-memory fd channel
// (CallId::SharedMemoryFdChannel), and waits until it has it.
bool send_shared_fd(uint64_t id, int fd);

// The dma-buf render-host exported under `id`, or -1.
int fetch_shared_fd(uint64_t id);

// Moves the render connection's requests into a shared-memory ring, once;
// see stud/transport_ring.h. Left on the socket if anything refuses.
void move_requests_into_ring();

void release_shared_allocation(SharedAllocation& a);

// Backs a shared page range with a memfd and hands it to the host.
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
bool create_shared_allocation(uint64_t size, SharedAllocation& out, bool hand_to_host = true);

}  // namespace stud::render_client
