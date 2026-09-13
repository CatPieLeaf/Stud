#pragma once

#include <cstdint>

// Transcoded texture blocks, kept between runs.
//
// Decoding an ETC2/EAC block and re-encoding it to BC7 is the single most
// expensive thing Stud does per texture: a 1024x1024 level is 65,536
// blocks, and a place loading its content delivers a flood of them at
// once. That is the CPU spike on launch and on teleporting somewhere new.
//
// The work is perfectly deterministic -- the same source bytes and the
// same target format always produce the same output -- and Roblox's
// content is immutable, so the result is worth keeping. A hit turns tens
// of milliseconds of encoding into a file read.
//
// It lives in the cache directory, where a cache cleaner is entitled to
// delete it: losing it costs the time to encode again and nothing else.
// STUD_TEX_NO_CACHE=1 turns it off.
namespace stud::texture_cache {

// Looks for a previously stored result. `key` identifies the source
// bytes and the target format together; see key_for(). Fills `dst` and
// returns true only on an exact size match.
bool load(uint64_t key_high, uint64_t key_low, void* dst, uint64_t bytes);

// Stores one. Failures are silent and harmless -- a cache that cannot be
// written is a cache that misses.
void store(uint64_t key_high, uint64_t key_low, const void* src, uint64_t bytes);

// Hashes the source bytes together with everything that changes the
// output, so two different transcodes can never collide on one entry.
void key_for(const void* src, uint64_t src_bytes, uint32_t format, uint32_t target_format,
             uint32_t width, uint32_t height, uint64_t* key_high, uint64_t* key_low);

// How much of it to keep, in megabytes. 0 disables the cache entirely.
// Set once, at startup, from the config file -- see
// `textureCacheMB` in config.json. Deliberately not in the settings
// window: it is a disk-space trade, not a thing to tune by feel.
void configure(uint64_t megabytes);

bool enabled();

}  // namespace stud::texture_cache
