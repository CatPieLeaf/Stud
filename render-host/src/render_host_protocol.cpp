#include "stud/render_host_protocol.h"

#include <cstdlib>

#include <unistd.h>

namespace stud::render_host {

std::string default_socket_path() {
    if (const char* xdg_runtime_dir = std::getenv("XDG_RUNTIME_DIR")) {
        return std::string(xdg_runtime_dir) + "/stud/render-host.sock";
    }
    return "/tmp/stud-" + std::to_string(::getuid()) + "/render-host.sock";
}

std::string shared_memory_path(uint64_t id) {
    std::string dir;
    if (const char* xdg_runtime_dir = std::getenv("XDG_RUNTIME_DIR")) {
        dir = std::string(xdg_runtime_dir) + "/stud";
    } else {
        dir = "/tmp/stud-" + std::to_string(::getuid());
    }
    return dir + "/mem-" + std::to_string(id);
}

}  // namespace stud::render_host
