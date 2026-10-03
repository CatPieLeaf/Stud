#include "flag_cache_guard.h"

#include <cstdio>
#include <filesystem>
#include <string_view>
#include <system_error>

namespace stud::flag_cache_guard {

void remove_tombstones(const std::string& engine_cache_dir) {
    std::error_code ec;
    // Where in the cache directory the engine keeps it is the engine's
    // business, so every tombstone.dat in it, at any depth.
    for (std::filesystem::recursive_directory_iterator it(
             engine_cache_dir, std::filesystem::directory_options::skip_permission_denied, ec),
         end;
         !ec && it != end; it.increment(ec)) {
        if (it->path().filename() != "tombstone.dat" || !it->is_regular_file(ec)) continue;
        std::error_code removed;
        std::filesystem::remove(it->path(), removed);
        std::printf("stud: FLAG CACHE: %s the engine's tombstone %s, left by the last session, "
                    "so the engine fetches its flags instead of loading the cached copy\n",
                    removed ? "COULD NOT REMOVE" : "removed", it->path().c_str());
        std::fflush(stdout);
    }
}

void note_engine_log_line(const char* text, size_t length) {
    const std::string_view line(text, length);
    const bool flag_cache = line.find("[DFLog::FlagCache]") != std::string_view::npos ||
                            line.find("[FLog::TombstoneCache]") != std::string_view::npos;
    if (!flag_cache) return;

    if (line.find("Tombstone") != std::string_view::npos &&
        line.find("written to file") != std::string_view::npos) {
        std::printf("stud: !!! FLAG CACHE: the engine wrote its tombstone; Stud removes it at the "
                    "next start. The engine's line: %.*s\n",
                    static_cast<int>(length), text);
        std::fflush(stdout);
        return;
    }
    const bool error = line.find(",Error [") != std::string_view::npos;
    const bool warning = line.find(",Warning [") != std::string_view::npos;
    if (!error && !warning) return;
    // What removing the tombstone makes the engine say at every start,
    // and nothing else: an engine that found no tombstone, as intended.
    if (line.find("Failed to open tombstone file for reading") != std::string_view::npos ||
        line.find("Tombstone default value precheck failed") != std::string_view::npos) {
        return;
    }
    std::printf("stud: !!! FLAG CACHE %s: something in the engine's flag cache went wrong, and "
                "a session on its default flags renders badly. The engine's line: %.*s\n",
                error ? "ERROR" : "WARNING", static_cast<int>(length), text);
    std::fflush(stdout);
}

}  // namespace stud::flag_cache_guard
