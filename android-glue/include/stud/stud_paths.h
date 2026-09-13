#pragma once

#include <cstdlib>
#include <string>

// Where Stud keeps its own files, by what the files ARE rather than by
// what is convenient. All three processes include this, so there is one
// answer rather than a copy per process.
//
//   config  ~/.config/stud       what the user chose: config.json, flags.json
//   data    ~/.local/share/stud  what would hurt to lose: the safe-storage
//                                secrets (the session cookie among them),
//                                the device identity, the engine's own
//                                user settings and local storage
//   cache   ~/.cache/stud        what Stud can rebuild: the extracted APK,
//                                its assets, the engine's asset store and
//                                over-the-air patches
//   state   ~/.local/state/stud  what only matters after the fact: Stud's
//                                own session logs
//
// The split is not cosmetic. Everything under the cache directory is
// something a cache cleaner (or `rm -rf ~/.cache`) is entitled to delete
// at any moment, and Stud used to keep the device identity and the
// engine's settings there -- so a cache clean silently changed the
// machine's identity and reset the user's own graphics settings.
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
// specifically, and it is where a log belongs -- losing it costs
// nothing, but a cache cleaner has no business taking it mid-session.
inline std::string state_dir() { return detail::xdg_dir("XDG_STATE_HOME", "/.local/state/stud"); }
inline std::string log_dir() { return state_dir() + "/logs"; }

// The engine's own two directories, in Android's own terms: getFilesDir()
// is what an app is expected to keep, getCacheDir() is what it is
// expected to lose. Stud honours that distinction rather than putting
// both under one root.
// Stud's own copy of the Roblox APK.
//
// The settings window is a picker, not a path field: whatever is chosen
// is copied here and used from here forever after, so moving, renaming
// or deleting the file that was picked cannot break a launch. Exactly
// one is kept, under one fixed name, and importing another overwrites
// it -- the name the user picked is a label in the window, not a thing
// on disk. The extractor works out a merged .apk from a split bundle by
// content, so the extension carries no meaning here either.
inline std::string apk_dir() { return data_dir() + "/apk"; }
inline std::string stored_apk_path() { return apk_dir() + "/roblox.apk"; }

inline std::string engine_files_dir() { return data_dir() + "/files"; }
inline std::string engine_cache_dir() { return cache_dir() + "/cache"; }

// The parts of the engine's files directory that are really a cache: its
// asset store, the over-the-air app-shell patches and its logs. They sit
// inside filesDir because that is where the engine puts them, and they
// are linked out to the cache directory so a cache clean reclaims the
// space (see runtime/src/main.cpp, link_engine_caches()).
inline std::string engine_cache_overflow_dir() { return cache_dir() + "/engine"; }

}  // namespace stud::paths
