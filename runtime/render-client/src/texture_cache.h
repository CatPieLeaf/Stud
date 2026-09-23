#pragma once

#include <cstdint>

// Decoded texture levels, kept between runs.
//
// Decoding an ETC2/EAC level is deterministic, the same source bytes
// always produce the same output, and Roblox's content is immutable, so
// the result can be kept and a hit turns a decode into a file read, which
// costs well under half the CPU; see the floor in texture_decode.cpp for
// the measurements.
//
// It lives in the cache directory, where a cache cleaner is entitled to
// delete it: losing it costs the time to decode again and nothing else.
// STUD_TEX_NO_CACHE=1 turns it off.
namespace stud::texture_cache {

// Bump whenever what a stored entry holds changes, so an entry written by
// an older build is never served. 13: entries are always the uncompressed
// decode; 12 and earlier held BC blocks from the removed re-encode.
inline constexpr uint32_t kFormatVersion = 13;

// Looks for a previously stored result. `key` identifies the source
// bytes and the target format together; see key_for(). Fills `dst` and
// returns true only on an exact size match.
bool load(uint64_t key_high, uint64_t key_low, void* dst, uint64_t bytes);

// Stores one. Failures are silent and harmless, a cache that cannot be
// written is a cache that misses.
void store(uint64_t key_high, uint64_t key_low, const void* src, uint64_t bytes);

// Hashes the source bytes together with everything that changes the
// output, so two different transcodes can never collide on one entry.
void key_for(const void* src, uint64_t src_bytes, uint32_t format, uint32_t target_format,
             uint32_t width, uint32_t height, uint64_t* key_high, uint64_t* key_low);

// How much of it to keep, in megabytes. 0 disables the cache entirely.
// Set once, at startup, from the config file; see
// `textureCacheMB` in config.json. Deliberately not in the settings
// window: it is a disk-space trade, not a thing to tune by feel.
void configure(uint64_t megabytes);

bool enabled();

}  // namespace stud::texture_cache
