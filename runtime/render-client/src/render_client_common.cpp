#include "render_client_common.h"

#include <cstdio>
#include <cstdlib>

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
    static bool tried = false;
    if (!tried) {
        tried = true;
        std::string path = stud::render_host::default_socket_path();
        if (!client.connect_to(path)) {
            std::fprintf(stderr,
                         "stud: render-client: failed to connect to stud-render-host at %s "
                         "(is it running?)\n",
                         path.c_str());
        }
        announce("render", client);
    }
    return client;
}

stud::render_host::Client& audio_connection() {
    static stud::render_host::Client client;
    static bool tried = false;
    if (!tried) {
        tried = true;
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
    }
    return client;
}

stud::render_host::Client& input_connection() {
    static stud::render_host::Client client;
    static bool tried = false;
    if (!tried) {
        tried = true;
        std::string path = stud::render_host::default_socket_path();
        if (!client.connect_to(path)) {
            std::fprintf(stderr,
                         "stud: render-client: input could not open its own connection to %s, "
                         "sharing the render connection instead\n",
                         path.c_str());
        }
        announce("input", client);
    }
    return client;
}

}  // namespace stud::render_client
