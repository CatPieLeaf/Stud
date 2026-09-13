#include "stud/android_glue.h"
#include "stud/ndk_types.h"

#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

namespace {
std::string g_asset_base_dir;
}

namespace stud::android_glue {
void set_asset_base_directory(const std::string& path) { g_asset_base_dir = path; }
}  // namespace stud::android_glue

// AAssetManager is opaque to callers -- Roblox's code only ever holds and
// forwards the pointer, never dereferences it -- so an empty concrete type
// is enough; there's only ever one real manager (g_asset_base_dir above),
// identified by this fixed sentinel address.
struct AAssetManager {};

// AAsset wraps a whole file, mmap'd read-only -- matches real Android's
// AAsset_getBuffer zero-copy semantics directly, and requires no read/seek
// bookkeeping since libroblox.so only imports getBuffer/getLength/
// openFileDescriptor/close (confirmed via the M1 symbol survey), not
// AAsset_read/AAsset_seek.
struct AAsset {
    void* data;
    size_t size;
    int fd;
};

extern "C" {

AAssetManager* AAssetManager_fromJava(JNIEnv* /*env*/, jobject /*assetManager*/) {
    // No real Java AssetManager object exists in Stud -- the jobject
    // parameter is intentionally ignored.
    static AAssetManager sentinel{};
    return &sentinel;
}

AAsset* AAssetManager_open(AAssetManager* /*mgr*/, const char* filename, int /*mode*/) {
    std::string path = g_asset_base_dir + "/" + filename;
    int fd = ::open(path.c_str(), O_RDONLY);
    if (fd < 0) return nullptr;

    struct stat st{};
    if (::fstat(fd, &st) != 0) {
        ::close(fd);
        return nullptr;
    }

    void* mapped = nullptr;
    if (st.st_size > 0) {
        mapped = ::mmap(nullptr, static_cast<size_t>(st.st_size), PROT_READ, MAP_PRIVATE, fd, 0);
        if (mapped == MAP_FAILED) {
            ::close(fd);
            return nullptr;
        }
    }

    return new AAsset{mapped, static_cast<size_t>(st.st_size), fd};
}

void AAsset_close(AAsset* asset) {
    if (asset == nullptr) return;
    if (asset->data != nullptr) {
        ::munmap(asset->data, asset->size);
    }
    ::close(asset->fd);
    delete asset;
}

const void* AAsset_getBuffer(AAsset* asset) { return asset->data; }

off_t AAsset_getLength(AAsset* asset) { return static_cast<off_t>(asset->size); }

int AAsset_openFileDescriptor(AAsset* asset, off_t* outStart, off_t* outLength) {
    if (outStart != nullptr) *outStart = 0;
    if (outLength != nullptr) *outLength = static_cast<off_t>(asset->size);
    // A fresh fd the caller owns independently of this AAsset's own fd.
    return ::dup(asset->fd);
}

}  // extern "C"
