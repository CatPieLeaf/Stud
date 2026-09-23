#include "texture_cache.h"

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

#include <dirent.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/sysmacros.h>
#include <unistd.h>

#include "stud/stud_paths.h"
#define XXH_INLINE_ALL
#include "xxhash.h"


namespace stud::texture_cache {
namespace {

// Entries are spread over 256 directories by the first byte of the key,
// so no single directory ends up with tens of thousands of files,
// which is what makes a cache slower than the work it saves.
// Scoped by the format version (kFormatVersion).
//
// The key already mixes that version in, so an entry from an older
// encoder can never be hit -- but it still sat in the directory eating
// the size budget, and the prune below could not tell it from a live one.
// Nine version bumps in one evening left 515 MB of dead entries against a
// 500 MB cap, so every new encode evicted something it wanted back a
// moment later and no place ever finished loading.
//
// A generation gets its own directory, and the old ones are deleted
// whole. Eviction inside the live generation stays least-recently-used,
// which is the right question to ask of entries that can all still be hit.
std::string cache_root() {
    static const std::string root = stud::paths::cache_dir() + "/textures/v" +
                                    std::to_string(kFormatVersion);
    return root;
}

// Everything under textures/ that is not this generation.
void drop_old_generations() {
    const std::string parent = stud::paths::cache_dir() + "/textures";
    const std::string keep = "v" + std::to_string(kFormatVersion);
    DIR* dir_handle = ::opendir(parent.c_str());
    if (dir_handle == nullptr) return;
    uint64_t freed = 0;
    while (const dirent* entry = ::readdir(dir_handle)) {
        const std::string name = entry->d_name;
        if (name == "." || name == ".." || name == keep) continue;
        const std::string victim = parent + "/" + name;
        // Two levels at most: the generation, then its 256 buckets.
        DIR* gen = ::opendir(victim.c_str());
        if (gen != nullptr) {
            while (const dirent* bucket = ::readdir(gen)) {
                const std::string bname = bucket->d_name;
                if (bname == "." || bname == "..") continue;
                const std::string bpath = victim + "/" + bname;
                DIR* files = ::opendir(bpath.c_str());
                if (files != nullptr) {
                    while (const dirent* f = ::readdir(files)) {
                        const std::string fname = f->d_name;
                        if (fname == "." || fname == "..") continue;
                        struct stat info {};
                        const std::string fpath = bpath + "/" + fname;
                        if (::stat(fpath.c_str(), &info) == 0) freed += info.st_size;
                        ::unlink(fpath.c_str());
                    }
                    ::closedir(files);
                    ::rmdir(bpath.c_str());
                } else {
                    struct stat info {};
                    if (::stat(bpath.c_str(), &info) == 0) freed += info.st_size;
                    ::unlink(bpath.c_str());
                }
            }
            ::closedir(gen);
            ::rmdir(victim.c_str());
        }
    }
    ::closedir(dir_handle);
    if (freed > 0) {
        std::printf("stud: dropped %llu MB of texture cache from older builds\n",
                    static_cast<unsigned long long>(freed / (1024 * 1024)));
        std::fflush(stdout);
    }
}

std::string path_for(uint64_t key_high, uint64_t key_low) {
    char name[64];
    std::snprintf(name, sizeof(name), "%02x/%016llx%016llx",
                  static_cast<unsigned>(key_high >> 56),
                  static_cast<unsigned long long>(key_high),
                  static_cast<unsigned long long>(key_low));
    return cache_root() + "/" + name;
}

// How much of it to keep. Set from the config file; the default is a
// size a place's worth of textures fits in several times over.
// 4 GiB, not 500 MB.
//
// 500 MB was smaller than one session's working set. Measured on a live
// session: 5380 entries, 535 MB, in a single sitting -- already past the
// cap before the user had finished playing. Past that point every store
// evicts an entry that is wanted again minutes later, so the cache stops
// being a cache and becomes a queue of things about to be re-encoded.
// From the outside that is a place that never settles: textures climbing
// from a low mip to full resolution, over and over, for content that was
// encoded once already.
//
// The budget has to hold the mip chains of the places actually visited,
// and an uncompressed 1024x1024 level alone is 4 MB. textureCacheMB in config.json
// still overrides this, and 0 disables the cache entirely.
std::atomic<uint64_t> g_max_bytes{4096ull * 1024ull * 1024ull};

// Bytes stored since the last prune. The cap used to be enforced only at
// startup, which is not a cap at all for anyone who leaves Stud running
// a long session could grow the cache without limit. Counting what
// has been added since gives a cheap trigger to check again, without
// stat()ing the whole tree on every store.
std::atomic<uint64_t> g_stored_since_prune{0};

// Whether a prune is already running, so several stores crossing the
// threshold together do not start several walks over the same tree.
std::atomic<bool> g_pruning{false};

void prune_once() {
    struct Entry {
        std::string path;
        uint64_t bytes = 0;
        int64_t atime = 0;
    };
    std::vector<Entry> entries;
    uint64_t total = 0;
    for (unsigned bucket = 0; bucket < 256; ++bucket) {
        char dir_name[8];
        std::snprintf(dir_name, sizeof(dir_name), "%02x", bucket);
        const std::string dir = cache_root() + "/" + dir_name;
        DIR* dir_handle = ::opendir(dir.c_str());
        if (dir_handle == nullptr) continue;
        while (const dirent* entry = ::readdir(dir_handle)) {
            if (entry->d_name[0] == '.') continue;
            const std::string path = dir + "/" + entry->d_name;
            struct stat info {};
            if (::stat(path.c_str(), &info) != 0) continue;
            total += static_cast<uint64_t>(info.st_size);
            entries.push_back({path, static_cast<uint64_t>(info.st_size),
                               static_cast<int64_t>(info.st_atime)});
        }
        ::closedir(dir_handle);
    }
    const uint64_t cap = g_max_bytes.load();
    if (total <= cap) return;

    // Least recently used first: a texture nobody has looked at since the
    // last prune is the one least likely to be wanted again.
    std::sort(entries.begin(), entries.end(),
              [](const Entry& a, const Entry& b) { return a.atime < b.atime; });
    for (const Entry& entry : entries) {
        if (total <= cap) break;
        if (::unlink(entry.path.c_str()) == 0) total -= entry.bytes;
    }
    std::printf("stud: texture cache pruned to %llu MB\n",
                static_cast<unsigned long long>(total / (1024 * 1024)));
    std::fflush(stdout);
}

// Whether the cache directory lives on a spinning disk.
//
// The cache trades a decode for a file read, and that trade is only
// worth anything when the read is fast. On a spinning disk a random read
// costs a seek and the writes compete with the engine's own asset writes,
// so it is off there by default. So it is off there
// unless the config file says otherwise.
//
// The kernel answers this directly: a block device reports whether it
// rotates. The cache directory's device is found by its st_dev, and for
// a partition the answer lives one level up, on the whole disk.
bool on_rotational_disk() {
    struct stat info {};
    if (::stat(cache_root().c_str(), &info) != 0) return false;  // unknown: assume not
    char base[64];
    std::snprintf(base, sizeof(base), "/sys/dev/block/%u:%u",
                  static_cast<unsigned>(major(info.st_dev)),
                  static_cast<unsigned>(minor(info.st_dev)));
    for (const char* suffix : {"/queue/rotational", "/../queue/rotational"}) {
        const std::string path = std::string(base) + suffix;
        FILE* file = std::fopen(path.c_str(), "r");
        if (file == nullptr) continue;
        int value = 0;
        const bool read_ok = std::fscanf(file, "%d", &value) == 1;
        std::fclose(file);
        if (read_ok) return value == 1;
    }
    return false;
}

bool ensure_ready() {
    static const bool ready = [] {
        if (std::getenv("STUD_TEX_NO_CACHE") != nullptr) return false;
        // The limit comes from the config file, by way of Process A.
        // Absent (a manual or diagnostic launch) keeps the default.
        if (const char* megabytes = std::getenv("STUD_TEXTURE_CACHE_MB")) {
            g_max_bytes.store(static_cast<uint64_t>(std::atoll(megabytes)) * 1024ull * 1024ull);
        }
        if (g_max_bytes.load() == 0) return false;
        // Created before asking what it sits on, the question is about
        // the directory, so it has to exist first.
        ::mkdir(stud::paths::cache_dir().c_str(), 0700);
        ::mkdir((stud::paths::cache_dir() + "/textures").c_str(), 0700);
        ::mkdir(cache_root().c_str(), 0700);
        drop_old_generations();
        static const bool forced = std::getenv("STUD_TEX_CACHE_FORCE") != nullptr;
        if (!forced && on_rotational_disk()) {
            std::printf("stud: texture cache off, %s is on a spinning disk, where decoding "
                        "is about as cheap as reading it back (textureCacheMB in config.json, or "
                        "STUD_TEX_CACHE_FORCE=1, overrides)\n",
                        cache_root().c_str());
            std::fflush(stdout);
            return false;
        }
        for (unsigned bucket = 0; bucket < 256; ++bucket) {
            char dir_name[8];
            std::snprintf(dir_name, sizeof(dir_name), "%02x", bucket);
            ::mkdir((cache_root() + "/" + dir_name).c_str(), 0700);
        }
        prune_once();
        return true;
    }();
    return ready;
}

}  // namespace

void configure(uint64_t megabytes) { g_max_bytes.store(megabytes * 1024ull * 1024ull); }

bool enabled() { return ensure_ready(); }

void key_for(const void* src, uint64_t src_bytes, uint32_t format, uint32_t target_format,
             uint32_t width, uint32_t height, uint64_t* key_high, uint64_t* key_low) {
    // XXH3, 128 bits wide.
    //
    // Wide because a collision here is not a slow texture, it is the
    // WRONG texture, silently. XXH3-128 is built for that and is not
    // cryptographic, which this does not need.
    //
    // It replaced two hand-rolled FNV-1a streams. Measured over a
    // 1024x1024 ETC2 level, the 512 KB that actually gets hashed:
    // FNV-1a 0.067 ms at 7.84 GB/s, XXH3 0.025 ms at 21.28 GB/s. Small
    // in isolation, but this runs on every cache HIT, where there is no
    // decode to hide behind and it is most of what a hit costs.
    //
    // The metadata goes in as the seed rather than through the same
    // stream, so two textures with identical bytes but a different
    // format, size or encoder version cannot land on one key.
    const uint32_t meta[5] = {kFormatVersion, format, target_format,
                              width, height};
    XXH64_hash_t seed = XXH3_64bits(meta, sizeof(meta));
    seed ^= XXH3_64bits(&src_bytes, sizeof(src_bytes));
    const XXH128_hash_t h = XXH3_128bits_withSeed(src, static_cast<size_t>(src_bytes), seed);

    *key_high = h.high64;
    *key_low = h.low64;
}

bool load(uint64_t key_high, uint64_t key_low, void* dst, uint64_t bytes) {
    if (!ensure_ready() || dst == nullptr || bytes == 0) return false;
    const std::string path = path_for(key_high, key_low);
    const int fd = ::open(path.c_str(), O_RDONLY | O_CLOEXEC);
    if (fd < 0) return false;
    struct stat info {};
    if (::fstat(fd, &info) != 0 || static_cast<uint64_t>(info.st_size) != bytes) {
        // A size that does not match is not this texture, whatever the
        // key says. Refuse rather than hand back the wrong pixels.
        ::close(fd);
        return false;
    }
    uint64_t done = 0;
    while (done < bytes) {
        const ssize_t n = ::read(fd, static_cast<uint8_t*>(dst) + done,
                                 static_cast<size_t>(bytes - done));
        if (n <= 0) {
            ::close(fd);
            return false;
        }
        done += static_cast<uint64_t>(n);
    }
    ::close(fd);
    return true;
}

void store(uint64_t key_high, uint64_t key_low, const void* src, uint64_t bytes) {
    if (!ensure_ready() || src == nullptr || bytes == 0) return;
    const std::string path = path_for(key_high, key_low);
    // Written under a unique name and renamed into place, so a reader
    // never sees a half-written entry and two threads racing on the same
    // texture cannot corrupt each other's.
    static std::atomic<uint64_t> counter{0};
    char suffix[32];
    std::snprintf(suffix, sizeof(suffix), ".%d.%llu", static_cast<int>(::getpid()),
                  static_cast<unsigned long long>(counter.fetch_add(1)));
    const std::string temp = path + suffix;
    const int fd = ::open(temp.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0600);
    if (fd < 0) return;
    uint64_t done = 0;
    while (done < bytes) {
        const ssize_t n = ::write(fd, static_cast<const uint8_t*>(src) + done,
                                  static_cast<size_t>(bytes - done));
        if (n <= 0) break;
        done += static_cast<uint64_t>(n);
    }
    ::close(fd);
    if (done != bytes || ::rename(temp.c_str(), path.c_str()) != 0) {
        ::unlink(temp.c_str());
        return;
    }

    // Enforce the cap during the session, not only at startup, but
    // never on the thread that is storing.
    //
    // Pruning walks the whole tree and stats every entry, and the caller
    // here is a texture-decode thread the engine is waiting on. Doing it
    // inline turns the cache into the thing it exists to prevent: a stall
    // in the middle of loading. It runs on a thread of its own instead,
    // one at a time, and nothing waits for it.
    const uint64_t added = g_stored_since_prune.fetch_add(bytes) + bytes;
    if (added >= g_max_bytes.load() / 8) {
        g_stored_since_prune.store(0);
        bool expected = false;
        if (g_pruning.compare_exchange_strong(expected, true)) {
            std::thread([] {
                prune_once();
                g_pruning.store(false);
            }).detach();
        }
    }
}

}  // namespace stud::texture_cache
