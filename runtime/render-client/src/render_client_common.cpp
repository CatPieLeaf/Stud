#include "render_client_common.h"

#include <cstdio>
#include <cstdlib>

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
                         "stud: render-client: audio could not open its own connection to %s -- "
                         "sharing the render connection instead\n",
                         path.c_str());
        }
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
                         "stud: render-client: input could not open its own connection to %s -- "
                         "sharing the render connection instead\n",
                         path.c_str());
        }
    }
    return client;
}

}  // namespace stud::render_client
