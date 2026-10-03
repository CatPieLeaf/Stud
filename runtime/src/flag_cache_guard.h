#pragma once

#include <cstddef>
#include <string>

// The engine's flag cache, kept from ever deciding how a session renders.
//
// At startup the engine either fetches its flags from Roblox's CDN, blocking,
// or -- when tombstone.dat in its cache directory says the cached copy is
// still good -- loads flag_cache.dat and skips the fetch. That second path
// is the one that went wrong: the cache failed its signature check
// ("[FlagCache] Security failure."), and the session came up on the
// engine's default flags, with broken shadows and text and every graphics
// feature the server flags turn on missing, until the cache was cleared.
// The cause was Stud's SharedPreferences forgetting the FlagCache's state
// at every exit; see SharedPreferencesJava.
//
// So Stud never lets the cached path run: the tombstone is removed before
// the engine starts, and the engine always fetches. Whatever the flag cache
// reports going wrong is said loudly, so a new way for it to fail does not
// pass as just another quietly broken session.
namespace stud::flag_cache_guard {

// Removes every tombstone.dat in the engine's cache directory. Before the
// engine is given that directory.
void remove_tombstones(const std::string& engine_cache_dir);

// Every line the engine logs.
void note_engine_log_line(const char* text, size_t length);

}  // namespace stud::flag_cache_guard
