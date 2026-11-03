#include "stud/render_host_protocol.h"
#include "stud/stud_paths.h"

#include <cstdlib>

#include <unistd.h>

namespace stud::render_host {

std::string default_socket_path() { return stud::paths::runtime_dir() + "/render-host.sock"; }

}  // namespace stud::render_host
