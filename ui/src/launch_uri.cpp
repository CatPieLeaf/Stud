#include "launch_uri.h"

namespace stud::ui {

bool is_roblox_link(std::string_view argument) {
    for (const std::string_view scheme : {"roblox:", "roblox-player:"}) {
        if (argument.size() > scheme.size() && argument.starts_with(scheme)) return true;
    }
    return false;
}

}  // namespace stud::ui
