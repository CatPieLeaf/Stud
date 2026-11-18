#pragma once

#include <sys/stat.h>

#include <cstdlib>
#include <string>

//   config  ~/.config/stud       user choices: config.json, flags.json
//   data    ~/.local/share/stud  the Roblox client and the Rosemary prefix
//                                (Roblox's settings and login)
//   cache   ~/.cache/stud        rebuildable; cleaners may delete it any time
//   state   ~/.local/state/stud  session logs
namespace stud::paths {

inline std::string home_dir() {
    const char* home = std::getenv("HOME");
    return (home != nullptr && *home != '\0') ? home : "/tmp";
}

namespace detail {
// A relative value counts as unset, per the XDG spec.
inline std::string xdg_dir(const char* env_var, const char* home_relative) {
    if (const char* value = std::getenv(env_var); value != nullptr && value[0] == '/') {
        return std::string(value) + "/stud";
    }
    return home_dir() + home_relative;
}
}  // namespace detail

inline std::string config_dir() { return detail::xdg_dir("XDG_CONFIG_HOME", "/.config/stud"); }
inline std::string data_dir() { return detail::xdg_dir("XDG_DATA_HOME", "/.local/share/stud"); }
inline std::string cache_dir() { return detail::xdg_dir("XDG_CACHE_HOME", "/.cache/stud"); }

inline std::string state_dir() { return detail::xdg_dir("XDG_STATE_HOME", "/.local/state/stud"); }
inline std::string log_dir() { return state_dir() + "/logs"; }

// Session-lifetime files. Without XDG_RUNTIME_DIR, a private directory in the
// user's home: a shared /tmp name could be created first by another user.
inline std::string runtime_dir() {
    if (const char* xdg = std::getenv("XDG_RUNTIME_DIR"); xdg != nullptr && xdg[0] == '/') {
        return std::string(xdg) + "/stud";
    }
    const std::string dir = state_dir() + "/run";
    for (size_t at = dir.find('/', 1); at != std::string::npos; at = dir.find('/', at + 1)) {
        ::mkdir(dir.substr(0, at).c_str(), 0700);
    }
    ::mkdir(dir.c_str(), 0700);
    ::chmod(dir.c_str(), 0700);
    return dir;
}

}  // namespace stud::paths
