#pragma once

#include <sys/stat.h>

#include <cstdlib>
#include <string>

// Where Stud keeps its own files, by what the files ARE rather than by
// what is convenient.
//
//   config  ~/.config/stud       what the user chose: config.json, flags.json
//   data    ~/.local/share/stud  what would hurt to lose: the Roblox client
//                                and the Rosemary prefix, which holds
//                                Roblox's own settings and login
//   cache   ~/.cache/stud        what Stud can rebuild
//   state   ~/.local/state/stud  what only matters after the fact: Stud's
//                                own session logs
//
// Everything under the cache directory is something a cache cleaner is
// entitled to delete at any moment, so nothing that would reset the user's
// settings or login lives there.
namespace stud::paths {

inline std::string home_dir() {
    const char* home = std::getenv("HOME");
    return (home != nullptr && *home != '\0') ? home : "/tmp";
}

namespace detail {
inline std::string xdg_dir(const char* env_var, const char* home_relative) {
    if (const char* value = std::getenv(env_var); value != nullptr && *value != '\0') {
        return std::string(value) + "/stud";
    }
    return home_dir() + home_relative;
}
}  // namespace detail

inline std::string config_dir() { return detail::xdg_dir("XDG_CONFIG_HOME", "/.config/stud"); }
inline std::string data_dir() { return detail::xdg_dir("XDG_DATA_HOME", "/.local/share/stud"); }
inline std::string cache_dir() { return detail::xdg_dir("XDG_CACHE_HOME", "/.cache/stud"); }

// Logs are neither config, data nor cache: XDG names this one
// specifically, and it is where a log belongs, losing it costs
// nothing, but a cache cleaner has no business taking it mid-session.
inline std::string state_dir() { return detail::xdg_dir("XDG_STATE_HOME", "/.local/state/stud"); }
inline std::string log_dir() { return state_dir() + "/logs"; }

// Sockets and other files that only live as long as a session.
//
// XDG_RUNTIME_DIR is private to the user by definition. Without one (su,
// sudo -u, a minimal session) this used to fall back to /tmp/stud-<uid>,
// a name any other local user could create first and own. The fallback is now under the user's own home, made private
// here, so it never depends on who got to a shared directory first.
inline std::string runtime_dir() {
    if (const char* xdg = std::getenv("XDG_RUNTIME_DIR"); xdg != nullptr && *xdg != '\0') {
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
