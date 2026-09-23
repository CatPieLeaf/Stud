// Real libvulkan.so.1, bionic-compiled, placed at /system/lib64/.
// Real bionic's own linker64 never loads this via a DT_NEEDED entry the
// way it does libEGL.so/libGLESv2.so (the ELF headers --dyn-syms on the real
// libroblox.so shows zero "vk*" entries in its dynamic symbol table):
// confirmed live that
// Roblox resolves its own Vulkan entry point exactly the way the
// Vulkan spec requires, dlopen("libvulkan.so.1", RTLD_NOW) (falling
// back to "libvulkan.so"), then dlsym(handle, "vkGetInstanceProcAddr")
// and resolves every other command, including vkGetDeviceProcAddr
// itself, BY NAME through that one function, never via a second dlsym.
// So this file's only real exported C symbol is vkGetInstanceProcAddr;
// everything else is a runtime name lookup below.
//
// First cut, deliberately trace-only (Phase 3's dynamic-capture half,
// combined with Phase 4's first-cut client stub per this project's own
// plan): render-host's Vulkan proxy protocol doesn't exist yet beyond
// the one legacy vkCreateAndroidSurfaceKHR CallId
// (VkCreateWaylandSurfaceForAndroidSurface) carried over from the old
// architecture's vulkan-wsi/ design. Every OTHER command name Roblox
// asks for returns nullptr here (a real, spec-legal "not supported"
// answer, the Vulkan spec explicitly allows vkGetInstanceProcAddr to
// return NULL for anything not present) and, if
// STUD_VULKAN_CALL_TRACE=1, is logged, the real, evidence-based
// signal this project needs to build the actual command list, instead
// of guessing from the ~580-entry embedded dispatch-table-over-count
// already ruled out as unreliable
// (see  notes from that session).

#include <cstdio>
#include <cstdlib>
#include <algorithm>
#include <cstring>
#include <condition_variable>
#include <deque>
#include <functional>
#include <set>
#include <thread>
#include <cerrno>
#include <fcntl.h>
#include <sys/mman.h>
#include <unistd.h>
#include <string>
#include <string_view>
#include <map>
#include <type_traits>
#include <utility>
#include <vector>

#include <vulkan/vulkan_core.h>

// Header-only, the same way texture_cache.cpp takes it: the symbols
// are static to this translation unit rather than linked.
#define XXH_INLINE_ALL
#include "xxhash.h"

#include "mapped_write_barrier.h"
#include "render_client_common.h"
#include "uffd_scan.h"
#include "texture_decode.h"
#include "stud/vulkan_forward.h"

using stud::render_host::CallId;
namespace vk_wire = stud::render_host::vk_wire;

namespace {

bool trace_enabled() {
    const char* env = std::getenv("STUD_VULKAN_CALL_TRACE");
    return env != nullptr && std::string_view(env) == "1";
}

void trace(const char* kind, const char* name) {
    if (trace_enabled()) {
        std::fprintf(stderr, "stud: vulkan-client: %s requested \"%s\"\n", kind, name == nullptr ? "(null)" : name);
        std::fflush(stderr);
    }
}

// Both enumerate calls follow the real two-call idiom: pProperties null
// means "how many are there", otherwise *pCount is the caller's capacity
// and gets written back with how many were actually returned.
template <typename WireT, typename VkT, CallId kId>
VkResult enumerate_properties(const char* layer_name, uint32_t* pCount, VkT* pProperties,
                              void (*convert)(const WireT&, VkT&)) {
    if (pCount == nullptr) return VK_ERROR_INITIALIZATION_FAILED;
    const uint32_t capacity = pProperties != nullptr ? *pCount : 0;
    uint64_t a[8] = {capacity};

    std::string filter = layer_name != nullptr ? layer_name : "";
    std::vector<uint8_t> out(sizeof(uint32_t) + sizeof(WireT) * (capacity + 1));
    uint32_t written = 0;
    uint64_t r = stud::render_client::connection().call(
        kId, a, filter.c_str(), static_cast<uint32_t>(filter.size() + 1), out.data(),
        static_cast<uint32_t>(out.size()), &written);
    if (written < sizeof(uint32_t)) return VK_ERROR_INITIALIZATION_FAILED;

    uint32_t count = 0;
    std::memcpy(&count, out.data(), sizeof(count));
    if (pProperties == nullptr) {
        *pCount = count;
        return static_cast<VkResult>(static_cast<int32_t>(r));
    }

    const uint32_t have = (written - sizeof(uint32_t)) / sizeof(WireT);
    const uint32_t n = count < have ? count : have;
    // What is really in pProperties when this returns. The PVRTC entry
    // below is written AT index n, so reporting n alone said "there are n
    // extensions" while having written n + 1 -- the emulated extension was
    // handed over and then hidden, so nothing could ever find it. The
    // engine only uses PVRTC formats if it sees this extension.
    uint32_t written_back = n;
    for (uint32_t i = 0; i < n; ++i) {
        WireT w{};
        std::memcpy(&w, out.data() + sizeof(uint32_t) + i * sizeof(WireT), sizeof(WireT));
        convert(w, pProperties[i]);
    }
    *pCount = n;
    return static_cast<VkResult>(static_cast<int32_t>(r));
}

// One distinct real function per slot, so the handler can name which
// Vulkan command was called. Mirrors the GL path's own named-no-op
// mechanism; VK_ERROR_INITIALIZATION_FAILED is an honest "not available"
// for a command that genuinely is not implemented yet.
constexpr int kMaxUnimplemented = 640;
const char* g_unimplemented_names[kMaxUnimplemented];
int g_unimplemented_count = 0;

template <int kIndex>
VKAPI_ATTR VkResult VKAPI_CALL unimplemented_slot() {
    std::fprintf(stderr, "stud: vulkan-client: CALLED unimplemented Vulkan function \"%s\"\n",
                 g_unimplemented_names[kIndex]);
    std::fflush(stderr);
    return VK_ERROR_INITIALIZATION_FAILED;
}

// Filled in chunks: a single fold over 640 slots exceeds clang's
// expression nesting limit.
template <int kBase, int... kIs>
constexpr void fill_chunk(PFN_vkVoidFunction (&table)[kMaxUnimplemented],
                          std::integer_sequence<int, kIs...>) {
    ((table[kBase + kIs] =
          reinterpret_cast<PFN_vkVoidFunction>(&unimplemented_slot<kBase + kIs>)),
     ...);
}

constexpr int kChunk = 64;
template <int... kChunkIs>
constexpr void fill_slots(PFN_vkVoidFunction (&table)[kMaxUnimplemented],
                          std::integer_sequence<int, kChunkIs...>) {
    (fill_chunk<kChunkIs * kChunk>(table, std::make_integer_sequence<int, kChunk>{}), ...);
}

// Does the real driver have this command? Cached: the engine resolves
// its whole dispatch table repeatedly, and this is a blocking call.
//
// This is what keeps an unimplemented command honest. Handing back a
// non-null stub for everything told the engine that extensions the
// driver does not have were available, and for
// vkGetRefreshCycleDurationGOOGLE (VK_GOOGLE_display_timing) that was
// not harmless: an extension entry point is exactly what an application
// null-checks to decide whether to use the extension, so the engine
// used it, read a meaningless refresh duration out of a stub, and paced
// frames to it. A real driver answers null for what it does not have,
// and so must this.
bool host_has_proc(const char* name) {
    static std::map<std::string, bool> cache;
    auto it = cache.find(name);
    if (it != cache.end()) return it->second;
    uint64_t a[8] = {};
    const uint64_t r = stud::render_client::connection().call(
        CallId::VkHostHasProc, a, name, static_cast<uint32_t>(std::strlen(name)), nullptr, 0,
        nullptr);
    const bool has = r != 0;
    cache.emplace(name, has);
    return has;
}

PFN_vkVoidFunction unimplemented_stub_for(const char* name) {
    static PFN_vkVoidFunction table[kMaxUnimplemented] = {};
    static bool filled = false;
    if (!filled) {
        filled = true;
        fill_slots(table, std::make_integer_sequence<int, kMaxUnimplemented / kChunk>{});
    }
    // Same name twice must map to the same slot: the engine resolves a
    // command once but may re-resolve after creating a device.
    for (int i = 0; i < g_unimplemented_count; ++i) {
        if (std::strcmp(g_unimplemented_names[i], name) == 0) return table[i];
    }
    if (g_unimplemented_count >= kMaxUnimplemented) return nullptr;
    const int slot = g_unimplemented_count++;
    g_unimplemented_names[slot] = strdup(name);
    return table[slot];
}

// A POD reply is prefixed with the size the host wrote. A mismatch means
// the two processes disagree about a Vulkan struct layout, which would
// otherwise be read as garbage, so it is reported and refused.
template <typename T>
bool read_pod(const uint8_t* buf, uint32_t written, T& value, const char* what) {
    if (written < sizeof(uint32_t)) return false;
    uint32_t size = 0;
    std::memcpy(&size, buf, sizeof(size));
    if (size != sizeof(T) || written < sizeof(uint32_t) + size) {
        std::fprintf(stderr,
                     "stud: vulkan-client: %s struct size mismatch (host %u, client %zu), "
                     "the two processes disagree about the Vulkan headers\n",
                     what, size, sizeof(T));
        std::fflush(stderr);
        return false;
    }
    std::memcpy(&value, buf + sizeof(uint32_t), sizeof(T));
    return true;
}

// Flattens a pNext chain for the wire, and counts the nodes. An sType
// this build cannot size is skipped, both processes share the table
// (vulkan_chain.cpp), so this is the same decision on both sides.
uint32_t flatten_chain(const void* head, std::vector<uint8_t>& out) {
    uint32_t count = 0;
    for (auto* n = static_cast<const VkBaseInStructure*>(head); n != nullptr; n = n->pNext) {
        const uint32_t size = vk_wire::chain_node_size(static_cast<uint32_t>(n->sType));
        if (size == 0) {
            std::fprintf(stderr, "stud: vulkan-client: skipping unknown pNext sType=%u\n",
                         static_cast<uint32_t>(n->sType));
            std::fflush(stderr);
            continue;
        }
        vk_wire::ChainNodeHeader h{static_cast<uint32_t>(n->sType), size};
        const size_t at = out.size();
        out.resize(at + sizeof(h) + size);
        std::memcpy(out.data() + at, &h, sizeof(h));
        std::memcpy(out.data() + at + sizeof(h), n, size);
        ++count;
    }
    return count;
}

// Copies the reply's nodes back into the caller's own chain, matched by
// sType, the driver may not fill them in the order they were sent.
void scatter_chain(void* head, const uint8_t* p, size_t len) {
    size_t off = 0;
    while (off + sizeof(vk_wire::ChainNodeHeader) <= len) {
        vk_wire::ChainNodeHeader h{};
        std::memcpy(&h, p + off, sizeof(h));
        off += sizeof(h);
        if (h.size == 0 || off + h.size > len) break;
        for (auto* n = static_cast<VkBaseOutStructure*>(head); n != nullptr; n = n->pNext) {
            if (static_cast<uint32_t>(n->sType) == h.s_type &&
                vk_wire::chain_node_size(h.s_type) == h.size) {
                void* saved_next = n->pNext;
                std::memcpy(n, p + off, h.size);
                n->pNext = static_cast<VkBaseOutStructure*>(saved_next);
                break;
            }
        }
        off += h.size;
    }
}

// Handles are pointer-typed on a 64-bit build, plain uint64 elsewhere.
template <typename H>
uint64_t to_u64(H h) {
    if constexpr (std::is_pointer_v<H>) {
        return static_cast<uint64_t>(reinterpret_cast<uintptr_t>(h));
    } else {
        return static_cast<uint64_t>(h);
    }
}
template <typename H>
H from_u64(uint64_t v) {
    if constexpr (std::is_pointer_v<H>) {
        return reinterpret_cast<H>(static_cast<uintptr_t>(v));
    } else {
        return static_cast<H>(v);
    }
}

// Shared shape for the simple creates: send scalars in args, get one
// handle back.
template <typename H, CallId kId>
VkResult simple_create(const uint64_t (&a)[8], H* outHandle) {
    if (outHandle == nullptr) return VK_ERROR_INITIALIZATION_FAILED;
    uint64_t handle = 0;
    uint32_t written = 0;
    uint64_t r = stud::render_client::connection().call(kId, a, nullptr, 0, &handle,
                                                         sizeof(handle), &written);
    VkResult result = static_cast<VkResult>(static_cast<int32_t>(r));
    if (result != VK_SUCCESS || written < sizeof(handle)) {
        return result != VK_SUCCESS ? result : VK_ERROR_INITIALIZATION_FAILED;
    }
    *outHandle = from_u64<H>(handle);
    return VK_SUCCESS;
}

// What the driver will ask for, remembered by the SHAPE of the object
// rather than by its handle.
//
// vkGetBufferMemoryRequirements and vkGetImageMemoryRequirements are two
// of the blocking round trips left on the engine's own thread, and the
// engine makes one of each per resource it creates -- which, while an
// experience streams textures and meshes in, is continuous. None of them
// could be batched away, because the engine reads the answer immediately.
//
// They can be answered without asking, though, because the answer is a
// pure function of the creation parameters. Vulkan says so directly: 1.3
// added vkGetDeviceBufferMemoryRequirements and its image counterpart,
// which return the requirements from a VkBufferCreateInfo/
// VkImageCreateInfo ALONE, with no object in existence. An object's
// requirements therefore cannot depend on anything but its create info,
// and two objects created identically must be told the same thing.
//
// So the round trip is worth making once per distinct shape. Roblox
// creates the same shapes over and over -- same formats, same mip chains,
// same usage flags -- so after the first of each, this answers from
// memory.
//
// Keyed on the exact serialised create parameters, and COMPARED in full
// rather than by a hash. A hash collision here would hand the engine
// another object's alignment or size, which is memory corruption that
// would surface far from its cause; the key is a few dozen bytes and the
// comparison is cheaper than the syscall it avoids either way.
class MemoryRequirementsCache {
public:
    // Called at create time, once the real handle is known.
    void remember_shape(uint64_t handle, std::string shape) {
        std::lock_guard<std::mutex> lock(mutex_);
        shape_of_[handle] = std::move(shape);
    }

    bool get(uint64_t handle, VkMemoryRequirements& out) {
        std::lock_guard<std::mutex> lock(mutex_);
        const auto h = shape_of_.find(handle);
        if (h == shape_of_.end()) return false;
        const auto r = by_shape_.find(h->second);
        if (r == by_shape_.end()) return false;
        out = r->second;
        ++hits_;
        return true;
    }

    void put(uint64_t handle, const VkMemoryRequirements& reqs) {
        std::lock_guard<std::mutex> lock(mutex_);
        const auto h = shape_of_.find(handle);
        if (h == shape_of_.end()) return;
        by_shape_[h->second] = reqs;
        ++misses_;
    }

    // A handle is only as good as the object behind it, and the driver
    // reuses both. Dropping it at destroy is what stops a recycled handle
    // answering with the previous object's shape.
    void forget(uint64_t handle) {
        std::lock_guard<std::mutex> lock(mutex_);
        shape_of_.erase(handle);
    }

    // STUD_VK_MEMREQ_STATS=1. "It caches" is not a measurement; the ratio
    // is, and a cache that never hits is worth knowing about.
    void report_if_asked() {
        static const bool on = std::getenv("STUD_VK_MEMREQ_STATS") != nullptr;
        if (!on) return;
        std::lock_guard<std::mutex> lock(mutex_);
        if ((hits_ + misses_) % 4096 != 0) return;
        std::fprintf(stderr,
                     "stud: vulkan-client: memory requirements %llu answered from memory, "
                     "%llu asked the host, %zu distinct shapes\n",
                     static_cast<unsigned long long>(hits_),
                     static_cast<unsigned long long>(misses_), by_shape_.size());
    }

private:
    std::mutex mutex_;
    std::map<uint64_t, std::string> shape_of_;
    std::map<std::string, VkMemoryRequirements> by_shape_;
    uint64_t hits_ = 0;
    uint64_t misses_ = 0;
};

MemoryRequirementsCache& mem_req_cache() {
    static MemoryRequirementsCache c;
    return c;
}


// Staging for mapped device memory. A real mapping is a pointer into
// driver-owned memory in Process C and cannot be shared, so the engine
// writes into a local buffer here and the bytes are shipped over on
// flush or unmap, the same emulation the GL path uses for
// glMapBufferRange.
//
// Honest limitation, stated where it matters: a mapping is not
// pre-filled with the memory's existing contents, so a read-modify-write
// of previously-written bytes through a fresh map would not see them.
// Roblox's Vulkan renderer writes whole ranges it owns, which is the
// case this serves.
struct MappedRange {
    // Where the engine actually writes. Page-aligned and write-protected
    // between flushes so the MMU records which pages changed; see
    // mapped_write_barrier.h for why this replaced a shadow-copy
    // comparison (that comparison was correct, but `memcmp` over every
    // mapped byte on every submit measured ~45% of the engine thread).
    stud::render_client::MappedWriteBarrier barrier;
    // Fallback when the barrier cannot be installed or allocated: the
    // engine's writes still have to reach the device, so this keeps the
    // previous compare-against-a-shadow behaviour rather than dropping
    // them. Empty whenever the barrier is in use.
    std::vector<uint8_t> staging;
    std::vector<uint8_t> shadow;
    // Which pages of `shadow` are known to match what the host holds,
    // one byte per page.
    //
    // The shadow is only ever the truth for a page Stud has actually
    // SENT. Seeding it from the engine's own buffer instead makes every
    // page compare equal to memory the host has never been given, and
    // nothing is sent at all -- a white, frozen window, live-caught.
    // A page is trusted here only after its bytes went down the socket.
    std::vector<uint8_t> shadow_has;
    // True when the host maps the very same pages the engine writes
    // through, so a flush names a run instead of carrying it.
    bool shared_with_host = false;
    uint64_t offset = 0;
    VkDevice device = VK_NULL_HANDLE;

    uint8_t* bytes() { return barrier.valid() ? barrier.data() : staging.data(); }
    std::size_t length() const { return barrier.valid() ? barrier.size() : staging.size(); }
};

// Images whose format this process decodes itself. The engine believes it
// created an ETC/PVRTC image; the device was handed an uncompressed one,
// and every upload is decoded on the way through. See texture_decode.h for
// why, in short, NVIDIA has no ETC2 at all and the engine's textures are
// visibly worse without it.
struct EmulatedImage {
    VkFormat compressed = VK_FORMAT_UNDEFINED;
    uint32_t width = 0;
    uint32_t height = 0;
};
std::map<uint64_t, EmulatedImage>& emulated_images() {
    static std::map<uint64_t, EmulatedImage> m;
    return m;
}
// Where each buffer's memory lives, so a staging buffer's bytes can be
// found when a copy out of it has to be decoded. Only what
// vkBindBufferMemory already tells us.
struct BufferBinding {
    uint64_t memory = 0;
    uint64_t offset = 0;
    // What the buffer was created with. A readback out of it has to be
    // fetched from the host, and this is the only bound on how far; see
    // fetch_pending_readbacks().
    uint64_t size = 0;
};
std::map<uint64_t, BufferBinding>& buffer_bindings() {
    static std::map<uint64_t, BufferBinding> m;
    return m;
}
// Sizes as created, kept until the buffer is bound. Same lock as
// buffer_bindings().
std::map<uint64_t, uint64_t>& buffer_sizes() {
    static std::map<uint64_t, uint64_t> m;
    return m;
}
// Whether a format Stud CAN decode actually needs decoding on this
// device. A driver that implements ETC2 or PVRTC natively is left to do
// it: hardware sampling of a compressed texture is smaller in memory and
// faster than anything done here, so emulation is strictly a fallback for
// a device that would otherwise refuse the format outright. Cached
// because it is asked once per image and the answer cannot change for a
// given physical device.
// The device Stud's own decode path allocates its scratch buffers from.
// The engine creates exactly one, and these are only ever used to service
// its own uploads on that device.
VkDevice g_decode_device = VK_NULL_HANDLE;
VkPhysicalDevice g_decode_physical_device = VK_NULL_HANDLE;

// Scratch upload buffers for decoded texture data, one pool per command
// buffer. A pool is rewound when its command buffer begins recording
// again, which is the point at which the engine has already waited for
// that buffer's previous submission, so nothing in flight is ever
// overwritten, and the buffers themselves are reused rather than churned.
struct ScratchBuffer {
    VkBuffer buffer = VK_NULL_HANDLE;
    VkDeviceMemory memory = VK_NULL_HANDLE;
    uint8_t* mapped = nullptr;
    VkDeviceSize size = 0;
};
// One growing scratch buffer per command buffer, sub-allocated by a bump
// pointer that rewinds when that buffer begins recording again, the
// point at which the engine has already waited for its previous
// submission, so nothing in flight is overwritten.
//
// A buffer per copy was the obvious first shape and it is the wrong one:
// loading a place records hundreds of texture uploads, which became
// hundreds of separately mapped ranges, all live at once and all walked
// on every flush. That froze the client on joining a game.
struct ScratchPool {
    ScratchBuffer current;
    VkDeviceSize used = 0;
    // Buffers outgrown mid-frame. They may still be referenced by work in
    // flight, so they are kept rather than destroyed; growth doubles, so
    // there are only ever a handful.
    std::vector<ScratchBuffer> retired;
};
// A decode that has been recorded but not yet performed. The copy is
// recorded pointing at scratch immediately, and the bytes are decoded at
// submit; see run_pending_decodes() for why that is not done at record
// time.
struct PendingDecode {
    VkCommandBuffer cb = VK_NULL_HANDLE;
    VkBuffer src = VK_NULL_HANDLE;
    uint64_t src_offset = 0;
    uint64_t src_layer_bytes = 0;
    uint64_t dst_layer_bytes = 0;
    uint64_t row_pitch = 0;
    uint8_t* dst = nullptr;
    VkFormat format = VK_FORMAT_UNDEFINED;
    uint32_t width = 0;
    uint32_t height = 0;
    uint32_t layers = 1;
    // The allocation this decode writes into, so those bytes can be sent
    // once they exist; see send_decoded_scratch().
    VkDeviceMemory scratch_memory = VK_NULL_HANDLE;
    // A decode that was already done, speculatively, when this copy was
    // recorded rather than when it was submitted. See early_decode().
    //
    // The hash is of the SOURCE bytes as they stood at that moment. A
    // copy's source only has to be correct when the copy executes, so
    // the engine is entitled to keep writing after recording it; the
    // hash is what says whether it did. Equal at submit means the
    // speculative decode describes the bytes the copy will actually
    // read, and the work is already done.
    bool speculated = false;
    uint64_t spec_hash_high = 0;
    uint64_t spec_hash_low = 0;
    // Diagnostic only: which image this upload is for, so a burst can be
    // read as "one texture streamed in" or "the same texture re-uploaded".
    uint64_t image = 0;
};
std::vector<PendingDecode>& pending_decodes() {
    static std::vector<PendingDecode> v;
    return v;
}

// A vkCmdCopyImageToBuffer whose result the GPU writes into memory the
// host could NOT import, so the engine's own pages never see it.
//
// Where the import works -- every NVIDIA session -- the GPU writes
// straight into the pages the engine reads and there is nothing to do,
// which is why this whole path did not exist. AMD refuses the import
// (amdgpu's userptr is ANONONLY, Stud's mapping is file-backed), so every
// readback there landed in the host's private copy and the engine read
// whatever happened to be in its own: uninitialised memory, which is
// exactly what "random textures" and "squares of horizontal noise" are.
//
// Recorded when the copy is recorded, promoted to awaiting when the
// command buffer is submitted, and fetched once something has waited for
// that submit to finish.
struct PendingReadback {
    VkCommandBuffer cb = VK_NULL_HANDLE;
    uint64_t memory = 0;
    uint64_t offset = 0;
    uint64_t size = 0;
};
std::vector<PendingReadback>& recorded_readbacks() {
    static std::vector<PendingReadback> v;
    return v;
}
std::vector<PendingReadback>& awaiting_readbacks() {
    static std::vector<PendingReadback> v;
    return v;
}
// Read without the lock on the path every fence wait takes, so a session
// that never reads anything back pays one relaxed load.
std::atomic<bool> g_have_readbacks{false};

std::map<uint64_t, ScratchPool>& scratch_pools() {
    static std::map<uint64_t, ScratchPool> m;
    return m;
}

// Recursive on purpose: allocating decode scratch happens with this held
// and goes back through Stud's own vkCreateBuffer/vkBindBufferMemory,
// which record into the very maps it guards. A plain mutex deadlocks
// there, live-caught, as a window that never appeared at all.
std::recursive_mutex& emulation_mutex() {
    static std::recursive_mutex m;
    return m;
}

std::map<uint64_t, MappedRange>& mapped_ranges() {
    static std::map<uint64_t, MappedRange> m;
    return m;
}

// Guards mapped_ranges() itself.
//
// Vulkan lets one thread map or free memory while another submits, and
// the engine really does: asset threads map and unmap while the render
// thread is inside vkQueueSubmit, whose flush walks this whole table.
// Stud's own decode workers map scratch buffers from their own threads
// too. A std::map is a red-black tree, so an insert rotates nodes under
// a concurrent walk and an erase frees one out from under it: a mapping
// silently skipped by a flush (its writes never sent, which is stale
// memory for the GPU to read) or a use-after-free.
//
// Recursive because vkUnmapMemory flushes before erasing, and both want
// the table.
std::recursive_mutex& mapped_mutex() {
    static std::recursive_mutex m;
    return m;
}

// STUD_VK_FRAME_TIME=1: where a frame's wall clock actually goes.
// CPU sitting well under one core while frames are slow means the thread
// is waiting, not computing; this says on what.
struct FrameTiming {
    double acquire_ms = 0;
    double submit_ms = 0;
    double present_ms = 0;
    double fence_ms = 0;
    double query_ms = 0;      // GPU query readback, which really can stall
    // Everything else that blocks on a reply from the host. `other` was
    // 45ms in a frame issuing only 207 commands, the engine was not
    // busy, it was waiting, so the blocking calls that were not being
    // timed have to be visible before any theory about them is worth
    // anything.
    double wait_idle_ms = 0;
    uint64_t wait_idle_calls = 0;
    double blocking_ms = 0;   // every other call() that waits for a reply
    uint64_t blocking_calls = 0;
    // Time inside record() itself, appending a command to the
    // reply-free queue. With waitIdle and blocking both measured at
    // zero, `other` is CPU somewhere, and this is the one path that
    // scales with cmds/frame (3708 commands in a 46ms frame is 12.4us
    // each, which is far too slow for a memcpy into a vector).
    double record_ms = 0;
    // Averages hide stutter: a run can average 144fps and still drop
    // frames badly. Track the worst frame and how many miss a 144Hz and
    // a 60Hz deadline, which is what "a ton of drops" actually means.
    double worst_ms = 0;
    uint64_t over_7ms = 0;
    uint64_t over_16ms = 0;
    uint64_t cmds = 0;        // command-buffer records issued
    uint64_t cmd_bytes = 0;   // and how much they carried
    double frame_ms = 0;
    int frames = 0;
    // Averages cannot explain a spike, and neither can a worst-frame
    // number on its own: what is needed is what that particular frame
    // spent its time on. These reset every frame, and a frame that misses
    // its deadline prints its own breakdown.
    double f_acquire = 0;
    double f_submit = 0;
    double f_present = 0;
    double f_record = 0;
    double f_blocking = 0;
    double f_decode = 0;
    // Time inside Stud's own command entry points, start to finish,
    // serialising the arguments and appending to the batch, not just the
    // append. With ten thousand commands a frame, this is the number that
    // says whether a slow frame is Stud's doing or the engine's.
    double f_stub = 0;
    double stub_ms = 0;
    uint64_t f_cmds = 0;
    uint64_t f_bytes = 0;
    // Texture-decode work across the reporting window, so a burst that
    // costs CPU and IPC bandwidth shows up next to the frame it landed in.
    double decode_ms = 0;
    uint64_t decode_bytes = 0;
    std::chrono::steady_clock::time_point last_present{};
};

FrameTiming& frame_timing() {
    static FrameTiming t;
    return t;
}

bool frame_timing_enabled() {
    static const bool on = std::getenv("STUD_VK_FRAME_TIME") != nullptr;
    return on;
}

void report_frame_timing() {
    FrameTiming& t = frame_timing();
    if (t.frames < 120) return;
    // The format used to be short of its arguments: worst_ms, over_7ms and
    // over_16ms were passed and never printed, so everything after "other"
    // was reading the wrong argument, which is why "cmds/frame" sat at
    // the same number all session whatever the scene did. Averages were
    // fine; the spike counters, the thing worth having, were not.
    std::printf("stud: vulkan-client: %d frames avg %.2fms (%.0f fps), acquire %.2f submit %.2f "
                "present %.2f fence %.2f query %.2f waitIdle %.2f(%llu) blocking %.2f(%llu) "
                "record %.2f, other %.2f: worst %.1fms, %llu over 7ms, %llu over 16ms, "
                "%llu cmds/frame, %llu KB/frame: decode %.2f ms/frame, %llu KB/frame\n",
                t.frames, t.frame_ms / t.frames, 1000.0 * t.frames / t.frame_ms,
                t.acquire_ms / t.frames, t.submit_ms / t.frames, t.present_ms / t.frames,
                t.fence_ms / t.frames, t.query_ms / t.frames,
                t.wait_idle_ms / t.frames,
                static_cast<unsigned long long>(t.wait_idle_calls / t.frames),
                t.blocking_ms / t.frames,
                static_cast<unsigned long long>(t.blocking_calls / t.frames),
                t.record_ms / t.frames,
                (t.frame_ms - t.acquire_ms - t.submit_ms - t.present_ms - t.fence_ms -
                 t.query_ms - t.wait_idle_ms - t.blocking_ms - t.record_ms) / t.frames,
                t.worst_ms, static_cast<unsigned long long>(t.over_7ms),
                static_cast<unsigned long long>(t.over_16ms),
                static_cast<unsigned long long>(t.cmds / t.frames),
                static_cast<unsigned long long>(t.cmd_bytes / t.frames / 1024),
                t.decode_ms / t.frames,
                static_cast<unsigned long long>(t.decode_bytes / t.frames / 1024));
    std::fflush(stdout);
    t.decode_ms = 0;
    t.decode_bytes = 0;
    t.acquire_ms = t.submit_ms = t.present_ms = t.frame_ms = t.fence_ms = 0;
    t.wait_idle_ms = t.blocking_ms = t.record_ms = 0;
    t.worst_ms = 0;
    t.over_7ms = t.over_16ms = 0;
    t.wait_idle_calls = t.blocking_calls = 0;
    t.query_ms = 0;
    t.cmds = 0;
    t.cmd_bytes = 0;
    t.frames = 0;
}

// Bytes actually put on the wire, and bytes a flush asked for.
// STUD_VK_MEM_STATS=1 reports both, the ratio is what says whether the
// dirty tracking is earning its keep on a given workload.
uint64_t g_mapped_bytes_sent = 0;
uint64_t g_mapped_bytes_shared = 0;
int g_shared_maps = 0;
int g_copied_maps = 0;
uint64_t g_mapped_bytes_asked = 0;

void send_mapped_run(uint64_t memory, MappedRange& m, uint64_t rel_offset, uint64_t n) {
    uint64_t a[8] = {0, memory, m.offset + rel_offset};
    // Inside an action on the writer thread -- the texture decode -- this
    // has to go down the socket right here. Queueing it would put it
    // behind the submit that reads it, and a blocking call would wait for
    // a reply that only this thread can read. See Client::write_inline().
    if (stud::render_client::connection().on_writer_thread()) {
        a[3] = n;
        const bool sent =
            m.shared_with_host
                ? stud::render_client::connection().write_inline(CallId::VkWriteSharedMappedMemory,
                                                                 a, nullptr, 0)
                : stud::render_client::connection().write_inline(CallId::VkWriteMappedMemory, a,
                                                                 m.bytes() + rel_offset,
                                                                 static_cast<uint32_t>(n));
        if (!sent) {
            static bool warned = false;
            if (!warned) {
                warned = true;
                std::fprintf(stderr,
                             "stud: vulkan-client: could not send a decoded texture inline\n");
            }
        }
        if (m.shared_with_host) g_mapped_bytes_shared += n;
        g_mapped_bytes_sent += n;
        return;
    }
    if (m.shared_with_host) {
        g_mapped_bytes_shared += n;
        // The bytes are already in the host's mapping of this file, so
        // this only names the run -- and it needs no answer.
        //
        // call() waits for a reply. A frame has hundreds of dirty runs,
        // and waiting for each one was the whole of the 10ms `submit`
        // that survived sharing the pages: the bytes had stopped
        // travelling but the round trips had not. call_void() queues it
        // with the rest, in order, which is all this needs -- the submit
        // that reads the memory goes down the same stream behind it.
        a[3] = n;
        stud::render_client::connection().call_void(CallId::VkWriteSharedMappedMemory, a);
    } else {
        stud::render_client::connection().call(CallId::VkWriteMappedMemory, a,
                                                m.bytes() + rel_offset,
                                                static_cast<uint32_t>(n), nullptr, 0, nullptr);
    }
    g_mapped_bytes_sent += n;
    // The host now holds these bytes, so the shadow can speak for them --
    // and only now. Whole pages only: a page sent in part is still
    // unknown in the rest, and claiming it would drop a later write to
    // the part that was never sent.
    if (m.shadow.size() == m.length() && !m.shadow_has.empty()) {
        std::memcpy(m.shadow.data() + rel_offset, m.bytes() + rel_offset,
                    static_cast<size_t>(n));
        const uint64_t ps = static_cast<uint64_t>(::sysconf(_SC_PAGESIZE));
        const uint64_t first_whole = (rel_offset + ps - 1) / ps;
        const uint64_t last_whole = (rel_offset + n) / ps;
        for (uint64_t p = first_whole; p < last_whole && p < m.shadow_has.size(); ++p) {
            m.shadow_has[p] = 1;
        }
    }
}

void push_mapped_bytes(uint64_t memory, uint64_t rel_offset, uint64_t size) {
    std::lock_guard<std::recursive_mutex> lock(mapped_mutex());
    auto it = mapped_ranges().find(memory);
    if (it == mapped_ranges().end()) return;
    MappedRange& m = it->second;
    const std::size_t mapped_len = m.length();
    if (rel_offset >= mapped_len) return;
    uint64_t n = size;
    if (n == VK_WHOLE_SIZE || rel_offset + n > mapped_len) n = mapped_len - rel_offset;
    if (n == 0) return;
    // Counted before the clean-skip below, so the stats keep answering
    // "how much did the engine ask us to consider" rather than silently
    // shrinking to flatter the ratio.
    g_mapped_bytes_asked += n;

    // STUD_VK_FULL_FLUSH=1 sends every byte every time, as this did
    // before any dirty tracking existed. Kept as a one-relaunch A/B for
    // any "geometry went missing" report: if the symptom survives it,
    // the tracking is not the cause.
    static const bool full_flush = std::getenv("STUD_VK_FULL_FLUSH") != nullptr;
    if (full_flush) {
        if (m.barrier.valid()) m.barrier.mark_clean_and_protect(rel_offset, n);
        send_mapped_run(memory, m, rel_offset, n);
        return;
    }

    // Nothing written since the last flush: there is nothing to send and
    // nothing to re-arm, so skip the allocation without walking its
    // pages. This is the common case, the engine keeps large heaps
    // mapped and rewrites a small part of one per frame, and a submit
    // used to scan every page of every one of them regardless.
    if (m.barrier.valid() && m.barrier.clean()) return;

    // The MMU already knows what changed: send the pages it recorded,
    // clipped to the range asked for, then re-arm.
    if (m.barrier.valid()) {
        const uint64_t end = rel_offset + n;
        // Snapshot, re-arm, then send. Sending first leaves a window in
        // which a write lands after its page was read and before it was
        // protected again, and that write would never be reported.
        // A sequential run unprotects up to kMaxFaultWindowPages ahead of
        // the page that faulted and marks them all dirty, because a fault
        // per 4KB costs far more than sending a few pages that turn out
        // not to have changed. Most of them are about to be written --
        // but not all, and the rest were being sent regardless: measured
        // in a real game at 24 MB per submit, about ten times the pages
        // that actually faulted.
        //
        // So the ones that were merely opened are checked against what
        // the host already holds, and skipped when they match. The check
        // runs only on those pages, never on the whole mapping, so it
        // cannot bring back the full compare the barrier replaced.
        const uint64_t ps = static_cast<uint64_t>(::sysconf(_SC_PAGESIZE));
        const uint64_t first_page = rel_offset / ps;
        const uint64_t last_page = (end + ps - 1) / ps;
        std::vector<bool> opened_ahead(last_page - first_page, false);
        for (uint64_t p = first_page; p < last_page; ++p) {
            opened_ahead[p - first_page] = m.barrier.page_is_speculative(static_cast<size_t>(p));
        }
        const std::size_t len = m.length();
        if (m.shadow.size() != len) {
            // Nothing is known about the host's contents yet, and it must
            // not be guessed at: every page starts untrusted.
            m.shadow.assign(len, 0);
            m.shadow_has.assign((len + ps - 1) / ps, 0);
        }
        const auto runs = m.barrier.take_dirty_runs_and_protect(rel_offset, n);
        for (const auto& run : runs) {
            uint64_t run_start = run.first;
            uint64_t run_end = run.first + run.second;
            if (run_end <= rel_offset || run_start >= end) continue;
            if (run_start < rel_offset) run_start = rel_offset;
            if (run_end > end) run_end = end;
            if (run_end <= run_start) continue;
            uint64_t batch = run_start;
            bool sending = false;
            for (uint64_t off = run_start; off < run_end;) {
                const uint64_t page = off / ps;
                const uint64_t page_end = std::min<uint64_t>((page + 1) * ps, run_end);
                bool send_this = true;
                const bool trusted = page < m.shadow_has.size() && m.shadow_has[page] != 0;
                if (trusted && page >= first_page && page < last_page &&
                    opened_ahead[page - first_page]) {
                    send_this = std::memcmp(m.bytes() + off, m.shadow.data() + off,
                                            static_cast<size_t>(page_end - off)) != 0;
                }
                if (send_this && !sending) {
                    batch = off;
                    sending = true;
                } else if (!send_this && sending) {
                    send_mapped_run(memory, m, batch, off - batch);
                    sending = false;
                }
                off = page_end;
            }
            if (sending) send_mapped_run(memory, m, batch, run_end - batch);
        }
        return;
    }

    // Fallback path, used only when the barrier could not be installed
    // or allocated: compare against a shadow of what the host was last
    // told. Correct but O(all mapped memory) per submit, which is why it
    // is not the default; see mapped_write_barrier.h.
    if (m.shadow.size() != m.staging.size()) {
        m.shadow.assign(m.staging.begin(), m.staging.end());
        send_mapped_run(memory, m, rel_offset, n);
        return;
    }
    // Named apart from the file-scope kChunk (64), which is a template
    // expansion width and has nothing to do with this byte count.
    constexpr uint64_t kCompareChunk = 4096;
    const uint8_t* cur = m.staging.data();
    uint8_t* shadow = m.shadow.data();
    const uint64_t end = rel_offset + n;
    uint64_t run_start = 0;
    bool in_run = false;
    for (uint64_t off = rel_offset; off < end; off += kCompareChunk) {
        const uint64_t len = std::min<uint64_t>(kCompareChunk, end - off);
        const bool dirty = std::memcmp(cur + off, shadow + off, len) != 0;
        if (dirty && !in_run) {
            run_start = off;
            in_run = true;
        } else if (!dirty && in_run) {
            std::memcpy(shadow + run_start, cur + run_start, off - run_start);
            send_mapped_run(memory, m, run_start, off - run_start);
            in_run = false;
        }
    }
    if (in_run) {
        std::memcpy(shadow + run_start, cur + run_start, end - run_start);
        send_mapped_run(memory, m, run_start, end - run_start);
    }
}

// Shared shape for a create that carries a payload: send the encoded
// struct, get one handle back.
template <typename H, CallId kId>
VkResult create_from_payload(const uint64_t (&a)[8], const std::vector<uint8_t>& in,
                             H* outHandle) {
    if (outHandle == nullptr) return VK_ERROR_INITIALIZATION_FAILED;
    uint64_t handle = 0;
    uint32_t written = 0;
    uint64_t r = stud::render_client::connection().call(kId, a, in.data(),
                                                         static_cast<uint32_t>(in.size()), &handle,
                                                         sizeof(handle), &written);
    VkResult result = static_cast<VkResult>(static_cast<int32_t>(r));
    if (result != VK_SUCCESS || written < sizeof(handle)) {
        return result != VK_SUCCESS ? result : VK_ERROR_INITIALIZATION_FAILED;
    }
    *outHandle = from_u64<H>(handle);
    return VK_SUCCESS;
}

// A descriptor-update template's payload is a bare pointer with no
// length; the size is implied by the entries the template was created
// with, so it is computed once there and remembered here.
std::map<uint64_t, uint64_t>& template_blob_size() {
    static std::map<uint64_t, uint64_t> m;
    return m;
}

}  // namespace

namespace {
// A host-visible allocation backed by a file both processes map.
//
// The engine writes vertices, uniforms and staging data through the
// pointer vkMapMemory hands back. Stud used to give it a private buffer
// and copy whatever changed to the host on every submit, measured at
// 197 MB/s through the socket in a real game, which was most of what the
// render thread was doing. Backing the allocation with a file the host
// imports as device memory (VK_EXT_external_memory_host) makes those
// writes land in the device's memory directly, and the copy stops
// existing. The file lives in the runtime directory, a tmpfs, so these
// are ordinary anonymous pages; nothing reaches a disk.
struct SharedAllocation {
    void* address = nullptr;
    size_t length = 0;
    uint64_t id = 0;
};
std::map<uint64_t, SharedAllocation>& shared_allocations() {
    static std::map<uint64_t, SharedAllocation> m;
    return m;
}

// Which memory types the device reports as host visible. Only those are
// ever mapped, so only those are worth sharing.
std::set<uint32_t>& host_visible_memory_types() {
    static std::set<uint32_t> t;
    return t;
}

// Host-visible types that are NOT coherent, and the allocations made
// from them.
//
// Coherent memory is the only kind that needs flushing behind the
// engine's back: the spec says a host write to it is visible to the
// device immediately, which across a process boundary is never true
// unless Stud ships it. NON-coherent memory carries the opposite
// contract -- the engine must call vkFlushMappedMemoryRanges itself,
// and Stud implements that call -- so scanning those mappings at every
// submit is work with no one waiting for it.
//
// It is not free work. Under uffd-scan each mapping costs a
// PAGEMAP_SCAN ioctl with PM_SCAN_WP_MATCHING, which rewrites page
// table protection bits and therefore shoots down TLBs on every core.
// flush_all_mapped_memory() runs at every submit over every live
// mapping, and a real session held 38-47 of them at once.
//
// Deliberately conservative: a type is only listed once the memory
// properties have actually been read, so anything unknown keeps the old
// behaviour and is still scanned.
std::set<uint32_t>& non_coherent_memory_types() {
    static std::set<uint32_t> t;
    return t;
}

std::set<uint64_t>& non_coherent_allocations() {
    static std::set<uint64_t> a;
    return a;
}

bool shared_memory_enabled() {
    // STUD_NO_SHARED_MEMORY=1 returns to copying, which is what this has
    // to be measured against.
    static const bool on = std::getenv("STUD_NO_SHARED_MEMORY") == nullptr;
    return on;
}

// Drops the NAME, keeping the pages. Both processes hold a mapping by
// then, and a mapping keeps the pages alive with no directory entry,
// the ordinary POSIX shared-memory idiom.
//
// Unlinking here rather than at vkFreeMemory is what makes these
// impossible to leak. The engine does not free every allocation before
// the process ends, and a process that crashes or is killed frees none of
// them, so the files accumulated in $XDG_RUNTIME_DIR/stud with nothing
// ever removing them, measured at 129 files and 3.1GB, which had filled
// that tmpfs to 100% and takes the Wayland socket, D-Bus and the rest of
// the session down with it.
void unlink_shared_allocation_name(uint64_t id) {
    if (id != 0) ::unlink(stud::render_host::shared_memory_path(id).c_str());
}

void release_shared_allocation(SharedAllocation& a) {
    if (a.address != nullptr) ::munmap(a.address, a.length);
    unlink_shared_allocation_name(a.id);
    a = SharedAllocation{};
}

bool create_shared_allocation(uint64_t size, SharedAllocation& out) {
    static std::atomic<uint64_t> next_id{1};
    const uint64_t id = next_id.fetch_add(1, std::memory_order_relaxed);
    const std::string path = stud::render_host::shared_memory_path(id);
    const int fd = ::open(path.c_str(), O_RDWR | O_CREAT | O_EXCL | O_CLOEXEC, 0600);
    if (fd < 0) return false;
    // Whole pages: the import on the other side needs a page-aligned
    // mapping of a whole number of pages.
    const size_t page = static_cast<size_t>(::sysconf(_SC_PAGESIZE));
    const size_t length = (static_cast<size_t>(size) + page - 1) & ~(page - 1);
    // Reserve the pages now, rather than letting them be filled in on
    // first touch.
    //
    // These files live in $XDG_RUNTIME_DIR, which is a tmpfs, usually
    // sized at a tenth of RAM. ftruncate alone leaves the file sparse:
    // it succeeds, mmap succeeds, and the pages are only allocated when
    // something touches them. If the tmpfs is full at that moment the
    // kernel answers the touch with SIGBUS, and the fault lands on
    // whoever was reading or writing the mapping rather than here.
    //
    // Live-caught on another machine: the render host died with
    // "CRASH in stud-render-host, signal 07" inside
    // vk_write_shared_mapped_memory's memcpy, and the log immediately
    // after was full of the desktop's own "No space left on device" for
    // that same tmpfs.
    //
    // posix_fallocate allocates the pages here, where ENOSPC is an
    // ordinary error: returning false means the caller simply does not
    // share this allocation and ships the bytes down the socket as it
    // did before sharing existed. Slower, and correct, which a crash is
    // not. It returns the error directly and does not set errno.
    const int reserved = ::posix_fallocate(fd, 0, static_cast<off_t>(length));
    if (reserved != 0) {
        if (reserved == ENOSPC) {
            static bool said = false;
            if (!said) {
                said = true;
                std::printf("stud: no room in %s for a %zu KB shared mapping; the engine's "
                            "writes will be copied instead of shared\n",
                            path.c_str(), length / 1024);
                std::fflush(stdout);
            }
        }
        ::close(fd);
        ::unlink(path.c_str());
        return false;
    }
    void* p = ::mmap(nullptr, length, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    ::close(fd);
    if (p == MAP_FAILED) {
        ::unlink(path.c_str());
        return false;
    }
    out.address = p;
    out.length = length;
    out.id = id;
    return true;
}

}  // namespace

extern "C" {

// Defined below, next to the format-capability query it depends on.
bool emulating(VkFormat format);
// Defined below, next to the decode scratch it works on.
void run_pending_decodes(const std::vector<VkCommandBuffer>& submitted);
// Brings back anything the GPU wrote into memory the host could not
// share with this process. Cheap and silent when there is nothing to
// bring back, which is every session where the import works.
void fetch_pending_readbacks();
// Moves the readbacks recorded against these command buffers to
// awaiting, now that a submit will actually run them.
void arm_pending_readbacks(const std::vector<VkCommandBuffer>& submitted);
// Defined below, next to the emulation decision it belongs to; needed at
// device creation, which comes first in this file.
bool driver_supports_format(VkPhysicalDevice physicalDevice, VkFormat format);
// Defined below, with the submit path; declared here because the calls
// whose ordering depends on it come first in this file.
bool sync_submit();
// Defined with the deferred queue further down; declared here because the
// entry points that have to wait for it come first in this file.


// Forwarded to Process C, where the real driver lives.
VKAPI_ATTR VkResult VKAPI_CALL stud_vkEnumerateInstanceVersion(uint32_t* pApiVersion) {
    if (pApiVersion == nullptr) return VK_ERROR_INITIALIZATION_FAILED;
    uint64_t a[8] = {};
    uint32_t version = 0;
    uint32_t written = 0;
    uint64_t r = stud::render_client::connection().call(
        CallId::VkEnumerateInstanceVersion, a, nullptr, 0, &version, sizeof(version), &written);
    if (written < sizeof(version)) return VK_ERROR_INITIALIZATION_FAILED;
    *pApiVersion = version;
    return static_cast<VkResult>(static_cast<int32_t>(r));
}

VKAPI_ATTR VkResult VKAPI_CALL stud_vkEnumerateInstanceExtensionProperties(
    const char* pLayerName, uint32_t* pPropertyCount, VkExtensionProperties* pProperties) {
    return enumerate_properties<vk_wire::ExtensionProperties, VkExtensionProperties,
                                 CallId::VkEnumerateInstanceExtensionProperties>(
        pLayerName, pPropertyCount, pProperties,
        [](const vk_wire::ExtensionProperties& w, VkExtensionProperties& v) {
            std::snprintf(v.extensionName, sizeof(v.extensionName), "%s", w.extension_name);
            v.specVersion = w.spec_version;
        });
}

VKAPI_ATTR VkResult VKAPI_CALL stud_vkEnumerateInstanceLayerProperties(
    uint32_t* pPropertyCount, VkLayerProperties* pProperties) {
    return enumerate_properties<vk_wire::LayerProperties, VkLayerProperties,
                                 CallId::VkEnumerateInstanceLayerProperties>(
        nullptr, pPropertyCount, pProperties,
        [](const vk_wire::LayerProperties& w, VkLayerProperties& v) {
            std::snprintf(v.layerName, sizeof(v.layerName), "%s", w.layer_name);
            v.specVersion = w.spec_version;
            v.implementationVersion = w.implementation_version;
            std::snprintf(v.description, sizeof(v.description), "%s", w.description);
        });
}

VKAPI_ATTR VkResult VKAPI_CALL stud_vkCreateInstance(const VkInstanceCreateInfo* pCreateInfo,
                                                      const VkAllocationCallbacks* /*pAllocator*/,
                                                      VkInstance* pInstance) {
    if (pCreateInfo == nullptr || pInstance == nullptr) return VK_ERROR_INITIALIZATION_FAILED;

    // Flatten the pointer graph; see vulkan_forward.h for why none of
    // it can travel as-is.
    vk_wire::CreateInstanceHeader hdr{};
    hdr.flags = pCreateInfo->flags;
    std::string app_name, engine_name;
    if (pCreateInfo->pApplicationInfo != nullptr) {
        const VkApplicationInfo* ai = pCreateInfo->pApplicationInfo;
        hdr.has_application_info = 1;
        hdr.application_version = ai->applicationVersion;
        hdr.engine_version = ai->engineVersion;
        hdr.api_version = ai->apiVersion;
        if (ai->pApplicationName != nullptr) app_name = ai->pApplicationName;
        if (ai->pEngineName != nullptr) engine_name = ai->pEngineName;
    }
    hdr.application_name_len = static_cast<uint32_t>(app_name.size());
    hdr.engine_name_len = static_cast<uint32_t>(engine_name.size());
    hdr.enabled_layer_count = pCreateInfo->enabledLayerCount;
    hdr.enabled_extension_count = pCreateInfo->enabledExtensionCount;

    std::vector<uint8_t> in(sizeof(hdr));
    std::memcpy(in.data(), &hdr, sizeof(hdr));
    auto append = [&in](const char* p, size_t n) {
        in.insert(in.end(), reinterpret_cast<const uint8_t*>(p),
                  reinterpret_cast<const uint8_t*>(p) + n);
    };
    append(app_name.data(), app_name.size());
    append(engine_name.data(), engine_name.size());
    for (uint32_t i = 0; i < pCreateInfo->enabledLayerCount; ++i) {
        const char* n = pCreateInfo->ppEnabledLayerNames[i];
        append(n, std::strlen(n) + 1);
    }
    for (uint32_t i = 0; i < pCreateInfo->enabledExtensionCount; ++i) {
        const char* n = pCreateInfo->ppEnabledExtensionNames[i];
        append(n, std::strlen(n) + 1);
    }

    uint64_t a[8] = {};
    uint64_t handle = 0;
    uint32_t written = 0;
    uint64_t r = stud::render_client::connection().call(
        CallId::VkCreateInstance, a, in.data(), static_cast<uint32_t>(in.size()), &handle,
        sizeof(handle), &written);
    VkResult result = static_cast<VkResult>(static_cast<int32_t>(r));
    if (result != VK_SUCCESS || written < sizeof(handle)) {
        return result != VK_SUCCESS ? result : VK_ERROR_INITIALIZATION_FAILED;
    }
    // The handle is the host's own real VkInstance, passed straight
    // through as an opaque token. Process B never dereferences it, it
    // only ever hands it back on a later call, same as a GLsync.
    *pInstance = reinterpret_cast<VkInstance>(static_cast<uintptr_t>(handle));
    return VK_SUCCESS;
}

// ---- Physical-device level ------------------------------------------
//
// The handles are the host's own real ones, opaque here: Process B never
// dereferences a VkPhysicalDevice, it only hands it back on a later call.

VKAPI_ATTR VkResult VKAPI_CALL stud_vkEnumeratePhysicalDevices(VkInstance /*instance*/,
                                                                uint32_t* pPhysicalDeviceCount,
                                                                VkPhysicalDevice* pPhysicalDevices) {
    if (pPhysicalDeviceCount == nullptr) return VK_ERROR_INITIALIZATION_FAILED;
    const uint32_t capacity = pPhysicalDevices != nullptr ? *pPhysicalDeviceCount : 0;
    uint64_t a[8] = {capacity};
    std::vector<uint8_t> out(sizeof(uint32_t) + sizeof(uint64_t) * (capacity + 1));
    uint32_t written = 0;
    uint64_t r = stud::render_client::connection().call(CallId::VkEnumeratePhysicalDevices, a,
                                                         nullptr, 0, out.data(),
                                                         static_cast<uint32_t>(out.size()),
                                                         &written);
    if (written < sizeof(uint32_t)) return VK_ERROR_INITIALIZATION_FAILED;
    uint32_t count = 0;
    std::memcpy(&count, out.data(), sizeof(count));
    if (pPhysicalDevices == nullptr) {
        *pPhysicalDeviceCount = count;
        return static_cast<VkResult>(static_cast<int32_t>(r));
    }
    const uint32_t have = (written - sizeof(uint32_t)) / sizeof(uint64_t);
    const uint32_t n = count < have ? count : have;
    for (uint32_t i = 0; i < n; ++i) {
        uint64_t h = 0;
        std::memcpy(&h, out.data() + sizeof(uint32_t) + i * sizeof(h), sizeof(h));
        pPhysicalDevices[i] = reinterpret_cast<VkPhysicalDevice>(static_cast<uintptr_t>(h));
    }
    *pPhysicalDeviceCount = n;
    return static_cast<VkResult>(static_cast<int32_t>(r));
}

VKAPI_ATTR void VKAPI_CALL stud_vkGetPhysicalDeviceProperties(
    VkPhysicalDevice physicalDevice, VkPhysicalDeviceProperties* pProperties) {
    if (pProperties == nullptr) return;
    uint64_t a[8] = {static_cast<uint64_t>(reinterpret_cast<uintptr_t>(physicalDevice))};
    std::vector<uint8_t> out(sizeof(uint32_t) + sizeof(VkPhysicalDeviceProperties));
    uint32_t written = 0;
    stud::render_client::connection().call(CallId::VkGetPhysicalDeviceProperties, a, nullptr, 0,
                                            out.data(), static_cast<uint32_t>(out.size()),
                                            &written);
    read_pod(out.data(), written, *pProperties, "VkPhysicalDeviceProperties");
}

VKAPI_ATTR void VKAPI_CALL stud_vkGetPhysicalDeviceFeatures(VkPhysicalDevice physicalDevice,
                                                             VkPhysicalDeviceFeatures* pFeatures) {
    if (pFeatures == nullptr) return;
    uint64_t a[8] = {static_cast<uint64_t>(reinterpret_cast<uintptr_t>(physicalDevice))};
    std::vector<uint8_t> out(sizeof(uint32_t) + sizeof(VkPhysicalDeviceFeatures));
    uint32_t written = 0;
    stud::render_client::connection().call(CallId::VkGetPhysicalDeviceFeatures, a, nullptr, 0,
                                            out.data(), static_cast<uint32_t>(out.size()),
                                            &written);
    read_pod(out.data(), written, *pFeatures, "VkPhysicalDeviceFeatures");
    // Stud decodes ETC/EAC itself when the device cannot, so the feature
    // really is available to the engine either way. If the device has it
    // natively this changes nothing, the bit is already set, and the
    // hardware keeps the work. See texture_decode.h.
    if (emulating(VK_FORMAT_ETC2_R8G8B8A8_UNORM_BLOCK)) {
        pFeatures->textureCompressionETC2 = VK_TRUE;
    }
}

VKAPI_ATTR void VKAPI_CALL stud_vkGetPhysicalDeviceMemoryProperties(
    VkPhysicalDevice physicalDevice, VkPhysicalDeviceMemoryProperties* pMemoryProperties) {
    if (pMemoryProperties == nullptr) return;
    uint64_t a[8] = {static_cast<uint64_t>(reinterpret_cast<uintptr_t>(physicalDevice))};
    std::vector<uint8_t> out(sizeof(uint32_t) + sizeof(VkPhysicalDeviceMemoryProperties));
    uint32_t written = 0;
    stud::render_client::connection().call(CallId::VkGetPhysicalDeviceMemoryProperties, a, nullptr,
                                            0, out.data(), static_cast<uint32_t>(out.size()),
                                            &written);
    read_pod(out.data(), written, *pMemoryProperties, "VkPhysicalDeviceMemoryProperties");
    for (uint32_t i = 0; i < pMemoryProperties->memoryTypeCount; ++i) {
        const VkMemoryPropertyFlags f = pMemoryProperties->memoryTypes[i].propertyFlags;
        if ((f & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT) != 0) {
            host_visible_memory_types().insert(i);
            if ((f & VK_MEMORY_PROPERTY_HOST_COHERENT_BIT) == 0) {
                non_coherent_memory_types().insert(i);
            }
        }
    }
}

VKAPI_ATTR void VKAPI_CALL stud_vkGetPhysicalDeviceQueueFamilyProperties(
    VkPhysicalDevice physicalDevice, uint32_t* pQueueFamilyPropertyCount,
    VkQueueFamilyProperties* pQueueFamilyProperties) {
    if (pQueueFamilyPropertyCount == nullptr) return;
    const uint32_t capacity = pQueueFamilyProperties != nullptr ? *pQueueFamilyPropertyCount : 0;
    uint64_t a[8] = {static_cast<uint64_t>(reinterpret_cast<uintptr_t>(physicalDevice)), capacity};
    std::vector<uint8_t> out(sizeof(uint32_t) * 2 +
                             sizeof(VkQueueFamilyProperties) * (capacity + 1));
    uint32_t written = 0;
    stud::render_client::connection().call(CallId::VkGetPhysicalDeviceQueueFamilyProperties, a,
                                            nullptr, 0, out.data(),
                                            static_cast<uint32_t>(out.size()), &written);
    if (written < sizeof(uint32_t) * 2) return;
    uint32_t count = 0, stride = 0;
    std::memcpy(&count, out.data(), sizeof(count));
    std::memcpy(&stride, out.data() + sizeof(uint32_t), sizeof(stride));
    if (pQueueFamilyProperties == nullptr) {
        *pQueueFamilyPropertyCount = count;
        return;
    }
    if (stride != sizeof(VkQueueFamilyProperties)) {
        std::fprintf(stderr,
                     "stud: vulkan-client: VkQueueFamilyProperties stride mismatch "
                     "(host %u, client %zu)\n",
                     stride, sizeof(VkQueueFamilyProperties));
        std::fflush(stderr);
        *pQueueFamilyPropertyCount = 0;
        return;
    }
    const uint32_t have = (written - sizeof(uint32_t) * 2) / stride;
    const uint32_t n = count < have ? count : have;
    for (uint32_t i = 0; i < n; ++i) {
        std::memcpy(&pQueueFamilyProperties[i], out.data() + sizeof(uint32_t) * 2 + i * stride,
                    stride);
    }
    *pQueueFamilyPropertyCount = n;
}

VKAPI_ATTR VkResult VKAPI_CALL stud_vkEnumerateDeviceExtensionProperties(
    VkPhysicalDevice physicalDevice, const char* pLayerName, uint32_t* pPropertyCount,
    VkExtensionProperties* pProperties) {
    if (pPropertyCount == nullptr) return VK_ERROR_INITIALIZATION_FAILED;
    const uint32_t capacity = pProperties != nullptr ? *pPropertyCount : 0;
    uint64_t a[8] = {static_cast<uint64_t>(reinterpret_cast<uintptr_t>(physicalDevice)), capacity};
    std::string filter = pLayerName != nullptr ? pLayerName : "";
    std::vector<uint8_t> out(sizeof(uint32_t) +
                             sizeof(vk_wire::ExtensionProperties) * (capacity + 1));
    uint32_t written = 0;
    uint64_t r = stud::render_client::connection().call(
        CallId::VkEnumerateDeviceExtensionProperties, a, filter.c_str(),
        static_cast<uint32_t>(filter.size() + 1), out.data(), static_cast<uint32_t>(out.size()),
        &written);
    if (written < sizeof(uint32_t)) return VK_ERROR_INITIALIZATION_FAILED;
    uint32_t count = 0;
    std::memcpy(&count, out.data(), sizeof(count));
    // VK_IMG_format_pvrtc is added to whatever the device really has:
    // Stud decodes PVRTC itself (see texture_decode.h), and the PVRTC
    // formats are only usable by an application that finds this
    // extension. Only added when the device does not already provide it,
    // so it is never a duplicate and a device with real PVRTC keeps its
    // own.
    const bool advertise_pvrtc = emulating(VK_FORMAT_PVRTC1_4BPP_UNORM_BLOCK_IMG);
    if (advertise_pvrtc) ++count;
    if (pProperties == nullptr) {
        *pPropertyCount = count;
        return static_cast<VkResult>(static_cast<int32_t>(r));
    }
    const uint32_t have =
        (written - sizeof(uint32_t)) / sizeof(vk_wire::ExtensionProperties);
    const uint32_t n = count < have ? count : have;
    // What is really in pProperties when this returns. The PVRTC entry is
    // written AT index n, so reporting n alone said "there are n
    // extensions" while having written n + 1 -- the emulated extension was
    // handed over and then hidden, and the engine only uses PVRTC formats
    // if it finds this extension.
    uint32_t written_back = n;
    for (uint32_t i = 0; i < n; ++i) {
        vk_wire::ExtensionProperties w{};
        std::memcpy(&w, out.data() + sizeof(uint32_t) + i * sizeof(w), sizeof(w));
        std::snprintf(pProperties[i].extensionName, sizeof(pProperties[i].extensionName), "%s",
                      w.extension_name);
        pProperties[i].specVersion = w.spec_version;
    }
    if (advertise_pvrtc && n < count && n < capacity) {
        std::snprintf(pProperties[n].extensionName, sizeof(pProperties[n].extensionName), "%s",
                      VK_IMG_FORMAT_PVRTC_EXTENSION_NAME);
        pProperties[n].specVersion = VK_IMG_FORMAT_PVRTC_SPEC_VERSION;
        ++written_back;
    }
    // Deliberately NOT advertising VK_GOOGLE_display_timing, though Stud
    // implements it above and could. Measured: advertising it changes
    // nothing, because the engine resolves
    // vkGetRefreshCycleDurationGOOGLE and then never calls it, with the
    // extension offered, steady state stayed 52-60fps and the
    // implementation's own first-call print never fired once. Since the
    // other half of the extension (past presentation timings) has no real
    // history behind it, claiming support buys nothing and risks a
    // consumer that does use it. The implementation stays, so it is one
    // line away if that ever changes.
    *pPropertyCount = written_back;
    return static_cast<VkResult>(static_cast<int32_t>(r));
}

VKAPI_ATTR void VKAPI_CALL stud_vkGetPhysicalDeviceFeatures2(
    VkPhysicalDevice physicalDevice, VkPhysicalDeviceFeatures2* pFeatures) {
    if (pFeatures == nullptr) return;
    std::vector<uint8_t> in;
    const uint32_t nodes = flatten_chain(pFeatures->pNext, in);
    uint64_t a[8] = {static_cast<uint64_t>(reinterpret_cast<uintptr_t>(physicalDevice)), nodes};

    std::vector<uint8_t> out(sizeof(uint32_t) + sizeof(VkPhysicalDeviceFeatures) + in.size() +
                             sizeof(vk_wire::ChainNodeHeader) * (nodes + 1));
    uint32_t written = 0;
    stud::render_client::connection().call(CallId::VkGetPhysicalDeviceFeatures2, a,
                                            in.empty() ? nullptr : in.data(),
                                            static_cast<uint32_t>(in.size()), out.data(),
                                            static_cast<uint32_t>(out.size()), &written);
    if (!read_pod(out.data(), written, pFeatures->features, "VkPhysicalDeviceFeatures2")) return;
    if (emulating(VK_FORMAT_ETC2_R8G8B8A8_UNORM_BLOCK)) {
        pFeatures->features.textureCompressionETC2 = VK_TRUE;
    }
    const size_t consumed = sizeof(uint32_t) + sizeof(VkPhysicalDeviceFeatures);
    if (written > consumed) {
        scatter_chain(pFeatures->pNext, out.data() + consumed, written - consumed);
    }
}

VKAPI_ATTR VkResult VKAPI_CALL stud_vkCreateDevice(VkPhysicalDevice physicalDevice,
                                                    const VkDeviceCreateInfo* pCreateInfo,
                                                    const VkAllocationCallbacks* /*pAllocator*/,
                                                    VkDevice* pDevice) {
    if (pCreateInfo == nullptr || pDevice == nullptr) return VK_ERROR_INITIALIZATION_FAILED;

    std::vector<uint8_t> body;
    auto append = [&body](const void* p, size_t n) {
        const size_t at = body.size();
        body.resize(at + n);
        std::memcpy(body.data() + at, p, n);
    };

    for (uint32_t i = 0; i < pCreateInfo->queueCreateInfoCount; ++i) {
        const VkDeviceQueueCreateInfo& q = pCreateInfo->pQueueCreateInfos[i];
        vk_wire::DeviceQueueCreateHeader h{q.flags, q.queueFamilyIndex, q.queueCount};
        append(&h, sizeof(h));
        if (q.queueCount > 0 && q.pQueuePriorities != nullptr) {
            append(q.pQueuePriorities, sizeof(float) * q.queueCount);
        }
    }
    for (uint32_t i = 0; i < pCreateInfo->enabledLayerCount; ++i) {
        const char* n = pCreateInfo->ppEnabledLayerNames[i];
        append(n, std::strlen(n) + 1);
    }
    // Same reasoning as the ETC2 feature bit below: the engine was told
    // PVRTC is available and will enable the extension, but no desktop
    // driver has it and would fail the whole device creation over it.
    // Stud provides those formats in software, so the request is dropped
    // here instead of forwarded.
    uint32_t forwarded_extensions = 0;
    for (uint32_t i = 0; i < pCreateInfo->enabledExtensionCount; ++i) {
        const char* n = pCreateInfo->ppEnabledExtensionNames[i];
        if (std::strcmp(n, VK_IMG_FORMAT_PVRTC_EXTENSION_NAME) == 0 &&
            emulating(VK_FORMAT_PVRTC1_4BPP_UNORM_BLOCK_IMG)) {
            continue;
        }
        append(n, std::strlen(n) + 1);
        ++forwarded_extensions;
    }
    if (pCreateInfo->pEnabledFeatures != nullptr) {
        // The engine was told ETC2 is available and will duly ask for it,
        // but the device underneath may genuinely not have it and would
        // reject the whole vkCreateDevice call over one bit. Stud provides
        // that feature in software, so it is dropped here rather than
        // forwarded. Every other requested feature goes through untouched.
        VkPhysicalDeviceFeatures requested = *pCreateInfo->pEnabledFeatures;
        if (emulating(VK_FORMAT_ETC2_R8G8B8A8_UNORM_BLOCK)) {
            requested.textureCompressionETC2 = VK_FALSE;
        }
        append(&requested, sizeof(requested));
    }
    std::vector<uint8_t> chain;
    const uint32_t nodes = flatten_chain(pCreateInfo->pNext, chain);
    append(chain.data(), chain.size());

    vk_wire::CreateDeviceHeader hdr{};
    hdr.flags = pCreateInfo->flags;
    hdr.queue_create_count = pCreateInfo->queueCreateInfoCount;
    hdr.enabled_layer_count = pCreateInfo->enabledLayerCount;
    hdr.enabled_extension_count = forwarded_extensions;
    hdr.has_enabled_features = pCreateInfo->pEnabledFeatures != nullptr ? 1 : 0;
    hdr.enabled_features_size = static_cast<uint32_t>(sizeof(VkPhysicalDeviceFeatures));
    hdr.chain_node_count = nodes;

    std::vector<uint8_t> in(sizeof(hdr));
    std::memcpy(in.data(), &hdr, sizeof(hdr));
    in.insert(in.end(), body.begin(), body.end());

    uint64_t a[8] = {static_cast<uint64_t>(reinterpret_cast<uintptr_t>(physicalDevice))};
    uint64_t handle = 0;
    uint32_t written = 0;
    uint64_t r = stud::render_client::connection().call(
        CallId::VkCreateDevice, a, in.data(), static_cast<uint32_t>(in.size()), &handle,
        sizeof(handle), &written);
    VkResult result = static_cast<VkResult>(static_cast<int32_t>(r));
    if (result != VK_SUCCESS || written < sizeof(handle)) {
        return result != VK_SUCCESS ? result : VK_ERROR_INITIALIZATION_FAILED;
    }
    *pDevice = reinterpret_cast<VkDevice>(static_cast<uintptr_t>(handle));
    g_decode_device = *pDevice;
    g_decode_physical_device = physicalDevice;
    // Emulated textures are stored uncompressed unless STUD_TEX_BC=1 asks
    // for them to be re-encoded to BC.
    //
    // BC was the default, for the reason texture_encode.h gives: an
    // uncompressed substitute is 4-8x the size the engine budgets for, and
    // measured on 2026-09-23 textures flicker between mips without it. It
    // also costs about a quarter of the engine process's CPU during play,
    // because every newly streamed texture is encoded. The project's
    // direction is to make the uncompressed path hold its textures and
    // retire the re-encode, so uncompressed is the default and BC is the
    // opt-in comparison.
    {
        static const bool no_bc = std::getenv("STUD_TEX_BC") == nullptr;
        const VkFormat needed[] = {
            VK_FORMAT_BC1_RGB_UNORM_BLOCK,  VK_FORMAT_BC1_RGB_SRGB_BLOCK,
            VK_FORMAT_BC1_RGBA_UNORM_BLOCK, VK_FORMAT_BC1_RGBA_SRGB_BLOCK,
            VK_FORMAT_BC3_UNORM_BLOCK,      VK_FORMAT_BC3_SRGB_BLOCK,
            VK_FORMAT_BC4_UNORM_BLOCK,      VK_FORMAT_BC5_UNORM_BLOCK,
        };
        bool all = !no_bc;
        for (VkFormat f : needed) {
            if (!all) break;
            all = driver_supports_format(physicalDevice, f);
        }
        stud::texture_decode::set_bc_available(all);
        // BC7 for the colour formats, where the device has it. Parity for
        // ETC2 RGBA8, twice the size for ETC2 RGB8, and worth it: BC1
        // holds four colours per block, which is what turned normal maps
        // into blocky noise. STUD_TEX_NO_BC7=1 goes back to BC1/BC3.
        static const bool no_bc7 = std::getenv("STUD_TEX_NO_BC7") != nullptr;
        const bool bc7 = all && !no_bc7 &&
                         driver_supports_format(physicalDevice, VK_FORMAT_BC7_UNORM_BLOCK) &&
                         driver_supports_format(physicalDevice, VK_FORMAT_BC7_SRGB_BLOCK);
        stud::texture_decode::set_bc7_available(bc7);
        std::printf("stud: vulkan-client: emulated textures stored as %s\n",
                    !all ? "uncompressed"
                         : (bc7 ? "BC7 for colour, BC4/BC5 for the EAC formats"
                                : "BC1/BC3 for colour, BC4/BC5 for the EAC formats"));
        std::fflush(stdout);
    }
    return VK_SUCCESS;
}

// What the real driver says about a format, with none of Stud's own
// answers layered on top.
VkFormatProperties real_format_properties(VkPhysicalDevice physicalDevice, VkFormat format) {
    VkFormatProperties props{};
    uint64_t a[8] = {static_cast<uint64_t>(reinterpret_cast<uintptr_t>(physicalDevice)),
                     static_cast<uint64_t>(format)};
    std::vector<uint8_t> out(sizeof(uint32_t) + sizeof(VkFormatProperties));
    uint32_t written = 0;
    stud::render_client::connection().call(CallId::VkGetPhysicalDeviceFormatProperties, a, nullptr,
                                            0, out.data(), static_cast<uint32_t>(out.size()),
                                            &written);
    read_pod(out.data(), written, props, "VkFormatProperties");
    return props;
}

bool driver_supports_format(VkPhysicalDevice physicalDevice, VkFormat format) {
    if (physicalDevice == VK_NULL_HANDLE) return false;
    static std::map<uint32_t, bool> cache;
    std::lock_guard<std::recursive_mutex> lock(emulation_mutex());
    auto it = cache.find(static_cast<uint32_t>(format));
    if (it != cache.end()) return it->second;
    const VkFormatProperties props = real_format_properties(physicalDevice, format);
    const bool ok =
        (props.optimalTilingFeatures & VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT) != 0;
    cache.emplace(static_cast<uint32_t>(format), ok);
    return ok;
}

// True when Stud must decode this format itself: it is one it can decode,
// and the device cannot sample it. An Intel iGPU reports real ETC2 and so
// takes none of this path; NVIDIA reports none and takes all of it.
bool emulating(VkFormat format) {
    if (!stud::texture_decode::is_emulated(format)) return false;
    // STUD_TEX_FORCE_DECODE=1 decodes even where the driver could do it
    // itself. That is not useful in normal running, hardware sampling is
    // smaller and faster, but it is the only way to check this decoder
    // against a GPU that implements the same formats: run the same scene
    // on a device with real ETC2 with and without it, and any visible
    // difference is this code's fault rather than the format's.
    static const bool force = std::getenv("STUD_TEX_FORCE_DECODE") != nullptr;
    if (force) return true;
    // STUD_NO_TEX_DECODE=1 gives up the formats entirely, so the engine
    // falls back to whatever the device really has, BC/DXT on a desktop
    // GPU. That is the pre-decode behaviour, and it is worth being able to
    // return to on demand: a decoded texture is stored uncompressed, so it
    // costs several times the memory and sampling bandwidth of the
    // compressed one it replaced. Better-looking textures are not free,
    // and this is the switch that measures the difference.
    static const bool disabled = std::getenv("STUD_NO_TEX_DECODE") != nullptr;
    if (disabled) return false;
    return !driver_supports_format(g_decode_physical_device, format);
}

VKAPI_ATTR void VKAPI_CALL stud_vkGetPhysicalDeviceFormatProperties(
    VkPhysicalDevice physicalDevice, VkFormat format, VkFormatProperties* pFormatProperties) {
    if (pFormatProperties == nullptr) return;
    // The first thing anything asks about a device, and often before a
    // device exists, so it is also where the physical device Stud's own
    // capability questions are answered against gets picked up.
    if (g_decode_physical_device == VK_NULL_HANDLE) g_decode_physical_device = physicalDevice;
    *pFormatProperties = real_format_properties(physicalDevice, format);
    if (stud::texture_decode::is_emulated(format) &&
        !driver_supports_format(physicalDevice, format)) {
        // What the engine may do with these is exactly what it may do with
        // the uncompressed image they are really stored as: sample it and
        // copy into it. Deliberately not claiming storage, blit or atomic
        // use. Nothing decodes on those paths, so promising them would
        // be a lie the engine could act on.
        pFormatProperties->linearTilingFeatures = 0;
        pFormatProperties->bufferFeatures = 0;
        pFormatProperties->optimalTilingFeatures =
            VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT | VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT |
            VK_FORMAT_FEATURE_TRANSFER_SRC_BIT | VK_FORMAT_FEATURE_TRANSFER_DST_BIT;
    }
}

VKAPI_ATTR VkResult VKAPI_CALL stud_vkGetPhysicalDeviceImageFormatProperties(
    VkPhysicalDevice physicalDevice, VkFormat format, VkImageType type, VkImageTiling tiling,
    VkImageUsageFlags usage, VkImageCreateFlags flags,
    VkImageFormatProperties* pImageFormatProperties) {
    if (pImageFormatProperties == nullptr) return VK_ERROR_INITIALIZATION_FAILED;
    // An emulated format is stored as its substitute, so the limits that
    // apply are the substitute's. A format the device handles itself is
    // asked about as-is.
    if (emulating(format)) format = stud::texture_decode::substitute(format);
    uint64_t a[8] = {static_cast<uint64_t>(reinterpret_cast<uintptr_t>(physicalDevice)),
                     static_cast<uint64_t>(format),
                     static_cast<uint64_t>(type),
                     static_cast<uint64_t>(tiling),
                     static_cast<uint64_t>(usage),
                     static_cast<uint64_t>(flags)};
    std::vector<uint8_t> out(sizeof(uint32_t) + sizeof(VkImageFormatProperties));
    uint32_t written = 0;
    uint64_t r = stud::render_client::connection().call(
        CallId::VkGetPhysicalDeviceImageFormatProperties, a, nullptr, 0, out.data(),
        static_cast<uint32_t>(out.size()), &written);
    read_pod(out.data(), written, *pImageFormatProperties, "VkImageFormatProperties");
    return static_cast<VkResult>(static_cast<int32_t>(r));
}

VKAPI_ATTR void VKAPI_CALL stud_vkGetDeviceQueue(VkDevice device, uint32_t queueFamilyIndex,
                                                  uint32_t queueIndex, VkQueue* pQueue) {
    uint64_t a[8] = {to_u64(device), queueFamilyIndex, queueIndex};
    simple_create<VkQueue, CallId::VkGetDeviceQueue>(a, pQueue);
}

VKAPI_ATTR VkResult VKAPI_CALL stud_vkCreateCommandPool(VkDevice device,
                                                         const VkCommandPoolCreateInfo* pCreateInfo,
                                                         const VkAllocationCallbacks*,
                                                         VkCommandPool* pCommandPool) {
    if (pCreateInfo == nullptr) return VK_ERROR_INITIALIZATION_FAILED;
    uint64_t a[8] = {to_u64(device), pCreateInfo->flags, pCreateInfo->queueFamilyIndex};
    return simple_create<VkCommandPool, CallId::VkCreateCommandPool>(a, pCommandPool);
}

VKAPI_ATTR VkResult VKAPI_CALL stud_vkCreateSemaphore(VkDevice device,
                                                       const VkSemaphoreCreateInfo* pCreateInfo,
                                                       const VkAllocationCallbacks*,
                                                       VkSemaphore* pSemaphore) {
    if (pCreateInfo == nullptr) return VK_ERROR_INITIALIZATION_FAILED;
    uint64_t a[8] = {to_u64(device), pCreateInfo->flags};
    return simple_create<VkSemaphore, CallId::VkCreateSemaphore>(a, pSemaphore);
}

VKAPI_ATTR VkResult VKAPI_CALL stud_vkCreateFence(VkDevice device,
                                                   const VkFenceCreateInfo* pCreateInfo,
                                                   const VkAllocationCallbacks*, VkFence* pFence) {
    if (pCreateInfo == nullptr) return VK_ERROR_INITIALIZATION_FAILED;
    uint64_t a[8] = {to_u64(device), pCreateInfo->flags};
    return simple_create<VkFence, CallId::VkCreateFence>(a, pFence);
}

VKAPI_ATTR VkResult VKAPI_CALL stud_vkCreateQueryPool(VkDevice device,
                                                       const VkQueryPoolCreateInfo* pCreateInfo,
                                                       const VkAllocationCallbacks*,
                                                       VkQueryPool* pQueryPool) {
    if (pCreateInfo == nullptr) return VK_ERROR_INITIALIZATION_FAILED;
    uint64_t a[8] = {to_u64(device), pCreateInfo->flags,
                     static_cast<uint64_t>(pCreateInfo->queryType), pCreateInfo->queryCount,
                     pCreateInfo->pipelineStatistics};
    return simple_create<VkQueryPool, CallId::VkCreateQueryPool>(a, pQueryPool);
}

VKAPI_ATTR VkResult VKAPI_CALL stud_vkCreatePipelineCache(
    VkDevice device, const VkPipelineCacheCreateInfo* pCreateInfo, const VkAllocationCallbacks*,
    VkPipelineCache* pPipelineCache) {
    if (pCreateInfo == nullptr || pPipelineCache == nullptr) return VK_ERROR_INITIALIZATION_FAILED;
    uint64_t a[8] = {to_u64(device), pCreateInfo->flags};
    uint64_t handle = 0;
    uint32_t written = 0;
    uint64_t r = stud::render_client::connection().call(
        CallId::VkCreatePipelineCache, a, pCreateInfo->pInitialData,
        static_cast<uint32_t>(pCreateInfo->initialDataSize), &handle, sizeof(handle), &written);
    VkResult result = static_cast<VkResult>(static_cast<int32_t>(r));
    if (result != VK_SUCCESS || written < sizeof(handle)) {
        return result != VK_SUCCESS ? result : VK_ERROR_INITIALIZATION_FAILED;
    }
    *pPipelineCache = from_u64<VkPipelineCache>(handle);
    return VK_SUCCESS;
}

VKAPI_ATTR VkResult VKAPI_CALL stud_vkGetPipelineCacheData(VkDevice device,
                                                            VkPipelineCache pipelineCache,
                                                            size_t* pDataSize, void* pData) {
    if (pDataSize == nullptr) return VK_ERROR_INITIALIZATION_FAILED;
    const uint32_t capacity = pData != nullptr ? static_cast<uint32_t>(*pDataSize) : 0;
    uint64_t a[8] = {to_u64(device), to_u64(pipelineCache), capacity};
    std::vector<uint8_t> out(sizeof(uint64_t) + capacity);
    uint32_t written = 0;
    uint64_t r = stud::render_client::connection().call(CallId::VkGetPipelineCacheData, a, nullptr,
                                                         0, out.data(),
                                                         static_cast<uint32_t>(out.size()),
                                                         &written);
    if (written < sizeof(uint64_t)) return VK_ERROR_INITIALIZATION_FAILED;
    uint64_t size = 0;
    std::memcpy(&size, out.data(), sizeof(size));
    if (pData != nullptr && size > 0) {
        const uint64_t have = written - sizeof(uint64_t);
        const uint64_t n = size < have ? size : have;
        std::memcpy(pData, out.data() + sizeof(uint64_t), static_cast<size_t>(n));
        size = n;
    }
    *pDataSize = static_cast<size_t>(size);
    return static_cast<VkResult>(static_cast<int32_t>(r));
}

VKAPI_ATTR void VKAPI_CALL stud_vkDestroyPipelineCache(VkDevice device,
                                                        VkPipelineCache pipelineCache,
                                                        const VkAllocationCallbacks*) {
    uint64_t a[8] = {to_u64(device), to_u64(pipelineCache)};
    stud::render_client::connection().call(CallId::VkDestroyPipelineCache, a, nullptr, 0, nullptr,
                                            0, nullptr);
}

VKAPI_ATTR VkResult VKAPI_CALL stud_vkCreateImage(VkDevice device,
                                                   const VkImageCreateInfo* pCreateInfo,
                                                   const VkAllocationCallbacks*, VkImage* pImage) {
    if (pCreateInfo == nullptr || pImage == nullptr) return VK_ERROR_INITIALIZATION_FAILED;
    vk_wire::CreateImageHeader h{};
    h.flags = pCreateInfo->flags;
    h.image_type = static_cast<uint32_t>(pCreateInfo->imageType);
    // An ETC/PVRTC image becomes a real uncompressed one on the device.
    // Substituting here rather than anywhere later keeps everything
    // downstream honest by construction: the memory requirements the
    // engine reads back, the layout transitions, the descriptor writes
    // are all the substitute's, because that is what actually exists.
    const VkFormat requested_format = pCreateInfo->format;
    const bool emulated = emulating(requested_format);
    if (std::getenv("STUD_TEX_DECODE_TRACE") != nullptr) {
        static std::map<uint32_t, uint32_t> seen;
        std::lock_guard<std::recursive_mutex> lock(emulation_mutex());
        if (++seen[static_cast<uint32_t>(requested_format)] == 1) {
            std::fprintf(stderr, "stud: texdec image format=%u emulated=%d %ux%u mips=%u\n",
                         static_cast<unsigned>(requested_format), emulated ? 1 : 0,
                         pCreateInfo->extent.width, pCreateInfo->extent.height,
                         pCreateInfo->mipLevels);
        }
    }
    h.format = static_cast<uint32_t>(emulated ? stud::texture_decode::substitute(requested_format)
                                              : requested_format);
    h.extent_width = pCreateInfo->extent.width;
    h.extent_height = pCreateInfo->extent.height;
    h.extent_depth = pCreateInfo->extent.depth;
    h.mip_levels = pCreateInfo->mipLevels;
    h.array_layers = pCreateInfo->arrayLayers;
    h.samples = static_cast<uint32_t>(pCreateInfo->samples);
    h.tiling = static_cast<uint32_t>(pCreateInfo->tiling);
    h.usage = pCreateInfo->usage;
    h.sharing_mode = static_cast<uint32_t>(pCreateInfo->sharingMode);
    h.queue_family_count = pCreateInfo->queueFamilyIndexCount;
    h.initial_layout = static_cast<uint32_t>(pCreateInfo->initialLayout);

    std::vector<uint8_t> chain;
    h.chain_node_count = flatten_chain(pCreateInfo->pNext, chain);

    std::vector<uint8_t> in(sizeof(h));
    std::memcpy(in.data(), &h, sizeof(h));
    if (h.queue_family_count > 0 && pCreateInfo->pQueueFamilyIndices != nullptr) {
        const auto* p = reinterpret_cast<const uint8_t*>(pCreateInfo->pQueueFamilyIndices);
        in.insert(in.end(), p, p + sizeof(uint32_t) * h.queue_family_count);
    }
    in.insert(in.end(), chain.begin(), chain.end());

    // For an emulated image, the memory its original format needs on a
    // device that samples it: every level, every layer. The host reports
    // this to the engine instead of the substitute's size while there is
    // room; see EmulatedImage in vulkan_host.cpp.
    uint64_t device_size = 0;
    if (emulated) {
        const uint32_t layers = std::max(1u, pCreateInfo->arrayLayers) *
                                std::max(1u, pCreateInfo->extent.depth);
        for (uint32_t m = 0; m < std::max(1u, pCreateInfo->mipLevels); ++m) {
            device_size += stud::texture_decode::encoded_size(
                               requested_format, std::max(1u, pCreateInfo->extent.width >> m),
                               std::max(1u, pCreateInfo->extent.height >> m)) *
                           layers;
        }
    }
    uint64_t a[8] = {to_u64(device), device_size};
    uint64_t handle = 0;
    uint32_t written = 0;
    uint64_t r = stud::render_client::connection().call(CallId::VkCreateImage, a, in.data(),
                                                         static_cast<uint32_t>(in.size()), &handle,
                                                         sizeof(handle), &written);
    VkResult result = static_cast<VkResult>(static_cast<int32_t>(r));
    if (result != VK_SUCCESS || written < sizeof(handle)) {
        return result != VK_SUCCESS ? result : VK_ERROR_INITIALIZATION_FAILED;
    }
    *pImage = from_u64<VkImage>(handle);
    // `in` IS the create parameters, already serialised for the host and
    // carrying the substituted format rather than the requested one, which
    // is what the object really has. Nothing better describes its shape.
    //
    // Not for an emulated image: the host decides its size per image, by
    // how much room there is at the time, so two of the same shape can be
    // told different things.
    if (!emulated) mem_req_cache().remember_shape(handle, std::string(in.begin(), in.end()));
    if (emulated) {
        std::lock_guard<std::recursive_mutex> lock(emulation_mutex());
        emulated_images()[handle] = EmulatedImage{requested_format, pCreateInfo->extent.width,
                                                  pCreateInfo->extent.height};
    }
    return VK_SUCCESS;
}

VKAPI_ATTR void VKAPI_CALL stud_vkGetImageMemoryRequirements(
    VkDevice device, VkImage image, VkMemoryRequirements* pMemoryRequirements) {
    if (pMemoryRequirements == nullptr) return;
    if (mem_req_cache().get(to_u64(image), *pMemoryRequirements)) return;
    uint64_t a[8] = {to_u64(device), to_u64(image)};
    std::vector<uint8_t> out(sizeof(uint32_t) + sizeof(VkMemoryRequirements));
    uint32_t written = 0;
    stud::render_client::connection().call(CallId::VkGetImageMemoryRequirements, a, nullptr, 0,
                                            out.data(), static_cast<uint32_t>(out.size()),
                                            &written);
    read_pod(out.data(), written, *pMemoryRequirements, "VkMemoryRequirements");
    mem_req_cache().put(to_u64(image), *pMemoryRequirements);
    mem_req_cache().report_if_asked();
}

VKAPI_ATTR VkResult VKAPI_CALL stud_vkGetPhysicalDeviceSurfaceCapabilitiesKHR(
    VkPhysicalDevice physicalDevice, VkSurfaceKHR surface,
    VkSurfaceCapabilitiesKHR* pSurfaceCapabilities) {
    if (pSurfaceCapabilities == nullptr) return VK_ERROR_INITIALIZATION_FAILED;
    // NOT cached, and that is deliberate. Asking costs a blocking round
    // trip on the render thread once a frame (1839 calls against 1838
    // frames, 0.42ms each, measured), which is real -- but the engine is
    // polling this to NOTICE that the window resized, and render-host owns
    // the real swapchain and absorbs a compositor resize itself, so the
    // engine's own acquire keeps returning VK_SUCCESS straight through
    // one. There is therefore no event on this side that reliably says
    // "the extent moved". Cached against those hooks it went stale and the
    // whole image rendered horizontally stretched, live-caught. If this is
    // ever worth removing, the invalidation has to come from render-host,
    // which is the only place that knows.
    uint64_t a[8] = {to_u64(physicalDevice), to_u64(surface)};
    std::vector<uint8_t> out(sizeof(uint32_t) + sizeof(VkSurfaceCapabilitiesKHR));
    uint32_t written = 0;
    uint64_t r = stud::render_client::connection().call(
        CallId::VkGetPhysicalDeviceSurfaceCapabilitiesKHR, a, nullptr, 0, out.data(),
        static_cast<uint32_t>(out.size()), &written);
    read_pod(out.data(), written, *pSurfaceCapabilities, "VkSurfaceCapabilitiesKHR");
    return static_cast<VkResult>(static_cast<int32_t>(r));
}

// The one real, guaranteed-dlsym-able entry point (see this file's own
// doc comment). Every other command is resolved by name through here,
// which is exactly what the Vulkan spec requires of a loader.
VKAPI_ATTR VkResult VKAPI_CALL stud_vkDeviceWaitIdle(VkDevice device) {
    // "Idle" has to include work this process has accepted but not yet
    // sent, or a teardown can destroy an object a queued submit names.
    const auto t0 = std::chrono::steady_clock::now();
    uint64_t a[8] = {to_u64(device)};
    uint64_t r = stud::render_client::connection().call(CallId::VkDeviceWaitIdle, a, nullptr, 0,
                                                         nullptr, 0, nullptr);
    if (frame_timing_enabled()) {
        frame_timing().wait_idle_ms +=
            std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
        ++frame_timing().wait_idle_calls;
    }
    // Everything submitted has finished, so anything read back is ready.
    if (static_cast<VkResult>(static_cast<int32_t>(r)) == VK_SUCCESS) fetch_pending_readbacks();
    return static_cast<VkResult>(static_cast<int32_t>(r));
}

VKAPI_ATTR VkResult VKAPI_CALL stud_vkAllocateMemory(VkDevice device,
                                                      const VkMemoryAllocateInfo* pAllocateInfo,
                                                      const VkAllocationCallbacks*,
                                                      VkDeviceMemory* pMemory) {
    if (pAllocateInfo == nullptr || pMemory == nullptr) return VK_ERROR_INITIALIZATION_FAILED;
    std::vector<uint8_t> chain;
    const uint32_t nodes = flatten_chain(pAllocateInfo->pNext, chain);
    // Worth sharing only for memory the engine will map. The host refuses
    // the import when the driver cannot back this memory type with a host
    // pointer, and then this falls back to copying.
    SharedAllocation shared;
    bool want_shared = false;
    if (shared_memory_enabled() &&
        host_visible_memory_types().count(pAllocateInfo->memoryTypeIndex) != 0) {
        want_shared = create_shared_allocation(pAllocateInfo->allocationSize, shared);
    }

    uint64_t a[8] = {to_u64(device), pAllocateInfo->allocationSize,
                     pAllocateInfo->memoryTypeIndex, nodes, want_shared ? shared.id : 0};
    uint64_t reply[2] = {0, 0};
    uint32_t written = 0;
    uint64_t r = stud::render_client::connection().call(
        CallId::VkAllocateMemory, a, chain.empty() ? nullptr : chain.data(),
        static_cast<uint32_t>(chain.size()), reply, sizeof(reply), &written);
    // The host has opened and mapped it by now (or refused), so the name
    // has done its whole job. Drop it here, on every path out of this
    // function, so nothing downstream has to remember to.
    unlink_shared_allocation_name(shared.id);
    const uint64_t handle = reply[0];
    VkResult result = static_cast<VkResult>(static_cast<int32_t>(r));
    if (result != VK_SUCCESS || written < sizeof(uint64_t)) {
        if (want_shared) release_shared_allocation(shared);
        return result != VK_SUCCESS ? result : VK_ERROR_OUT_OF_DEVICE_MEMORY;
    }
    *pMemory = from_u64<VkDeviceMemory>(handle);
    // Recorded now, while the type index is still in hand: the flush path
    // only ever sees the memory handle. See non_coherent_memory_types().
    if (non_coherent_memory_types().count(pAllocateInfo->memoryTypeIndex) != 0) {
        non_coherent_allocations().insert(handle);
    }
    // Share it only if the host says it really imported those pages. When
    // the driver refuses the import the host allocates ordinary memory
    // instead, and handing the engine the shared pointer anyway would mean
    // it writes where nothing reads, a black screen, live-caught.
    const bool host_shared = written >= sizeof(reply) && reply[1] != 0;
    if (want_shared && host_shared) {
        std::lock_guard<std::recursive_mutex> lock(emulation_mutex());
        shared_allocations()[handle] = shared;
    } else if (want_shared) {
        release_shared_allocation(shared);
    }
    return VK_SUCCESS;
}

VKAPI_ATTR VkResult VKAPI_CALL stud_vkBindImageMemory(VkDevice device, VkImage image,
                                                       VkDeviceMemory memory,
                                                       VkDeviceSize memoryOffset) {
    uint64_t a[8] = {to_u64(device), to_u64(image), to_u64(memory), memoryOffset};
    uint64_t r = stud::render_client::connection().call(CallId::VkBindImageMemory, a, nullptr, 0,
                                                         nullptr, 0, nullptr);
    return static_cast<VkResult>(static_cast<int32_t>(r));
}

VKAPI_ATTR void VKAPI_CALL stud_vkFreeMemory(VkDevice device, VkDeviceMemory memory,
                                              const VkAllocationCallbacks*) {
    {
        std::lock_guard<std::recursive_mutex> lock(mapped_mutex());
        mapped_ranges().erase(to_u64(memory));
        non_coherent_allocations().erase(to_u64(memory));
    }
    {
        std::lock_guard<std::recursive_mutex> lock(emulation_mutex());
        auto it = shared_allocations().find(to_u64(memory));
        if (it != shared_allocations().end()) {
            // The host still has the memory object at this point; it drops
            // its own mapping when it frees it. Unlinking now is safe,
            // the pages live until both mappings are gone.
            ::munmap(it->second.address, it->second.length);
            ::unlink(stud::render_host::shared_memory_path(it->second.id).c_str());
            shared_allocations().erase(it);
        }
    }
    uint64_t a[8] = {to_u64(device), to_u64(memory)};
    stud::render_client::connection().call(CallId::VkFreeMemory, a, nullptr, 0, nullptr, 0,
                                            nullptr);
}

VKAPI_ATTR VkResult VKAPI_CALL stud_vkMapMemory(VkDevice device, VkDeviceMemory memory,
                                                 VkDeviceSize offset, VkDeviceSize size,
                                                 VkMemoryMapFlags flags, void** ppData) {
    if (ppData == nullptr) return VK_ERROR_MEMORY_MAP_FAILED;
    uint64_t a[8] = {to_u64(device), to_u64(memory), offset, size, flags};
    uint64_t r = stud::render_client::connection().call(CallId::VkMapMemory, a, nullptr, 0, nullptr,
                                                         0, nullptr);
    VkResult result = static_cast<VkResult>(static_cast<int32_t>(r));
    if (result != VK_SUCCESS) return result;
    // VK_WHOLE_SIZE has no length the client can know; the engine always
    // maps a range it sized itself, so this is the case that matters.
    if (size == VK_WHOLE_SIZE) {
        std::fprintf(stderr, "stud: vulkan-client: vkMapMemory(VK_WHOLE_SIZE) is not supported\n");
        std::fflush(stderr);
        return VK_ERROR_MEMORY_MAP_FAILED;
    }
    {
        // Shared with the host: the engine writes straight into the
        // device's own memory, so there is nothing to stage, nothing to
        // track as dirty and nothing to send at submit.
        std::lock_guard<std::recursive_mutex> lock(emulation_mutex());
        auto shared = shared_allocations().find(to_u64(memory));
        if (shared != shared_allocations().end() && shared->second.address != nullptr) {
            *ppData = static_cast<uint8_t*>(shared->second.address) + offset;
            return VK_SUCCESS;
        }
    }
    std::lock_guard<std::recursive_mutex> lock(mapped_mutex());
    auto& slot = mapped_ranges()[to_u64(memory)];
    slot = MappedRange{};
    slot.offset = offset;
    slot.device = device;
    // Prefer the write barrier; fall back to a plain buffer if it cannot
    // be installed or the mapping cannot be allocated page-aligned, so a
    // mapping always works even where the barrier does not.
    // The barrier registers with Stud's own trap handler rather than
    // chaining sigaction: trap_recovery.cpp installs its SIGSEGV handler
    // later in bring-up, so a chained handler is simply replaced and
    // every barrier write-fault arrives at crash classification, is read
    // as wild-shaped, and kills the render thread, live-caught as a
    // window that never appeared. STUD_VK_NO_WRITE_BARRIER=1 forces the
    // compare-based path back for an A/B.
    static const bool barrier_ok = std::getenv("STUD_VK_NO_WRITE_BARRIER") == nullptr &&
                                   stud::render_client::install_write_barrier();
    // Back the barrier with a file the host maps too, so a flush only has
    // to say WHICH bytes moved instead of carrying them. Best effort: if
    // the host will not take it, this falls back to the plain mapping and
    // the socket, exactly as before.
    bool landed_shared = false;
    if (barrier_ok) {
        SharedAllocation bounce;
        if (create_shared_allocation(size, bounce)) {
            const std::string path = stud::render_host::shared_memory_path(bounce.id);
            const int fd = ::open(path.c_str(), O_RDWR | O_CLOEXEC);
            if (fd >= 0) {
                if (slot.barrier.allocate_backed_by(static_cast<size_t>(size), fd)) {
                    uint64_t sa[8] = {to_u64(device), to_u64(memory), bounce.id, size};
                    const uint64_t sr = stud::render_client::connection().call(
                        CallId::VkShareMappedMemory, sa, nullptr, 0, nullptr, 0, nullptr);
                    landed_shared = static_cast<VkResult>(static_cast<int32_t>(sr)) == VK_SUCCESS;
                    slot.shared_with_host = landed_shared;
                }
                ::close(fd);
            }
            // The client's own mapping of it is not needed: the barrier
            // holds the pages through its own.
            if (bounce.address != nullptr) ::munmap(bounce.address, bounce.length);
            unlink_shared_allocation_name(bounce.id);
        }
    }
    if (landed_shared) {
        ++g_shared_maps;
    } else {
        ++g_copied_maps;
        // Once per size class, so a mapping that had to fall back says so
        // without a line per allocation.
        static std::set<uint64_t> said;
        const uint64_t mb = size / (1024 * 1024);
        if (said.insert(mb).second) {
            std::printf("stud: vulkan-client: a %llu MB mapping could not be shared with the "
                        "host; its writes go through the socket\n",
                        static_cast<unsigned long long>(mb));
            std::fflush(stdout);
        }
    }
    if (!landed_shared && (!barrier_ok || !slot.barrier.allocate(static_cast<size_t>(size)))) {
        slot.staging.assign(static_cast<size_t>(size), 0);
        *ppData = slot.staging.data();
        return VK_SUCCESS;
    }
    // A fresh mapping is entirely dirty until the first flush ships it,
    // which is what allocate() already records.
    *ppData = slot.barrier.data();
    return VK_SUCCESS;
}

VKAPI_ATTR void VKAPI_CALL stud_vkUnmapMemory(VkDevice device, VkDeviceMemory memory) {
    const uint64_t key = to_u64(memory);
    std::lock_guard<std::recursive_mutex> lock(mapped_mutex());
    push_mapped_bytes(key, 0, VK_WHOLE_SIZE);
    mapped_ranges().erase(key);
    uint64_t a[8] = {to_u64(device), key};
    stud::render_client::connection().call(CallId::VkUnmapMemory, a, nullptr, 0, nullptr, 0,
                                            nullptr);
}

// Copies what the device holds for each range into the engine's own mapping.
//
// This used to accept the call and do nothing, on the reasoning that every
// mapping is upload-only. That held for everything observed, and would
// have been silently wrong the first time the engine read a result back:
// it would have seen its own last write, or zeroes. The readback is the
// same VkReadMappedMemory the copy-to-buffer path already uses, and the
// bytes are written through the engine's mapping so the write barrier
// treats them like any other write.
VKAPI_ATTR VkResult VKAPI_CALL stud_vkInvalidateMappedMemoryRanges(
    VkDevice device, uint32_t memoryRangeCount, const VkMappedMemoryRange* pMemoryRanges) {
    if (pMemoryRanges == nullptr) return VK_SUCCESS;
    constexpr uint64_t kChunk = 16ull << 20;
    std::vector<uint8_t> bytes;
    for (uint32_t i = 0; i < memoryRangeCount; ++i) {
        const VkMappedMemoryRange& r = pMemoryRanges[i];
        const uint64_t key = to_u64(r.memory);
        uint8_t* dst = nullptr;
        uint64_t start = 0;
        uint64_t length = 0;
        {
            std::lock_guard<std::recursive_mutex> lock(mapped_mutex());
            auto it = mapped_ranges().find(key);
            if (it == mapped_ranges().end()) continue;
            MappedRange& range = it->second;
            if (r.offset < range.offset) continue;
            const uint64_t local = r.offset - range.offset;
            if (local >= range.length()) continue;
            const uint64_t avail = range.length() - local;
            length = r.size == VK_WHOLE_SIZE ? avail : std::min<uint64_t>(r.size, avail);
            dst = range.bytes() + local;
            start = r.offset;
        }
        for (uint64_t done = 0; done < length;) {
            const uint64_t want = std::min<uint64_t>(kChunk, length - done);
            bytes.assign(static_cast<size_t>(want), 0);
            uint64_t a[8] = {to_u64(device), key, start + done, want};
            uint32_t written = 0;
            const uint64_t res = stud::render_client::connection().call(
                CallId::VkReadMappedMemory, a, nullptr, 0, bytes.data(),
                static_cast<uint32_t>(bytes.size()), &written);
            const VkResult vr = static_cast<VkResult>(static_cast<int32_t>(res));
            if (vr != VK_SUCCESS) return vr;
            if (written == 0) break;
            std::memcpy(dst + done, bytes.data(), std::min<size_t>(written, static_cast<size_t>(want)));
            done += written;
        }
    }
    return VK_SUCCESS;
}

VKAPI_ATTR VkResult VKAPI_CALL stud_vkFlushMappedMemoryRanges(
    VkDevice device, uint32_t memoryRangeCount, const VkMappedMemoryRange* pMemoryRanges) {
    if (pMemoryRanges == nullptr) return VK_SUCCESS;
    for (uint32_t i = 0; i < memoryRangeCount; ++i) {
        const VkMappedMemoryRange& r = pMemoryRanges[i];
        const uint64_t key = to_u64(r.memory);
        std::lock_guard<std::recursive_mutex> lock(mapped_mutex());
        auto it = mapped_ranges().find(key);
        // The range's offset is absolute; the staging buffer starts at
        // whatever offset the mapping used.
        const uint64_t rel = it != mapped_ranges().end() && r.offset >= it->second.offset
                                 ? r.offset - it->second.offset
                                 : 0;
        push_mapped_bytes(key, rel, r.size);
        uint64_t a[8] = {to_u64(device), key, r.offset, r.size};
        stud::render_client::connection().call(CallId::VkFlushMappedMemoryRanges, a, nullptr, 0,
                                                nullptr, 0, nullptr);
    }
    return VK_SUCCESS;
}

VKAPI_ATTR VkResult VKAPI_CALL stud_vkGetPhysicalDeviceSurfaceFormatsKHR(
    VkPhysicalDevice physicalDevice, VkSurfaceKHR surface, uint32_t* pSurfaceFormatCount,
    VkSurfaceFormatKHR* pSurfaceFormats) {
    if (pSurfaceFormatCount == nullptr) return VK_ERROR_INITIALIZATION_FAILED;
    const uint32_t capacity = pSurfaceFormats != nullptr ? *pSurfaceFormatCount : 0;
    uint64_t a[8] = {to_u64(physicalDevice), to_u64(surface), capacity};
    std::vector<uint8_t> out(sizeof(uint32_t) * 2 + sizeof(VkSurfaceFormatKHR) * (capacity + 1));
    uint32_t written = 0;
    uint64_t r = stud::render_client::connection().call(
        CallId::VkGetPhysicalDeviceSurfaceFormatsKHR, a, nullptr, 0, out.data(),
        static_cast<uint32_t>(out.size()), &written);
    if (written < sizeof(uint32_t) * 2) return VK_ERROR_INITIALIZATION_FAILED;
    uint32_t count = 0, stride = 0;
    std::memcpy(&count, out.data(), sizeof(count));
    std::memcpy(&stride, out.data() + sizeof(uint32_t), sizeof(stride));
    if (pSurfaceFormats == nullptr) {
        *pSurfaceFormatCount = count;
        return static_cast<VkResult>(static_cast<int32_t>(r));
    }
    if (stride != sizeof(VkSurfaceFormatKHR)) return VK_ERROR_INITIALIZATION_FAILED;
    const uint32_t have = (written - sizeof(uint32_t) * 2) / stride;
    const uint32_t n = count < have ? count : have;
    for (uint32_t i = 0; i < n; ++i) {
        std::memcpy(&pSurfaceFormats[i], out.data() + sizeof(uint32_t) * 2 + i * stride, stride);
    }
    *pSurfaceFormatCount = n;
    return static_cast<VkResult>(static_cast<int32_t>(r));
}

VKAPI_ATTR VkResult VKAPI_CALL stud_vkGetPhysicalDeviceSurfacePresentModesKHR(
    VkPhysicalDevice physicalDevice, VkSurfaceKHR surface, uint32_t* pPresentModeCount,
    VkPresentModeKHR* pPresentModes) {
    if (pPresentModeCount == nullptr) return VK_ERROR_INITIALIZATION_FAILED;
    const uint32_t capacity = pPresentModes != nullptr ? *pPresentModeCount : 0;
    uint64_t a[8] = {to_u64(physicalDevice), to_u64(surface), capacity};
    std::vector<uint8_t> out(sizeof(uint32_t) * (capacity + 2));
    uint32_t written = 0;
    uint64_t r = stud::render_client::connection().call(
        CallId::VkGetPhysicalDeviceSurfacePresentModesKHR, a, nullptr, 0, out.data(),
        static_cast<uint32_t>(out.size()), &written);
    if (written < sizeof(uint32_t)) return VK_ERROR_INITIALIZATION_FAILED;
    uint32_t count = 0;
    std::memcpy(&count, out.data(), sizeof(count));
    if (pPresentModes == nullptr) {
        *pPresentModeCount = count;
        return static_cast<VkResult>(static_cast<int32_t>(r));
    }
    const uint32_t have = (written - sizeof(uint32_t)) / sizeof(uint32_t);
    const uint32_t n = count < have ? count : have;
    for (uint32_t i = 0; i < n; ++i) {
        uint32_t m = 0;
        std::memcpy(&m, out.data() + sizeof(uint32_t) * (1 + i), sizeof(m));
        pPresentModes[i] = static_cast<VkPresentModeKHR>(m);
    }
    *pPresentModeCount = n;
    return static_cast<VkResult>(static_cast<int32_t>(r));
}

VKAPI_ATTR VkResult VKAPI_CALL stud_vkGetPhysicalDeviceSurfaceSupportKHR(
    VkPhysicalDevice physicalDevice, uint32_t queueFamilyIndex, VkSurfaceKHR surface,
    VkBool32* pSupported) {
    if (pSupported == nullptr) return VK_ERROR_INITIALIZATION_FAILED;
    uint64_t a[8] = {to_u64(physicalDevice), queueFamilyIndex, to_u64(surface)};
    std::vector<uint8_t> out(sizeof(uint32_t) + sizeof(VkBool32));
    uint32_t written = 0;
    uint64_t r = stud::render_client::connection().call(
        CallId::VkGetPhysicalDeviceSurfaceSupportKHR, a, nullptr, 0, out.data(),
        static_cast<uint32_t>(out.size()), &written);
    read_pod(out.data(), written, *pSupported, "VkBool32");
    return static_cast<VkResult>(static_cast<int32_t>(r));
}

VKAPI_ATTR VkResult VKAPI_CALL stud_vkCreateSwapchainKHR(
    VkDevice device, const VkSwapchainCreateInfoKHR* pCreateInfo, const VkAllocationCallbacks*,
    VkSwapchainKHR* pSwapchain) {
    if (pCreateInfo == nullptr || pSwapchain == nullptr) return VK_ERROR_INITIALIZATION_FAILED;
    vk_wire::CreateSwapchainHeader h{};
    h.surface = to_u64(pCreateInfo->surface);
    h.old_swapchain = to_u64(pCreateInfo->oldSwapchain);
    h.flags = pCreateInfo->flags;
    h.min_image_count = pCreateInfo->minImageCount;
    h.image_format = static_cast<uint32_t>(pCreateInfo->imageFormat);
    h.image_color_space = static_cast<uint32_t>(pCreateInfo->imageColorSpace);
    h.width = pCreateInfo->imageExtent.width;
    h.height = pCreateInfo->imageExtent.height;
    h.image_array_layers = pCreateInfo->imageArrayLayers;
    h.image_usage = pCreateInfo->imageUsage;
    h.sharing_mode = static_cast<uint32_t>(pCreateInfo->imageSharingMode);
    h.queue_family_count = pCreateInfo->queueFamilyIndexCount;
    h.pre_transform = static_cast<uint32_t>(pCreateInfo->preTransform);
    h.composite_alpha = static_cast<uint32_t>(pCreateInfo->compositeAlpha);
    h.present_mode = static_cast<uint32_t>(pCreateInfo->presentMode);
    h.clipped = pCreateInfo->clipped;

    std::vector<uint8_t> in(sizeof(h));
    std::memcpy(in.data(), &h, sizeof(h));
    if (h.queue_family_count > 0 && pCreateInfo->pQueueFamilyIndices != nullptr) {
        const auto* p = reinterpret_cast<const uint8_t*>(pCreateInfo->pQueueFamilyIndices);
        in.insert(in.end(), p, p + sizeof(uint32_t) * h.queue_family_count);
    }

    uint64_t a[8] = {to_u64(device)};
    uint64_t handle = 0;
    uint32_t written = 0;
    uint64_t r = stud::render_client::connection().call(CallId::VkCreateSwapchainKHR, a, in.data(),
                                                         static_cast<uint32_t>(in.size()), &handle,
                                                         sizeof(handle), &written);
    VkResult result = static_cast<VkResult>(static_cast<int32_t>(r));
    if (result != VK_SUCCESS || written < sizeof(handle)) {
        return result != VK_SUCCESS ? result : VK_ERROR_INITIALIZATION_FAILED;
    }
    *pSwapchain = from_u64<VkSwapchainKHR>(handle);
    return VK_SUCCESS;
}

VKAPI_ATTR VkResult VKAPI_CALL stud_vkGetSwapchainImagesKHR(VkDevice device,
                                                             VkSwapchainKHR swapchain,
                                                             uint32_t* pSwapchainImageCount,
                                                             VkImage* pSwapchainImages) {
    if (pSwapchainImageCount == nullptr) return VK_ERROR_INITIALIZATION_FAILED;
    const uint32_t capacity = pSwapchainImages != nullptr ? *pSwapchainImageCount : 0;
    uint64_t a[8] = {to_u64(device), to_u64(swapchain), capacity};
    std::vector<uint8_t> out(sizeof(uint32_t) + sizeof(uint64_t) * (capacity + 1));
    uint32_t written = 0;
    uint64_t r = stud::render_client::connection().call(CallId::VkGetSwapchainImagesKHR, a, nullptr,
                                                         0, out.data(),
                                                         static_cast<uint32_t>(out.size()),
                                                         &written);
    if (written < sizeof(uint32_t)) return VK_ERROR_INITIALIZATION_FAILED;
    uint32_t count = 0;
    std::memcpy(&count, out.data(), sizeof(count));
    if (pSwapchainImages == nullptr) {
        *pSwapchainImageCount = count;
        return static_cast<VkResult>(static_cast<int32_t>(r));
    }
    const uint32_t have = (written - sizeof(uint32_t)) / sizeof(uint64_t);
    const uint32_t n = count < have ? count : have;
    for (uint32_t i = 0; i < n; ++i) {
        uint64_t hh = 0;
        std::memcpy(&hh, out.data() + sizeof(uint32_t) + i * sizeof(hh), sizeof(hh));
        pSwapchainImages[i] = from_u64<VkImage>(hh);
    }
    *pSwapchainImageCount = n;
    return static_cast<VkResult>(static_cast<int32_t>(r));
}

VKAPI_ATTR VkResult VKAPI_CALL stud_vkGetPhysicalDeviceImageFormatProperties2(
    VkPhysicalDevice physicalDevice, const VkPhysicalDeviceImageFormatInfo2* pImageFormatInfo,
    VkImageFormatProperties2* pImageFormatProperties) {
    if (pImageFormatInfo == nullptr || pImageFormatProperties == nullptr) {
        return VK_ERROR_INITIALIZATION_FAILED;
    }
    uint32_t q[6] = {static_cast<uint32_t>(pImageFormatInfo->format),
                     static_cast<uint32_t>(pImageFormatInfo->type),
                     static_cast<uint32_t>(pImageFormatInfo->tiling),
                     pImageFormatInfo->usage,
                     pImageFormatInfo->flags,
                     0};
    std::vector<uint8_t> in(sizeof(q));
    std::memcpy(in.data(), q, sizeof(q));
    std::vector<uint8_t> chain;
    const uint32_t nodes = flatten_chain(pImageFormatProperties->pNext, chain);
    in.insert(in.end(), chain.begin(), chain.end());

    uint64_t a[8] = {to_u64(physicalDevice), nodes};
    std::vector<uint8_t> out(sizeof(uint32_t) + sizeof(VkImageFormatProperties) + chain.size() +
                             sizeof(vk_wire::ChainNodeHeader) * (nodes + 1));
    uint32_t written = 0;
    uint64_t r = stud::render_client::connection().call(
        CallId::VkGetPhysicalDeviceImageFormatProperties2, a, in.data(),
        static_cast<uint32_t>(in.size()), out.data(), static_cast<uint32_t>(out.size()), &written);
    if (!read_pod(out.data(), written, pImageFormatProperties->imageFormatProperties,
                  "VkImageFormatProperties")) {
        return VK_ERROR_INITIALIZATION_FAILED;
    }
    const size_t consumed = sizeof(uint32_t) + sizeof(VkImageFormatProperties);
    if (written > consumed) {
        scatter_chain(pImageFormatProperties->pNext, out.data() + consumed, written - consumed);
    }
    return static_cast<VkResult>(static_cast<int32_t>(r));
}

// Android's surface extension does not exist on Wayland, so this creates
// a real Wayland surface on the window Process C already owns. The engine
// never learns which WSI is underneath, the same substitution
// vkCreateInstance makes for the extension name itself.
VKAPI_ATTR VkResult VKAPI_CALL stud_vkCreateAndroidSurfaceKHR(
    VkInstance instance, const void* /*pCreateInfo*/, const VkAllocationCallbacks*,
    VkSurfaceKHR* pSurface) {
    if (pSurface == nullptr) return VK_ERROR_INITIALIZATION_FAILED;
    uint64_t a[8] = {to_u64(instance)};
    uint64_t handle = stud::render_client::connection().call(
        CallId::VkCreateWaylandSurfaceForAndroidSurface, a, nullptr, 0, nullptr, 0, nullptr);
    if (handle == 0) return VK_ERROR_INITIALIZATION_FAILED;
    *pSurface = from_u64<VkSurfaceKHR>(handle);
    return VK_SUCCESS;
}

VKAPI_ATTR VkResult VKAPI_CALL stud_vkCreateBuffer(VkDevice device,
                                                    const VkBufferCreateInfo* pCreateInfo,
                                                    const VkAllocationCallbacks*,
                                                    VkBuffer* pBuffer) {
    if (pCreateInfo == nullptr) return VK_ERROR_INITIALIZATION_FAILED;
    uint64_t a[8] = {to_u64(device), pCreateInfo->flags, pCreateInfo->size, pCreateInfo->usage,
                     static_cast<uint64_t>(pCreateInfo->sharingMode)};
    const VkResult r = simple_create<VkBuffer, CallId::VkCreateBuffer>(a, pBuffer);
    if (r == VK_SUCCESS) {
        // Everything the host is given to create this buffer with, minus
        // the device -- so two buffers of the same shape on the same
        // device share a key. See MemoryRequirementsCache.
        char shape[sizeof(uint64_t) * 4];
        std::memcpy(shape, &a[1], sizeof(shape));
        mem_req_cache().remember_shape(to_u64(*pBuffer), std::string(shape, sizeof(shape)));
        std::lock_guard<std::recursive_mutex> lock(emulation_mutex());
        buffer_sizes()[to_u64(*pBuffer)] = pCreateInfo->size;
    }
    return r;
}

VKAPI_ATTR void VKAPI_CALL stud_vkGetBufferMemoryRequirements(
    VkDevice device, VkBuffer buffer, VkMemoryRequirements* pMemoryRequirements) {
    if (pMemoryRequirements == nullptr) return;
    if (mem_req_cache().get(to_u64(buffer), *pMemoryRequirements)) return;
    uint64_t a[8] = {to_u64(device), to_u64(buffer)};
    std::vector<uint8_t> out(sizeof(uint32_t) + sizeof(VkMemoryRequirements));
    uint32_t written = 0;
    stud::render_client::connection().call(CallId::VkGetBufferMemoryRequirements, a, nullptr, 0,
                                            out.data(), static_cast<uint32_t>(out.size()),
                                            &written);
    read_pod(out.data(), written, *pMemoryRequirements, "VkMemoryRequirements");
    mem_req_cache().put(to_u64(buffer), *pMemoryRequirements);
    mem_req_cache().report_if_asked();
}

VKAPI_ATTR VkResult VKAPI_CALL stud_vkBindBufferMemory(VkDevice device, VkBuffer buffer,
                                                        VkDeviceMemory memory,
                                                        VkDeviceSize memoryOffset) {
    uint64_t a[8] = {to_u64(device), to_u64(buffer), to_u64(memory), memoryOffset};
    uint64_t r = stud::render_client::connection().call(CallId::VkBindBufferMemory, a, nullptr, 0,
                                                         nullptr, 0, nullptr);
    if (static_cast<VkResult>(static_cast<int32_t>(r)) == VK_SUCCESS) {
        // Only needed to find a staging buffer's bytes when a copy out of
        // it has to be decoded; see stud_vkCmdCopyBufferToImage.
        std::lock_guard<std::recursive_mutex> lock(emulation_mutex());
        const auto size_it = buffer_sizes().find(to_u64(buffer));
        buffer_bindings()[to_u64(buffer)] = BufferBinding{
            to_u64(memory), memoryOffset,
            size_it == buffer_sizes().end() ? 0 : size_it->second};
    }
    return static_cast<VkResult>(static_cast<int32_t>(r));
}

VKAPI_ATTR VkResult VKAPI_CALL stud_vkCreateImageView(VkDevice device,
                                                       const VkImageViewCreateInfo* pCreateInfo,
                                                       const VkAllocationCallbacks*,
                                                       VkImageView* pView) {
    if (pCreateInfo == nullptr || pView == nullptr) return VK_ERROR_INITIALIZATION_FAILED;
    uint64_t image = to_u64(pCreateInfo->image);
    uint32_t q[12] = {pCreateInfo->flags,
                      static_cast<uint32_t>(pCreateInfo->viewType),
                      static_cast<uint32_t>(emulating(pCreateInfo->format)
                                                ? stud::texture_decode::substitute(pCreateInfo->format)
                                                : pCreateInfo->format),
                      static_cast<uint32_t>(pCreateInfo->components.r),
                      static_cast<uint32_t>(pCreateInfo->components.g),
                      static_cast<uint32_t>(pCreateInfo->components.b),
                      static_cast<uint32_t>(pCreateInfo->components.a),
                      pCreateInfo->subresourceRange.aspectMask,
                      pCreateInfo->subresourceRange.baseMipLevel,
                      pCreateInfo->subresourceRange.levelCount,
                      pCreateInfo->subresourceRange.baseArrayLayer,
                      pCreateInfo->subresourceRange.layerCount};
    std::vector<uint8_t> in(sizeof(image) + sizeof(q));
    std::memcpy(in.data(), &image, sizeof(image));
    std::memcpy(in.data() + sizeof(image), q, sizeof(q));

    uint64_t a[8] = {to_u64(device)};
    uint64_t handle = 0;
    uint32_t written = 0;
    uint64_t r = stud::render_client::connection().call(CallId::VkCreateImageView, a, in.data(),
                                                         static_cast<uint32_t>(in.size()), &handle,
                                                         sizeof(handle), &written);
    VkResult result = static_cast<VkResult>(static_cast<int32_t>(r));
    if (result != VK_SUCCESS || written < sizeof(handle)) {
        return result != VK_SUCCESS ? result : VK_ERROR_INITIALIZATION_FAILED;
    }
    *pView = from_u64<VkImageView>(handle);
    return VK_SUCCESS;
}

VKAPI_ATTR VkResult VKAPI_CALL stud_vkCreateShaderModule(
    VkDevice device, const VkShaderModuleCreateInfo* pCreateInfo, const VkAllocationCallbacks*,
    VkShaderModule* pShaderModule) {
    if (pCreateInfo == nullptr || pShaderModule == nullptr) return VK_ERROR_INITIALIZATION_FAILED;
    uint64_t a[8] = {to_u64(device)};
    uint64_t handle = 0;
    uint32_t written = 0;
    uint64_t r = stud::render_client::connection().call(
        CallId::VkCreateShaderModule, a, pCreateInfo->pCode,
        static_cast<uint32_t>(pCreateInfo->codeSize), &handle, sizeof(handle), &written);
    VkResult result = static_cast<VkResult>(static_cast<int32_t>(r));
    if (result != VK_SUCCESS || written < sizeof(handle)) {
        return result != VK_SUCCESS ? result : VK_ERROR_INITIALIZATION_FAILED;
    }
    *pShaderModule = from_u64<VkShaderModule>(handle);
    return VK_SUCCESS;
}

// Every vkDestroyX has the same shape and returns nothing, so they share
// one call id and ride the reply-free path; no round-trip each.
void destroy_handle(VkDevice device, vk_wire::DestroyKind kind, uint64_t handle) {
    uint64_t a[8] = {to_u64(device), static_cast<uint64_t>(kind), handle};
    stud::render_client::connection().call_void(CallId::VkDestroyHandle, a);
}

#define STUD_VK_DESTROY(fn, type, kind)                                                      \
    VKAPI_ATTR void VKAPI_CALL fn(VkDevice device, type handle, const VkAllocationCallbacks*) { \
        destroy_handle(device, vk_wire::DestroyKind::kind, to_u64(handle));                  \
    }

// Buffers and images carry Stud's own texture-decode bookkeeping, and a
// driver is free to hand the same handle value back for the next object
// it creates. A stale entry therefore attaches one texture's staging
// buffer, or one image's compressed format, to a completely different
// object, which decodes the wrong bytes, or decodes bytes that should
// have been left alone. Both of those look like a handful of corrupt
// textures rather than an obvious failure, so these two do not use the
// shared macro.
VKAPI_ATTR void VKAPI_CALL stud_vkDestroyBuffer(VkDevice device, VkBuffer handle,
                                                 const VkAllocationCallbacks*) {
    {
        std::lock_guard<std::recursive_mutex> lock(emulation_mutex());
        buffer_bindings().erase(to_u64(handle));
        buffer_sizes().erase(to_u64(handle));
        // The driver reuses handles; a stale shape would answer for the
        // next object to get this one's number.
        mem_req_cache().forget(to_u64(handle));
        // Anything still queued against this buffer can no longer read it.
        auto& pending = pending_decodes();
        pending.erase(std::remove_if(pending.begin(), pending.end(),
                                     [handle](const PendingDecode& pd) { return pd.src == handle; }),
                      pending.end());
    }
    destroy_handle(device, vk_wire::DestroyKind::Buffer, to_u64(handle));
}

VKAPI_ATTR void VKAPI_CALL stud_vkDestroyImage(VkDevice device, VkImage handle,
                                                const VkAllocationCallbacks*) {
    {
        std::lock_guard<std::recursive_mutex> lock(emulation_mutex());
        emulated_images().erase(to_u64(handle));
        mem_req_cache().forget(to_u64(handle));
    }
    destroy_handle(device, vk_wire::DestroyKind::Image, to_u64(handle));
}
STUD_VK_DESTROY(stud_vkDestroyImageView, VkImageView, ImageView)
STUD_VK_DESTROY(stud_vkDestroyShaderModule, VkShaderModule, ShaderModule)
STUD_VK_DESTROY(stud_vkDestroySemaphore, VkSemaphore, Semaphore)
STUD_VK_DESTROY(stud_vkDestroyFence, VkFence, Fence)
STUD_VK_DESTROY(stud_vkDestroyCommandPool, VkCommandPool, CommandPool)
STUD_VK_DESTROY(stud_vkDestroyQueryPool, VkQueryPool, QueryPool)
STUD_VK_DESTROY(stud_vkDestroySwapchainKHR, VkSwapchainKHR, Swapchain)
STUD_VK_DESTROY(stud_vkDestroyFramebuffer, VkFramebuffer, Framebuffer)
// The pipeline family. Every one of these was resolving to a named
// no-op stub, so the host kept the objects forever, 2668 of them in a
// handful of sessions, which is GPU memory that grows with every join.
STUD_VK_DESTROY(stud_vkDestroySampler, VkSampler, Sampler)
STUD_VK_DESTROY(stud_vkDestroyRenderPass, VkRenderPass, RenderPass)
STUD_VK_DESTROY(stud_vkDestroyPipeline, VkPipeline, Pipeline)
STUD_VK_DESTROY(stud_vkDestroyPipelineLayout, VkPipelineLayout, PipelineLayout)
STUD_VK_DESTROY(stud_vkDestroyDescriptorSetLayout, VkDescriptorSetLayout, DescriptorSetLayout)
STUD_VK_DESTROY(stud_vkDestroyDescriptorPool, VkDescriptorPool, DescriptorPool)
STUD_VK_DESTROY(stud_vkDestroyDescriptorUpdateTemplate, VkDescriptorUpdateTemplate,
                DescriptorUpdateTemplate)
#undef STUD_VK_DESTROY

VKAPI_ATTR void VKAPI_CALL stud_vkDestroySurfaceKHR(VkInstance instance, VkSurfaceKHR surface,
                                                     const VkAllocationCallbacks*) {
    uint64_t a[8] = {to_u64(instance),
                     static_cast<uint64_t>(vk_wire::DestroyKind::Surface), to_u64(surface)};
    stud::render_client::connection().call_void(CallId::VkDestroyHandle, a);
}

VKAPI_ATTR void VKAPI_CALL stud_vkDestroyDevice(VkDevice device, const VkAllocationCallbacks*) {
    // FORGET THE DECODE SCRATCH FIRST. It belongs to the device that is
    // about to die.
    //
    // scratch_pools() holds a VkBuffer per command buffer, plus the
    // retired ones growth left behind, and they are deliberately kept
    // alive because work may still reference them. Nothing cleared them
    // when the device went away, so the next texture upload after a
    // device rebuild reused the pool for that command buffer and
    // recorded vkCmdCopyBufferToImage against a buffer belonging to a
    // destroyed device.
    //
    // That is a dangling handle, and the driver dereferences it:
    //
    //   vkCmdCopyBufferToImage(): srcBuffer Invalid VkBuffer Object
    //   CRASH, signal 11, fault address 0xf8
    //   last: vk_cmd_record kind=14   (CopyBufferToImage)
    //
    // inside libnvidia-glcore. It needs a device rebuild to happen at
    // all, which is why it shows up when switching games and not while
    // sitting in one.
    //
    // FORGOTTEN, NOT DESTROYED. Destroying them here would be the same
    // mistake in the other direction -- the host learned that the hard
    // way when destroying the engine's abandoned buffers turned a leak
    // into exactly this crash. The device teardown reclaims them; all
    // that is needed is that nothing reaches for them afterwards.
    {
        std::lock_guard<std::recursive_mutex> lock(emulation_mutex());
        scratch_pools().clear();
        pending_decodes().clear();
    }
    destroy_handle(device, vk_wire::DestroyKind::Device, to_u64(device));
}

VKAPI_ATTR void VKAPI_CALL stud_vkDestroyInstance(VkInstance instance,
                                                   const VkAllocationCallbacks*) {
    uint64_t a[8] = {to_u64(instance),
                     static_cast<uint64_t>(vk_wire::DestroyKind::Instance), to_u64(instance)};
    stud::render_client::connection().call_void(CallId::VkDestroyHandle, a);
}


// ---- draw pipeline ---------------------------------------------------
//
// Each of these mirrors a Reader on the host, field for field, in the
// same order. The pairing is the whole contract: there is no schema, so
// a field added on one side must be added on the other.

VKAPI_ATTR VkResult VKAPI_CALL stud_vkCreateRenderPass(VkDevice device,
                                                        const VkRenderPassCreateInfo* ci,
                                                        const VkAllocationCallbacks*,
                                                        VkRenderPass* pRenderPass) {
    if (ci == nullptr || pRenderPass == nullptr) return VK_ERROR_INITIALIZATION_FAILED;
    std::vector<uint8_t> in;
    vk_wire::Writer w(in);
    w.u32(ci->flags);
    w.u32(ci->attachmentCount);
    for (uint32_t i = 0; i < ci->attachmentCount; ++i) {
        const auto& a = ci->pAttachments[i];
        w.u32(a.flags);
        w.u32(a.format);
        w.u32(a.samples);
        w.u32(a.loadOp);
        w.u32(a.storeOp);
        w.u32(a.stencilLoadOp);
        w.u32(a.stencilStoreOp);
        w.u32(a.initialLayout);
        w.u32(a.finalLayout);
    }
    w.u32(ci->subpassCount);
    auto write_refs = [&w](const VkAttachmentReference* refs, uint32_t n) {
        w.u32(refs != nullptr ? n : 0);
        if (refs == nullptr) return;
        for (uint32_t i = 0; i < n; ++i) {
            w.u32(refs[i].attachment);
            w.u32(refs[i].layout);
        }
    };
    for (uint32_t i = 0; i < ci->subpassCount; ++i) {
        const auto& sp = ci->pSubpasses[i];
        w.u32(sp.flags);
        w.u32(sp.pipelineBindPoint);
        write_refs(sp.pInputAttachments, sp.inputAttachmentCount);
        write_refs(sp.pColorAttachments, sp.colorAttachmentCount);
        write_refs(sp.pResolveAttachments, sp.colorAttachmentCount);
        w.u32(sp.pDepthStencilAttachment != nullptr ? 1 : 0);
        if (sp.pDepthStencilAttachment != nullptr) {
            w.u32(sp.pDepthStencilAttachment->attachment);
            w.u32(sp.pDepthStencilAttachment->layout);
        }
        w.u32(sp.preserveAttachmentCount);
        for (uint32_t j = 0; j < sp.preserveAttachmentCount; ++j) {
            w.u32(sp.pPreserveAttachments[j]);
        }
    }
    w.u32(ci->dependencyCount);
    for (uint32_t i = 0; i < ci->dependencyCount; ++i) {
        const auto& d = ci->pDependencies[i];
        w.u32(d.srcSubpass);
        w.u32(d.dstSubpass);
        w.u32(d.srcStageMask);
        w.u32(d.dstStageMask);
        w.u32(d.srcAccessMask);
        w.u32(d.dstAccessMask);
        w.u32(d.dependencyFlags);
    }
    uint64_t a[8] = {to_u64(device)};
    return create_from_payload<VkRenderPass, CallId::VkCreateRenderPass>(a, in, pRenderPass);
}

VKAPI_ATTR VkResult VKAPI_CALL stud_vkCreateFramebuffer(VkDevice device,
                                                         const VkFramebufferCreateInfo* ci,
                                                         const VkAllocationCallbacks*,
                                                         VkFramebuffer* pFramebuffer) {
    if (ci == nullptr || pFramebuffer == nullptr) return VK_ERROR_INITIALIZATION_FAILED;
    std::vector<uint8_t> in;
    vk_wire::Writer w(in);
    w.u32(ci->flags);
    w.u64(to_u64(ci->renderPass));
    w.u32(ci->attachmentCount);
    for (uint32_t i = 0; i < ci->attachmentCount; ++i) w.u64(to_u64(ci->pAttachments[i]));
    w.u32(ci->width);
    w.u32(ci->height);
    w.u32(ci->layers);
    uint64_t a[8] = {to_u64(device)};
    return create_from_payload<VkFramebuffer, CallId::VkCreateFramebuffer>(a, in, pFramebuffer);
}

VKAPI_ATTR VkResult VKAPI_CALL stud_vkCreateSampler(VkDevice device,
                                                     const VkSamplerCreateInfo* ci,
                                                     const VkAllocationCallbacks*,
                                                     VkSampler* pSampler) {
    if (ci == nullptr || pSampler == nullptr) return VK_ERROR_INITIALIZATION_FAILED;
    std::vector<uint8_t> in;
    vk_wire::Writer w(in);
    w.u32(ci->flags);
    w.u32(ci->magFilter);
    w.u32(ci->minFilter);
    w.u32(ci->mipmapMode);
    w.u32(ci->addressModeU);
    w.u32(ci->addressModeV);
    w.u32(ci->addressModeW);
    w.f32(ci->mipLodBias);
    w.u32(ci->anisotropyEnable);
    w.f32(ci->maxAnisotropy);
    w.u32(ci->compareEnable);
    w.u32(ci->compareOp);
    w.f32(ci->minLod);
    w.f32(ci->maxLod);
    w.u32(ci->borderColor);
    w.u32(ci->unnormalizedCoordinates);
    uint64_t a[8] = {to_u64(device)};
    return create_from_payload<VkSampler, CallId::VkCreateSampler>(a, in, pSampler);
}

// Says once, loudly, that this wire dropped something it was given.
//
// NOT a fix. Carrying specialization constants means serialising the map
// entries and the data blob on both sides, and carrying a descriptor
// layout's pNext means reconstructing the binding-flags chain -- neither
// is worth writing on the chance the engine might use it. These say
// whether it actually does, which is the thing nobody knew.
//
// Both are silent by nature: a dropped specialization constant compiles
// the shader with a default and computes the wrong thing without a word,
// and a dropped binding-flags chain gives the host a layout of a
// different shape from the one the engine believes it made. That is
// exactly the kind of drop worth a line in the log rather than a guess.
void report_dropped(const void* present, const char* what, const char* where) {
    if (present == nullptr) return;
    // One per kind: these are per-pipeline and per-layout calls, and the
    // engine makes thousands.
    static std::set<std::string> said;
    const std::string key = std::string(where) + "/" + what;
    if (!said.insert(key).second) return;
    std::fprintf(stderr,
                 "stud: vulkan-client: %s was given %s, which this wire DROPS -- it never "
                 "reaches the real driver\n",
                 where, what);
    std::fflush(stderr);
}

void report_dropped_specialization(const void* spec, const char* where) {
    report_dropped(spec, "specialization constants", where);
}

VKAPI_ATTR VkResult VKAPI_CALL stud_vkCreateDescriptorSetLayout(
    VkDevice device, const VkDescriptorSetLayoutCreateInfo* ci, const VkAllocationCallbacks*,
    VkDescriptorSetLayout* pLayout) {
    if (ci == nullptr || pLayout == nullptr) return VK_ERROR_INITIALIZATION_FAILED;
    // Chiefly VkDescriptorSetLayoutBindingFlagsCreateInfo: the
    // UPDATE_AFTER_BIND / PARTIALLY_BOUND / VARIABLE_DESCRIPTOR_COUNT
    // flags that descriptor indexing needs.
    report_dropped(ci->pNext, "a pNext chain", "vkCreateDescriptorSetLayout");
    std::vector<uint8_t> in;
    vk_wire::Writer w(in);
    w.u32(ci->flags);
    w.u32(ci->bindingCount);
    for (uint32_t i = 0; i < ci->bindingCount; ++i) {
        const auto& b = ci->pBindings[i];
        w.u32(b.binding);
        w.u32(b.descriptorType);
        w.u32(b.descriptorCount);
        w.u32(b.stageFlags);
        const uint32_t ns = b.pImmutableSamplers != nullptr ? b.descriptorCount : 0;
        w.u32(ns);
        for (uint32_t j = 0; j < ns; ++j) w.u64(to_u64(b.pImmutableSamplers[j]));
    }
    uint64_t a[8] = {to_u64(device)};
    return create_from_payload<VkDescriptorSetLayout, CallId::VkCreateDescriptorSetLayout>(a, in,
                                                                                            pLayout);
}

VKAPI_ATTR VkResult VKAPI_CALL stud_vkCreatePipelineLayout(
    VkDevice device, const VkPipelineLayoutCreateInfo* ci, const VkAllocationCallbacks*,
    VkPipelineLayout* pLayout) {
    if (ci == nullptr || pLayout == nullptr) return VK_ERROR_INITIALIZATION_FAILED;
    std::vector<uint8_t> in;
    vk_wire::Writer w(in);
    w.u32(ci->flags);
    w.u32(ci->setLayoutCount);
    for (uint32_t i = 0; i < ci->setLayoutCount; ++i) w.u64(to_u64(ci->pSetLayouts[i]));
    w.u32(ci->pushConstantRangeCount);
    for (uint32_t i = 0; i < ci->pushConstantRangeCount; ++i) {
        w.u32(ci->pPushConstantRanges[i].stageFlags);
        w.u32(ci->pPushConstantRanges[i].offset);
        w.u32(ci->pPushConstantRanges[i].size);
    }
    uint64_t a[8] = {to_u64(device)};
    return create_from_payload<VkPipelineLayout, CallId::VkCreatePipelineLayout>(a, in, pLayout);
}

VKAPI_ATTR VkResult VKAPI_CALL stud_vkCreateDescriptorPool(
    VkDevice device, const VkDescriptorPoolCreateInfo* ci, const VkAllocationCallbacks*,
    VkDescriptorPool* pPool) {
    if (ci == nullptr || pPool == nullptr) return VK_ERROR_INITIALIZATION_FAILED;
    std::vector<uint8_t> in;
    vk_wire::Writer w(in);
    w.u32(ci->flags);
    w.u32(ci->maxSets);
    w.u32(ci->poolSizeCount);
    for (uint32_t i = 0; i < ci->poolSizeCount; ++i) {
        w.u32(ci->pPoolSizes[i].type);
        w.u32(ci->pPoolSizes[i].descriptorCount);
    }
    uint64_t a[8] = {to_u64(device)};
    return create_from_payload<VkDescriptorPool, CallId::VkCreateDescriptorPool>(a, in, pPool);
}

VKAPI_ATTR VkResult VKAPI_CALL stud_vkAllocateDescriptorSets(
    VkDevice device, const VkDescriptorSetAllocateInfo* ai, VkDescriptorSet* pSets) {
    if (ai == nullptr || pSets == nullptr) return VK_ERROR_INITIALIZATION_FAILED;
    std::vector<uint8_t> in;
    vk_wire::Writer w(in);
    w.u64(to_u64(ai->descriptorPool));
    w.u32(ai->descriptorSetCount);
    for (uint32_t i = 0; i < ai->descriptorSetCount; ++i) w.u64(to_u64(ai->pSetLayouts[i]));

    // Name the sets here instead of waiting to be told what they are
    // called. A descriptor set is opaque to the application; it only
    // ever hands the handle back, so the only thing a returned handle
    // buys is a synchronous round trip, and the engine allocates 738 sets
    // a frame in a real game. The host keeps id -> real handle and
    // translates at the two places a set is used.
    //
    // The tag makes an id impossible to mistake for a real handle, so a
    // lookup miss on the host is a bug and not an ambiguity.
    static std::atomic<uint64_t> next_id{1};
    constexpr uint64_t kTag = 0x5D00000000000000ull;
    for (uint32_t i = 0; i < ai->descriptorSetCount; ++i) {
        const uint64_t id = kTag | next_id.fetch_add(1, std::memory_order_relaxed);
        w.u64(id);
        pSets[i] = from_u64<VkDescriptorSet>(id);
    }

    uint64_t a[8] = {to_u64(device)};
    stud::render_client::connection().call_void(CallId::VkAllocateDescriptorSets, a, in.data(),
                                                 static_cast<uint32_t>(in.size()));
    return VK_SUCCESS;
}

VKAPI_ATTR VkResult VKAPI_CALL stud_vkResetDescriptorPool(VkDevice device, VkDescriptorPool pool,
                                                           VkDescriptorPoolResetFlags flags) {
    // Once per frame, measured, and nothing reads the answer: it can only
    // fail by running out of memory, and the descriptor sets it frees are
    // the host's own bookkeeping. Ordered against everything else by being
    // in the same queue, which is what makes this safe now and was not
    // before there was a single writer.
    uint64_t a[8] = {to_u64(device), to_u64(pool), flags};
    if (!sync_submit()) {
        stud::render_client::connection().call_void(CallId::VkResetDescriptorPool, a);
        return VK_SUCCESS;
    }
    uint64_t r = stud::render_client::connection().call(CallId::VkResetDescriptorPool, a, nullptr,
                                                         0, nullptr, 0, nullptr);
    return static_cast<VkResult>(static_cast<int32_t>(r));
}

VKAPI_ATTR VkResult VKAPI_CALL stud_vkCreateDescriptorUpdateTemplate(
    VkDevice device, const VkDescriptorUpdateTemplateCreateInfo* ci, const VkAllocationCallbacks*,
    VkDescriptorUpdateTemplate* pTemplate) {
    if (ci == nullptr || pTemplate == nullptr) return VK_ERROR_INITIALIZATION_FAILED;
    std::vector<uint8_t> in;
    vk_wire::Writer w(in);
    w.u32(ci->flags);
    w.u32(ci->descriptorUpdateEntryCount);
    uint64_t blob_size = 0;
    for (uint32_t i = 0; i < ci->descriptorUpdateEntryCount; ++i) {
        const auto& e = ci->pDescriptorUpdateEntries[i];
        w.u32(e.dstBinding);
        w.u32(e.dstArrayElement);
        w.u32(e.descriptorCount);
        w.u32(e.descriptorType);
        w.u64(e.offset);
        w.u64(e.stride);
        // offset + stride*count is WRONG and crashed the driver: Vulkan
        // permits stride == 0 when descriptorCount is 1, which made the
        // computed size stop at `offset` and sent fewer bytes than the
        // driver then read, walking off the end of the buffer.
        //
        // The last descriptor starts at offset + stride*(count-1) and is
        // itself a whole struct. kMaxDescriptorSize is the largest of the
        // three (VkDescriptorImageInfo and VkDescriptorBufferInfo are both
        // 24 bytes; VkBufferView is 8), so this is an upper bound rather
        // than a per-type guess, sending a few spare bytes is harmless,
        // sending too few is a crash.
        constexpr uint64_t kMaxDescriptorSize = 24;
        if (e.descriptorCount == 0) continue;
        const uint64_t end = e.offset +
                             static_cast<uint64_t>(e.stride) * (e.descriptorCount - 1) +
                             kMaxDescriptorSize;
        if (end > blob_size) blob_size = end;
    }
    w.u32(ci->templateType);
    w.u64(to_u64(ci->descriptorSetLayout));
    w.u32(ci->pipelineBindPoint);
    w.u64(to_u64(ci->pipelineLayout));
    w.u32(ci->set);

    uint64_t a[8] = {to_u64(device)};
    VkResult res = create_from_payload<VkDescriptorUpdateTemplate,
                                        CallId::VkCreateDescriptorUpdateTemplate>(a, in, pTemplate);
    if (res == VK_SUCCESS) {
        // Remember how big a payload this template's updates carry: the
        // caller passes a bare pointer with no length, so the size has to
        // come from the entries that defined the layout.
        template_blob_size()[to_u64(*pTemplate)] = blob_size;
    }
    return res;
}

VKAPI_ATTR void VKAPI_CALL stud_vkUpdateDescriptorSetWithTemplate(
    VkDevice device, VkDescriptorSet set, VkDescriptorUpdateTemplate tmpl, const void* pData) {
    if (pData == nullptr) return;
    auto it = template_blob_size().find(to_u64(tmpl));
    if (it == template_blob_size().end()) return;
    uint64_t a[8] = {to_u64(device), to_u64(set), to_u64(tmpl)};
    // Reply-free: this returns nothing, so blocking for an answer bought
    // nothing but latency. Measured in a real game, it was 771 calls a
    // frame and the second-largest single cost in the whole frame.
    // Ordering is unaffected, the stream stays ordered, and anything
    // that does need an answer flushes what is queued ahead of it first.
    stud::render_client::connection().call_void(CallId::VkUpdateDescriptorSetWithTemplate, a,
                                                 pData, static_cast<uint32_t>(it->second));
}

VKAPI_ATTR VkResult VKAPI_CALL stud_vkCreateGraphicsPipelines(
    VkDevice device, VkPipelineCache cache, uint32_t createInfoCount,
    const VkGraphicsPipelineCreateInfo* pCreateInfos, const VkAllocationCallbacks*,
    VkPipeline* pPipelines) {
    if (pCreateInfos == nullptr || pPipelines == nullptr) return VK_ERROR_INITIALIZATION_FAILED;
    // One per call: the host creates one at a time, so a batch is just a
    // loop here rather than a second wire format.
    for (uint32_t p = 0; p < createInfoCount; ++p) {
        const VkGraphicsPipelineCreateInfo& ci = pCreateInfos[p];
        std::vector<uint8_t> in;
        vk_wire::Writer w(in);
        w.u64(to_u64(cache));
        w.u32(ci.flags);

        w.u32(ci.stageCount);
        for (uint32_t i = 0; i < ci.stageCount; ++i) {
            const auto& st = ci.pStages[i];
            report_dropped_specialization(st.pSpecializationInfo, "vkCreateGraphicsPipelines");
            w.u32(st.flags);
            w.u32(st.stage);
            w.u64(to_u64(st.module));
            const char* name = st.pName != nullptr ? st.pName : "main";
            w.bytes(name, std::strlen(name));
        }

        const auto* vi = ci.pVertexInputState;
        w.u32(vi != nullptr ? vi->vertexBindingDescriptionCount : 0);
        if (vi != nullptr) {
            for (uint32_t i = 0; i < vi->vertexBindingDescriptionCount; ++i) {
                w.u32(vi->pVertexBindingDescriptions[i].binding);
                w.u32(vi->pVertexBindingDescriptions[i].stride);
                w.u32(vi->pVertexBindingDescriptions[i].inputRate);
            }
        }
        w.u32(vi != nullptr ? vi->vertexAttributeDescriptionCount : 0);
        if (vi != nullptr) {
            for (uint32_t i = 0; i < vi->vertexAttributeDescriptionCount; ++i) {
                w.u32(vi->pVertexAttributeDescriptions[i].location);
                w.u32(vi->pVertexAttributeDescriptions[i].binding);
                w.u32(vi->pVertexAttributeDescriptions[i].format);
                w.u32(vi->pVertexAttributeDescriptions[i].offset);
            }
        }

        const auto* ia = ci.pInputAssemblyState;
        w.u32(ia != nullptr ? ia->topology : 0);
        w.u32(ia != nullptr ? ia->primitiveRestartEnable : 0);

        // The viewport/scissor VALUES, not just their counts.
        //
        // These pointers are only ignored when the pipeline declares
        // VK_DYNAMIC_STATE_VIEWPORT / _SCISSOR; otherwise they are
        // required and the pipeline bakes them in. Sending only the
        // counts left the host building pipelines with null pointers, so
        // every draw was clipped away and the frame came back black,
        // while every call still reported success.
        const auto* vp = ci.pViewportState;
        const uint32_t viewport_count = vp != nullptr ? vp->viewportCount : 0;
        const uint32_t scissor_count = vp != nullptr ? vp->scissorCount : 0;
        w.u32(viewport_count);
        w.u32(vp != nullptr && vp->pViewports != nullptr ? viewport_count : 0);
        if (vp != nullptr && vp->pViewports != nullptr) {
            for (uint32_t i = 0; i < viewport_count; ++i) {
                w.f32(vp->pViewports[i].x);
                w.f32(vp->pViewports[i].y);
                w.f32(vp->pViewports[i].width);
                w.f32(vp->pViewports[i].height);
                w.f32(vp->pViewports[i].minDepth);
                w.f32(vp->pViewports[i].maxDepth);
            }
        }
        w.u32(scissor_count);
        w.u32(vp != nullptr && vp->pScissors != nullptr ? scissor_count : 0);
        if (vp != nullptr && vp->pScissors != nullptr) {
            for (uint32_t i = 0; i < scissor_count; ++i) {
                w.u32(static_cast<uint32_t>(vp->pScissors[i].offset.x));
                w.u32(static_cast<uint32_t>(vp->pScissors[i].offset.y));
                w.u32(vp->pScissors[i].extent.width);
                w.u32(vp->pScissors[i].extent.height);
            }
        }

        const auto* rs = ci.pRasterizationState;
        w.u32(rs != nullptr ? rs->depthClampEnable : 0);
        w.u32(rs != nullptr ? rs->rasterizerDiscardEnable : 0);
        w.u32(rs != nullptr ? rs->polygonMode : 0);
        w.u32(rs != nullptr ? rs->cullMode : 0);
        w.u32(rs != nullptr ? rs->frontFace : 0);
        w.u32(rs != nullptr ? rs->depthBiasEnable : 0);
        w.f32(rs != nullptr ? rs->depthBiasConstantFactor : 0.0f);
        w.f32(rs != nullptr ? rs->depthBiasClamp : 0.0f);
        w.f32(rs != nullptr ? rs->depthBiasSlopeFactor : 0.0f);
        w.f32(rs != nullptr ? rs->lineWidth : 1.0f);

        const auto* ms = ci.pMultisampleState;
        w.u32(ms != nullptr ? ms->rasterizationSamples : VK_SAMPLE_COUNT_1_BIT);
        w.u32(ms != nullptr ? ms->sampleShadingEnable : 0);
        w.f32(ms != nullptr ? ms->minSampleShading : 0.0f);
        const bool has_mask = ms != nullptr && ms->pSampleMask != nullptr;
        w.u32(has_mask ? 1 : 0);
        w.u32(has_mask ? ms->pSampleMask[0] : 0xFFFFFFFFu);
        w.u32(ms != nullptr ? ms->alphaToCoverageEnable : 0);
        w.u32(ms != nullptr ? ms->alphaToOneEnable : 0);

        const auto* ds = ci.pDepthStencilState;
        w.u32(ds != nullptr ? 1 : 0);
        if (ds != nullptr) {
            w.u32(ds->depthTestEnable);
            w.u32(ds->depthWriteEnable);
            w.u32(ds->depthCompareOp);
            w.u32(ds->depthBoundsTestEnable);
            w.u32(ds->stencilTestEnable);
            auto write_stencil = [&w](const VkStencilOpState& s) {
                w.u32(s.failOp);
                w.u32(s.passOp);
                w.u32(s.depthFailOp);
                w.u32(s.compareOp);
                w.u32(s.compareMask);
                w.u32(s.writeMask);
                w.u32(s.reference);
            };
            write_stencil(ds->front);
            write_stencil(ds->back);
            w.f32(ds->minDepthBounds);
            w.f32(ds->maxDepthBounds);
        }

        const auto* cb = ci.pColorBlendState;
        w.u32(cb != nullptr ? 1 : 0);
        if (cb != nullptr) {
            w.u32(cb->logicOpEnable);
            w.u32(cb->logicOp);
            w.u32(cb->attachmentCount);
            for (uint32_t i = 0; i < cb->attachmentCount; ++i) {
                const auto& b = cb->pAttachments[i];
                w.u32(b.blendEnable);
                w.u32(b.srcColorBlendFactor);
                w.u32(b.dstColorBlendFactor);
                w.u32(b.colorBlendOp);
                w.u32(b.srcAlphaBlendFactor);
                w.u32(b.dstAlphaBlendFactor);
                w.u32(b.alphaBlendOp);
                w.u32(b.colorWriteMask);
            }
            for (int i = 0; i < 4; ++i) w.f32(cb->blendConstants[i]);
        }

        const auto* dyn = ci.pDynamicState;
        w.u32(dyn != nullptr ? dyn->dynamicStateCount : 0);
        if (dyn != nullptr) {
            for (uint32_t i = 0; i < dyn->dynamicStateCount; ++i) w.u32(dyn->pDynamicStates[i]);
        }

        w.u64(to_u64(ci.layout));
        w.u64(to_u64(ci.renderPass));
        w.u32(ci.subpass);

        uint64_t a[8] = {to_u64(device)};
        VkResult res = create_from_payload<VkPipeline, CallId::VkCreateGraphicsPipelines>(
            a, in, &pPipelines[p]);
        if (res != VK_SUCCESS) return res;
    }
    return VK_SUCCESS;
}

VKAPI_ATTR VkResult VKAPI_CALL stud_vkCreateComputePipelines(
    VkDevice device, VkPipelineCache cache, uint32_t createInfoCount,
    const VkComputePipelineCreateInfo* pCreateInfos, const VkAllocationCallbacks*,
    VkPipeline* pPipelines) {
    if (pCreateInfos == nullptr || pPipelines == nullptr) return VK_ERROR_INITIALIZATION_FAILED;
    for (uint32_t p = 0; p < createInfoCount; ++p) {
        const VkComputePipelineCreateInfo& ci = pCreateInfos[p];
        // Specialization constants are NOT carried over the wire, so if
        // the engine ever sends any, every shader here compiles with the
        // defaults instead -- which silently changes workgroup sizes,
        // loop bounds and which branches survive. Whether the engine
        // uses them at all was unknown, and guessing either way is how a
        // "cleanup" breaks a renderer, so it says so instead.
        report_dropped_specialization(ci.stage.pSpecializationInfo, "vkCreateComputePipelines");
        std::vector<uint8_t> in;
        vk_wire::Writer w(in);
        w.u64(to_u64(cache));
        w.u32(ci.flags);
        w.u32(ci.stage.flags);
        w.u32(ci.stage.stage);
        w.u64(to_u64(ci.stage.module));
        const char* name = ci.stage.pName != nullptr ? ci.stage.pName : "main";
        w.bytes(name, std::strlen(name));
        w.u64(to_u64(ci.layout));
        uint64_t a[8] = {to_u64(device)};
        VkResult res = create_from_payload<VkPipeline, CallId::VkCreateComputePipelines>(
            a, in, &pPipelines[p]);
        if (res != VK_SUCCESS) return res;
    }
    return VK_SUCCESS;
}

// ---- command buffers and submission ----------------------------------

VKAPI_ATTR VkResult VKAPI_CALL stud_vkAllocateCommandBuffers(
    VkDevice device, const VkCommandBufferAllocateInfo* ai, VkCommandBuffer* pBuffers) {
    if (ai == nullptr || pBuffers == nullptr) return VK_ERROR_INITIALIZATION_FAILED;
    uint64_t a[8] = {to_u64(device), to_u64(ai->commandPool),
                     static_cast<uint64_t>(ai->level), ai->commandBufferCount};
    std::vector<uint8_t> out(sizeof(uint64_t) * ai->commandBufferCount);
    uint32_t written = 0;
    uint64_t r = stud::render_client::connection().call(CallId::VkAllocateCommandBuffers, a,
                                                         nullptr, 0, out.data(),
                                                         static_cast<uint32_t>(out.size()),
                                                         &written);
    VkResult result = static_cast<VkResult>(static_cast<int32_t>(r));
    if (result != VK_SUCCESS) return result;
    const uint32_t n = written / sizeof(uint64_t);
    for (uint32_t i = 0; i < n && i < ai->commandBufferCount; ++i) {
        uint64_t h = 0;
        std::memcpy(&h, out.data() + i * sizeof(h), sizeof(h));
        pBuffers[i] = from_u64<VkCommandBuffer>(h);
    }
    return VK_SUCCESS;
}

// Whether bracketing a command buffer still costs a blocking round trip.
//
// It does not need to. Both calls return only a VkResult, both of whose
// failures are out-of-memory, and the engine records MANY command buffers
// in a frame on several threads at once -- so this was two blocking round
// trips per command buffer per frame, each one also forcing the recorded-
// command batch out early and holding every other recording thread on the
// connection's own mutex for the length of a socket round trip. Queued
// reply-free they keep their place in the same ordered stream and cost
// the caller a memcpy.
//
// ---- The ordering this relies on, which is easy to break
//
// Reply-free means these now travel in the connection's queue rather than
// being waited for. Against the engine's own recorded commands that is
// exact: same queue, same mutex, program order, and call_void() emits any
// pending command batch before appending, so a Begin can never land ahead
// of commands recorded before it.
//
// Against a SUBMIT it is not, and that is the trap. vkQueueSubmit is
// handed to the deferred thread, so a submit naming this command buffer
// may still be sitting there unsent. What stops a Begin overtaking it is
// that the engine resets the pool (or the fence) before re-recording, and
// vkResetCommandPool/vkResetFences drain the deferred queue first. That
// drain is the ordering guarantee this call rides on.
//
// So: do NOT make those resets reply-free as well without replacing that
// guarantee with another one. Tried together, the pair reorders a Begin
// ahead of the submit that still has to read the same command buffer,
// which is a GPU executing whatever is in it now. Separately, this one is
// safe, because the drain still happens between the submit and the next
// Begin.
//
// STUD_VK_SYNC_CMDBUF=1 puts both back to blocking, as the A/B for any
// "did this change what the engine sees" question.
bool sync_command_buffer_calls() {
    static const bool on = std::getenv("STUD_VK_SYNC_CMDBUF") != nullptr;
    return on;
}

VKAPI_ATTR VkResult VKAPI_CALL stud_vkBeginCommandBuffer(VkCommandBuffer cb,
                                                          const VkCommandBufferBeginInfo* bi) {
    {
        // Recording this buffer again means its previous submission is
        // done, so its decode scratch can be handed out from the start.
        std::lock_guard<std::recursive_mutex> lock(emulation_mutex());
        auto it = scratch_pools().find(to_u64(cb));
        if (it != scratch_pools().end()) it->second.used = 0;
        // And anything recorded into it that was never submitted is gone
        // with the re-record, so it must not be collected later.
        auto& recorded = recorded_readbacks();
        recorded.erase(std::remove_if(recorded.begin(), recorded.end(),
                                      [cb](const PendingReadback& rb) { return rb.cb == cb; }),
                       recorded.end());
    }
    uint64_t a[8] = {to_u64(cb), bi != nullptr ? bi->flags : 0};
    if (!sync_command_buffer_calls()) {
        stud::render_client::connection().call_void(CallId::VkBeginCommandBuffer, a);
        return VK_SUCCESS;
    }
    uint64_t r = stud::render_client::connection().call(CallId::VkBeginCommandBuffer, a, nullptr, 0,
                                                         nullptr, 0, nullptr);
    return static_cast<VkResult>(static_cast<int32_t>(r));
}

VKAPI_ATTR VkResult VKAPI_CALL stud_vkEndCommandBuffer(VkCommandBuffer cb) {
    uint64_t a[8] = {to_u64(cb)};
    if (!sync_command_buffer_calls()) {
        stud::render_client::connection().call_void(CallId::VkEndCommandBuffer, a);
        return VK_SUCCESS;
    }
    uint64_t r = stud::render_client::connection().call(CallId::VkEndCommandBuffer, a, nullptr, 0,
                                                         nullptr, 0, nullptr);
    return static_cast<VkResult>(static_cast<int32_t>(r));
}

VKAPI_ATTR VkResult VKAPI_CALL stud_vkResetCommandPool(VkDevice device, VkCommandPool pool,
                                                        VkCommandPoolResetFlags flags) {
    // Recycling command buffers has to happen after the submits that use
    // them, or the host resets a command buffer a queued submit is still
    // going to execute, which is a GPU running whatever is there now.
    //
    // That used to need a drain, because submits were produced by a second
    // thread and this one could overtake them. With a single writer the
    // submit is already ahead of this in the same queue -- the engine
    // issued it first -- so the ordering is program order and costs
    // nothing. Reply-free for the same reason: nothing reads the answer.
    uint64_t a[8] = {to_u64(device), to_u64(pool), flags};
    if (!sync_submit()) {
        stud::render_client::connection().call_void(CallId::VkResetCommandPool, a);
        return VK_SUCCESS;
    }
    uint64_t r = stud::render_client::connection().call(CallId::VkResetCommandPool, a, nullptr, 0,
                                                         nullptr, 0, nullptr);
    return static_cast<VkResult>(static_cast<int32_t>(r));
}

// Ships every live mapping's contents to the host.
//
// This is what makes HOST_COHERENT memory work across the process
// boundary. Coherent memory exists precisely so an application does NOT
// have to call vkFlushMappedMemoryRanges, writes are visible to the
// device as soon as they are made. Stud's mapping is a local staging
// buffer in another process, so "as soon as they are made" is never:
// without this, an engine that maps coherent memory, writes its vertex
// and uniform data, and submits, sends nothing but the zeros the staging
// buffer started with. Every draw then reads zeroed vertices and covers
// no pixels, which is exactly a black frame with a healthy-looking
// command stream.
//
// Flushing at submit is the honest point: it is the moment the spec
// requires prior host writes to be visible to the device.
void flush_all_mapped_memory() {
    std::lock_guard<std::recursive_mutex> lock(mapped_mutex());
    for (auto& kv : mapped_ranges()) {
        // Non-coherent memory is the engine's own to flush, and it does
        // so through vkFlushMappedMemoryRanges, which Stud implements.
        // Scanning it here as well is a page-table rewrite and a
        // cross-core TLB shootdown that nothing is waiting on. See
        // non_coherent_memory_types() for why that is expensive enough
        // to be worth skipping, and why "unknown" still gets scanned.
        if (non_coherent_allocations().count(kv.first) != 0) {
            static bool said = false;
            if (!said) {
                said = true;
                std::printf("stud: vulkan-client: skipping non-coherent mappings in the "
                            "per-submit flush; the engine flushes those itself\n");
                std::fflush(stdout);
            }
            continue;
        }
        push_mapped_bytes(kv.first, 0, VK_WHOLE_SIZE);
    }
    static const bool stats = std::getenv("STUD_VK_MEM_STATS") != nullptr;
    if (stats) {
        static int submits = 0;
        if ((++submits % 120) == 0) {
            // Which mechanism produced these numbers is part of the
            // measurement: the fault barrier counts signals, uffd-scan
            // counts ioctls, and the two are being compared on traffic.
            if (stud::render_client::UffdScan::available()) {
                std::printf(
                    "stud: vk mapped flush: %d submits, %llu KB sent (%llu KB shared) of %llu KB asked, %d shared maps, %d copied maps, "
                    "%llu scans reporting %llu pages in %.1fms total (max %.3fms, %llu over 1ms) "
                    "(uffd-scan)\n",
                    submits, (unsigned long long)(g_mapped_bytes_sent / 1024),
                    (unsigned long long)(g_mapped_bytes_shared / 1024),
                    (unsigned long long)(g_mapped_bytes_asked / 1024), g_shared_maps,
                    g_copied_maps, stud::render_client::UffdScan::scan_count(),
                    stud::render_client::UffdScan::pages_reported(),
                    stud::render_client::UffdScan::scan_time_ns() / 1e6,
                    stud::render_client::UffdScan::scan_time_max_ns() / 1e6,
                    stud::render_client::UffdScan::slow_scans());
            } else {
                std::printf(
                    "stud: vk mapped flush: %d submits, %llu KB sent (%llu KB shared) of %llu KB asked, %d shared maps, %d copied maps, "
                    "%llu write faults\n",
                    submits, (unsigned long long)(g_mapped_bytes_sent / 1024),
                    (unsigned long long)(g_mapped_bytes_shared / 1024),
                    (unsigned long long)(g_mapped_bytes_asked / 1024), g_shared_maps,
                    g_copied_maps, stud::render_client::barrier_fault_count());
            }
            std::fflush(stdout);
        }
    }
}

namespace {

// Real Vulkan's vkQueueSubmit does not block: it hands the work to the
// driver and returns. Stud's made a synchronous IPC round-trip and, when
// a texture upload was in flight, first decoded the compressed texels,
// measured at 15ms on the engine's own render thread during a streaming
// burst, which is a dropped frame the engine would never have had on a
// device that samples ETC2 in hardware.
//
// So the ordered tail of a submit: the decode, then the IPC call,
// runs on one background thread, and everything that could observe it
// flushes that thread first: acquiring the next image, waiting on a
// fence, waiting for the device to go idle, and tearing a swapchain
// down. One thread, not a pool, because queue submission is ordered.
//
// What this deliberately does NOT move is flush_all_mapped_memory().
// That has to stay in program order with the engine's own writes to its
// mapped memory: running it later would ship whatever the engine had
// written by then, which is the next frame's data for an earlier submit.
// Submit and present are queued reply-free on the render connection's own
// single writer, and the texture decode rides the same queue as an ordered
// action just ahead of the submit that reads it.
//
// This replaced a second thread (DeferredQueue) that ran submit and
// present itself. It kept the 15ms decode off the engine's thread, which
// was its whole point and is preserved here, but it also made the stream
// have two producers, so anything needing to be ordered after a submit
// could only get that by draining it. Measured in a real game, the
// engine's Main thread spent 13.1% of its wall clock in exactly that
// drain. With one writer the ordering is program order and there is
// nothing to drain. See Client::start_writer/enqueue_action.
//
// What still does NOT move is flush_all_mapped_memory(): it has to stay in
// program order with the engine's own writes to its mapped memory, so it
// runs on the engine's thread, before the submit is queued.


// STUD_VK_SYNC_SUBMIT=1 puts submit and present back to blocking round
// trips on the calling thread. The A/B for anything that looks like an
// ordering or a "did the frame really land" question.
bool sync_submit() {
    static const bool on = std::getenv("STUD_VK_SYNC_SUBMIT") != nullptr;
    return on;
}

}  // namespace

VKAPI_ATTR VkResult VKAPI_CALL stud_vkQueueSubmit(VkQueue queue, uint32_t submitCount,
                                                   const VkSubmitInfo* pSubmits, VkFence fence) {
    const auto submit_t0 = std::chrono::steady_clock::now();
    std::vector<VkCommandBuffer> submitted;
    for (uint32_t i = 0; i < submitCount; ++i) {
        for (uint32_t j = 0; j < pSubmits[i].commandBufferCount; ++j) {
            submitted.push_back(pSubmits[i].pCommandBuffers[j]);
        }
    }
    // Stays here, on the engine's own thread: this has to keep program
    // order with the engine's writes to its mapped memory.
    flush_all_mapped_memory();
    // Any readback recorded into these command buffers is now really
    // going to run, so it becomes something to go and collect.
    arm_pending_readbacks(submitted);
    std::vector<uint8_t> in;
    vk_wire::Writer w(in);
    w.u32(submitCount);
    for (uint32_t i = 0; i < submitCount; ++i) {
        const auto& s = pSubmits[i];
        w.u32(s.waitSemaphoreCount);
        for (uint32_t j = 0; j < s.waitSemaphoreCount; ++j) w.u64(to_u64(s.pWaitSemaphores[j]));
        for (uint32_t j = 0; j < s.waitSemaphoreCount; ++j) {
            w.u32(s.pWaitDstStageMask != nullptr ? s.pWaitDstStageMask[j] : 0);
        }
        w.u32(s.commandBufferCount);
        for (uint32_t j = 0; j < s.commandBufferCount; ++j) w.u64(to_u64(s.pCommandBuffers[j]));
        w.u32(s.signalSemaphoreCount);
        for (uint32_t j = 0; j < s.signalSemaphoreCount; ++j) {
            w.u64(to_u64(s.pSignalSemaphores[j]));
        }
    }
    uint64_t a[8] = {to_u64(queue), to_u64(fence)};
    uint64_t r = VK_SUCCESS;
    if (sync_submit()) {
        run_pending_decodes(submitted);
        r = stud::render_client::connection().call(CallId::VkQueueSubmit, a, in.data(),
                                                    static_cast<uint32_t>(in.size()), nullptr, 0,
                                                    nullptr);
    } else {
        // The decode goes into the stream just ahead of the submit that
        // reads what it produces, so it is ordered against that submit
        // without the engine's thread waiting for it. On the writer
        // thread, not this one: it costs 15ms during a streaming burst
        // and this is the engine's own render thread.
        stud::render_client::connection().enqueue_action(
            [submitted] { run_pending_decodes(submitted); });
        // Reply-free. Real Vulkan's vkQueueSubmit does not block either,
        // and the result is reported on the next submit instead, a frame
        // late, which is honest as long as it is never dropped.
        stud::render_client::connection().call_void(CallId::VkQueueSubmit, a, in.data(),
                                                     static_cast<uint32_t>(in.size()));
        // Nothing can report this back: the request carries no reply, so
        // there is no channel for one. Said plainly rather than dressed up
        // as a real answer -- render-host names any non-success of a
        // reply-free call in its own output (report_reply_free_failure),
        // which is the only place it can be seen.
        r = VK_SUCCESS;
    }
    if (frame_timing_enabled()) {
        const double submit_before = frame_timing().submit_ms;
        frame_timing().submit_ms +=
            std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - submit_t0)
                .count();
        frame_timing().f_submit += frame_timing().submit_ms - submit_before;
    }
    return static_cast<VkResult>(static_cast<int32_t>(r));
}

VKAPI_ATTR VkResult VKAPI_CALL stud_vkWaitForFences(VkDevice device, uint32_t fenceCount,
                                                     const VkFence* pFences, VkBool32 waitAll,
                                                     uint64_t timeout) {
    // A fence cannot signal before the submit that would signal it has
    // actually been sent.
    std::vector<uint8_t> in;
    vk_wire::Writer w(in);
    w.u32(fenceCount);
    for (uint32_t i = 0; i < fenceCount; ++i) w.u64(to_u64(pFences[i]));
    uint64_t a[8] = {to_u64(device), waitAll, timeout};
    const auto t0 = std::chrono::steady_clock::now();
    uint64_t r = stud::render_client::connection().call(CallId::VkWaitForFences, a, in.data(),
                                                         static_cast<uint32_t>(in.size()), nullptr,
                                                         0, nullptr);
    if (frame_timing_enabled()) {
        frame_timing().fence_ms +=
            std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0)
                .count();
    }
    // The engine waited because it is about to look at what the submit
    // produced. If any of that was a readback into memory the host does
    // not share, this is the moment it has to be real.
    if (static_cast<VkResult>(static_cast<int32_t>(r)) == VK_SUCCESS) fetch_pending_readbacks();
    return static_cast<VkResult>(static_cast<int32_t>(r));
}

VKAPI_ATTR VkResult VKAPI_CALL stud_vkResetFences(VkDevice device, uint32_t fenceCount,
                                                   const VkFence* pFences) {
    // A reset must not overtake a submit that signals the same fence.
    //
    // It used to be able to. Submits were produced by a second thread, so
    // a reset sent straight to the host could arrive while the submit that
    // signals that fence was still queued there; the submit then landed
    // and made the fence pending again, and the engine's next submit
    // reused a fence still in use, which Vulkan does not allow and which
    // ends with the fence never signalling for anyone waiting on it.
    // Live-caught in a physics-heavy experience: the same fence handle
    // submitted twelve times with a wait outstanding.
    //
    // One writer removes the race outright rather than paying a drain to
    // dodge it. The engine calls vkQueueSubmit before vkResetFences, so
    // the submit is ahead of this in the queue, and the queue is the only
    // route to the host.
    std::vector<uint8_t> in;
    vk_wire::Writer w(in);
    w.u32(fenceCount);
    for (uint32_t i = 0; i < fenceCount; ++i) w.u64(to_u64(pFences[i]));
    uint64_t a[8] = {to_u64(device)};
    if (!sync_submit()) {
        stud::render_client::connection().call_void(CallId::VkResetFences, a, in.data(),
                                                     static_cast<uint32_t>(in.size()));
        return VK_SUCCESS;
    }
    uint64_t r = stud::render_client::connection().call(CallId::VkResetFences, a, in.data(),
                                                         static_cast<uint32_t>(in.size()), nullptr,
                                                         0, nullptr);
    return static_cast<VkResult>(static_cast<int32_t>(r));
}

// VK_GOOGLE_display_timing, answered from the compositor.
//
// The engine resolves vkGetRefreshCycleDurationGOOGLE on every launch.
// It is how an Android client learns how long a display refresh actually
// takes, and therefore how fast it may present. Stud IS the window-system
// integration here (render-host owns the real surface), so implementing
// this is not a lie about the driver: it is the one layer that genuinely
// knows, and it answers with the rate the compositor reports for the
// display the window is on.
//
// Returning null for it, which is what a desktop driver does, since
// this is an Android extension, is honest but leaves the engine with no
// answer at all.
VKAPI_ATTR VkResult VKAPI_CALL stud_vkGetRefreshCycleDurationGOOGLE(
    VkDevice /*device*/, VkSwapchainKHR /*swapchain*/,
    VkRefreshCycleDurationGOOGLE* pDisplayTimingProperties) {
    if (pDisplayTimingProperties == nullptr) return VK_ERROR_INITIALIZATION_FAILED;
    static const uint64_t refresh_ns = [] {
        uint64_t a[8] = {};
        const uint64_t mhz = stud::render_client::connection().call(
            CallId::GetDisplayRefreshRate, a, nullptr, 0, nullptr, 0, nullptr);
        if (mhz == 0) return static_cast<uint64_t>(0);
        // mHz -> nanoseconds per refresh.
        return static_cast<uint64_t>(1000000000000000.0 / static_cast<double>(mhz));
    }();
    if (refresh_ns == 0) return VK_ERROR_INITIALIZATION_FAILED;
    pDisplayTimingProperties->refreshDuration = refresh_ns;
    static bool announced = false;
    if (!announced) {
        announced = true;
        std::printf("stud: vulkan-client: refresh cycle reported as %llu ns (%.1f Hz)\n",
                    static_cast<unsigned long long>(refresh_ns), 1e9 / refresh_ns);
        std::fflush(stdout);
    }
    return VK_SUCCESS;
}

// The history half of the same extension. Stud keeps no per-present
// timing history, and an empty history is the honest answer, the spec
// allows reporting zero available timings.
VKAPI_ATTR VkResult VKAPI_CALL stud_vkGetPastPresentationTimingGOOGLE(
    VkDevice /*device*/, VkSwapchainKHR /*swapchain*/, uint32_t* pPresentationTimingCount,
    VkPastPresentationTimingGOOGLE* /*pPresentationTimings*/) {
    if (pPresentationTimingCount != nullptr) *pPresentationTimingCount = 0;
    return VK_SUCCESS;
}

VKAPI_ATTR VkResult VKAPI_CALL stud_vkAcquireNextImageKHR(VkDevice device,
                                                           VkSwapchainKHR swapchain,
                                                           uint64_t timeout,
                                                           VkSemaphore semaphore, VkFence fence,
                                                           uint32_t* pImageIndex) {
    if (pImageIndex == nullptr) return VK_ERROR_INITIALIZATION_FAILED;
    // Everything the deferred thread still owes, the previous frame's
    // submit and present, has to have reached the host before asking
    // which image is next.
    uint64_t a[8] = {to_u64(device), to_u64(swapchain), timeout, to_u64(semaphore),
                     to_u64(fence)};
    uint32_t index = 0;
    uint32_t written = 0;
    const auto t0 = std::chrono::steady_clock::now();
    uint64_t r = stud::render_client::connection().call(CallId::VkAcquireNextImageKHR, a, nullptr,
                                                         0, &index, sizeof(index), &written);
    if (frame_timing_enabled()) {
        const double acquire_before = frame_timing().acquire_ms;
        frame_timing().acquire_ms +=
            std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0)
                .count();
        frame_timing().f_acquire = frame_timing().acquire_ms - acquire_before;
    }
    if (written >= sizeof(index)) *pImageIndex = index;
    return static_cast<VkResult>(static_cast<int32_t>(r));
}

VKAPI_ATTR VkResult VKAPI_CALL stud_vkQueuePresentKHR(VkQueue queue,
                                                       const VkPresentInfoKHR* pi) {
    if (pi == nullptr) return VK_ERROR_INITIALIZATION_FAILED;
    std::vector<uint8_t> in;
    vk_wire::Writer w(in);
    w.u32(pi->waitSemaphoreCount);
    for (uint32_t i = 0; i < pi->waitSemaphoreCount; ++i) w.u64(to_u64(pi->pWaitSemaphores[i]));
    w.u32(pi->swapchainCount);
    for (uint32_t i = 0; i < pi->swapchainCount; ++i) w.u64(to_u64(pi->pSwapchains[i]));
    for (uint32_t i = 0; i < pi->swapchainCount; ++i) w.u32(pi->pImageIndices[i]);
    uint64_t a[8] = {to_u64(queue)};
    const auto t0 = std::chrono::steady_clock::now();
    uint64_t r = VK_SUCCESS;
    if (sync_submit()) {
        r = stud::render_client::connection().call(CallId::VkQueuePresentKHR, a, in.data(),
                                                    static_cast<uint32_t>(in.size()), nullptr, 0,
                                                    nullptr);
    } else {
        // Queued behind the submit it presents, by program order, which is
        // all "the same ordered thread" ever bought. Its result is
        // reported on the next present rather than this one, a frame late,
        // which is the price of not blocking the engine here.
        stud::render_client::connection().call_void(CallId::VkQueuePresentKHR, a, in.data(),
                                                     static_cast<uint32_t>(in.size()));
        r = VK_SUCCESS;  // as above: reply-free, so the host is the only reporter

    }
    if (frame_timing_enabled()) {
        const auto now = std::chrono::steady_clock::now();
        FrameTiming& t = frame_timing();
        const double present_ms = std::chrono::duration<double, std::milli>(now - t0).count();
        t.present_ms += present_ms;
        t.f_present += present_ms;
        if (t.last_present.time_since_epoch().count() != 0) {
            const double this_frame_ms =
                std::chrono::duration<double, std::milli>(now - t.last_present).count();
            t.frame_ms += this_frame_ms;
            if (this_frame_ms > t.worst_ms) t.worst_ms = this_frame_ms;
            if (this_frame_ms > 7.0) ++t.over_7ms;
            if (this_frame_ms > 16.7) ++t.over_16ms;
            ++t.frames;
            static const bool slow_frames = std::getenv("STUD_VK_SLOW_FRAMES") != nullptr;
            if (slow_frames && this_frame_ms > 16.7) {
                std::fprintf(stderr,
                             "stud: SLOW FRAME %.1fms, acquire %.2f submit %.2f present %.2f "
                             "record %.2f stub %.2f blocking %.2f decode %.2f, "
                             "unaccounted %.2f, %llu "
                             "cmds, %llu KB\n",
                             this_frame_ms, t.f_acquire, t.f_submit, t.f_present, t.f_record,
                             t.f_stub, t.f_blocking, t.f_decode,
                             this_frame_ms - t.f_acquire - t.f_submit - t.f_present - t.f_stub -
                                 t.f_blocking - t.f_decode,
                             static_cast<unsigned long long>(t.f_cmds),
                             static_cast<unsigned long long>(t.f_bytes / 1024));
            }
            t.f_acquire = t.f_submit = t.f_present = t.f_record = t.f_stub = 0;
            t.f_blocking = t.f_decode = 0;
            t.f_cmds = t.f_bytes = 0;
        }
        t.last_present = now;
        report_frame_timing();
    }
    return static_cast<VkResult>(static_cast<int32_t>(r));
}

VKAPI_ATTR VkResult VKAPI_CALL stud_vkGetQueryPoolResults(VkDevice device, VkQueryPool pool,
                                                           uint32_t firstQuery,
                                                           uint32_t queryCount, size_t dataSize,
                                                           void* pData, VkDeviceSize stride,
                                                           VkQueryResultFlags flags) {
    if (pData == nullptr) return VK_ERROR_INITIALIZATION_FAILED;
    uint64_t a[8] = {to_u64(device), to_u64(pool), firstQuery, queryCount,
                     static_cast<uint64_t>(stride), flags};
    std::vector<uint8_t> out(dataSize);
    uint32_t written = 0;
    const auto q0 = std::chrono::steady_clock::now();
    uint64_t r = stud::render_client::connection().call(CallId::VkGetQueryPoolResults, a, nullptr,
                                                         0, out.data(),
                                                         static_cast<uint32_t>(out.size()),
                                                         &written);
    if (frame_timing_enabled()) {
        frame_timing().query_ms +=
            std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - q0)
                .count();
    }
    if (written > 0) std::memcpy(pData, out.data(), written < dataSize ? written : dataSize);
    return static_cast<VkResult>(static_cast<int32_t>(r));
}

// ---- recording -------------------------------------------------------
//
// Every one of these returns void, so they all go out reply-free: the
// request is appended to the client's queue and sent in bulk, with no
// blocking round-trip. That is what makes Vulkan cheaper to forward than
// GLES was.
// One reusable wire buffer per thread, instead of a fresh heap allocation
// for every Vulkan call.
//
// Every command built here used to construct a std::vector<uint8_t>, grow
// it a field at a time, hand it over and free it, once per call, at
// hundreds of calls a frame. Profiling the render thread in a real game
// found the top costs were exactly that: vector construct/destroy,
// __split_buffer growth, memcpy, and scudo (bionic's allocator, which is
// hardened and not fast). None of it is the engine's work or the driver's.
//
// The buffer keeps its capacity between calls, so after the first few
// frames the per-call cost is the copy of the arguments and nothing else.
// Reentrancy is the one hazard: a function using this must not call
// another that also uses it before it is done, which is why this is used
// on the command-recording paths and the callers that were checked, not
// applied blindly everywhere.
// C++ linkage: it returns a C++ type, and clang warns about that inside an
// extern "C" block.
static std::vector<uint8_t>& wire_scratch() {
    thread_local std::vector<uint8_t> buffer;
    buffer.clear();
    return buffer;
}

void record_bytes(VkCommandBuffer cb, vk_wire::CmdKind kind, const void* in, size_t len);

// Times a whole command entry point, argument serialisation included.
struct StubScope {
    std::chrono::steady_clock::time_point t0;
    StubScope() {
        if (frame_timing_enabled()) t0 = std::chrono::steady_clock::now();
    }
    ~StubScope() {
        if (!frame_timing_enabled()) return;
        const double ms =
            std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0)
                .count();
        frame_timing().f_stub += ms;
        frame_timing().stub_ms += ms;
    }
};

void record(VkCommandBuffer cb, vk_wire::CmdKind kind, const std::vector<uint8_t>& in) {
    const auto rec_t0 = frame_timing_enabled() ? std::chrono::steady_clock::now()
                                                : std::chrono::steady_clock::time_point{};
    // STUD_VK_CMD_STATS=1 on the CLIENT side too: the host counts what it
    // executes, this counts what was sent. A gap between the two is work
    // the transport lost, which is exactly the question when the engine
    // records thousands of draws and the swapchain image comes back
    // empty.
    static const bool stats = std::getenv("STUD_VK_CMD_STATS") != nullptr;
    if (stats) {
        static std::map<uint32_t, uint64_t> counts;
        static int since = 0;
        ++counts[static_cast<uint32_t>(kind)];
        if (++since >= 2000) {
            since = 0;
            std::fprintf(stderr, "stud: vulkan-client: CLIENTCMDSTATS");
            for (const auto& kv : counts) {
                std::fprintf(stderr, " %u=%llu", kv.first,
                             static_cast<unsigned long long>(kv.second));
            }
            std::fprintf(stderr, "\n");
            std::fflush(stderr);
        }
    }
    if (frame_timing_enabled()) {
        ++frame_timing().cmds;
        frame_timing().cmd_bytes += in.size();
    }
    stud::render_client::connection().append_command(to_u64(cb), static_cast<uint32_t>(kind),
                                                      in.data(),
                                                      static_cast<uint32_t>(in.size()));
    if (frame_timing_enabled()) {
        const double ms =
            std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - rec_t0)
                .count();
        frame_timing().record_ms += ms;
        frame_timing().f_record += ms;
    }
}

// Same thing for a command already serialised into a stack buffer, which
// is how the hot ones get here: no vector, no allocator, one copy into the
// connection's queue.
void record_bytes(VkCommandBuffer cb, vk_wire::CmdKind kind, const void* in, size_t len) {
    const auto rec_t0 = frame_timing_enabled() ? std::chrono::steady_clock::now()
                                                : std::chrono::steady_clock::time_point{};
    if (frame_timing_enabled()) {
        ++frame_timing().cmds;
        ++frame_timing().f_cmds;
        frame_timing().cmd_bytes += len;
        frame_timing().f_bytes += len;
    }
    stud::render_client::connection().append_command(to_u64(cb), static_cast<uint32_t>(kind), in,
                                                      static_cast<uint32_t>(len));
    if (frame_timing_enabled()) {
        frame_timing().record_ms +=
            std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - rec_t0)
                .count();
    }
}

VKAPI_ATTR void VKAPI_CALL stud_vkCmdBeginRenderPass(VkCommandBuffer cb,
                                                      const VkRenderPassBeginInfo* bi,
                                                      VkSubpassContents contents) {
    if (bi == nullptr) return;
    StubScope stub_scope;
    vk_wire::FixedWriter<256> w;

    w.u64(to_u64(bi->renderPass));
    w.u64(to_u64(bi->framebuffer));
    w.u32(static_cast<uint32_t>(bi->renderArea.offset.x));
    w.u32(static_cast<uint32_t>(bi->renderArea.offset.y));
    w.u32(bi->renderArea.extent.width);
    w.u32(bi->renderArea.extent.height);
    w.u32(bi->clearValueCount);
    for (uint32_t i = 0; i < bi->clearValueCount; ++i) {
        // A VkClearValue is a union; the float view carries the bits
        // either way, and the host reads it back the same way.
        for (int j = 0; j < 4; ++j) w.f32(bi->pClearValues[i].color.float32[j]);
    }
    w.u32(contents);
    record_bytes(cb, vk_wire::CmdKind::BeginRenderPass, w.data(), w.size());
}

VKAPI_ATTR void VKAPI_CALL stud_vkCmdEndRenderPass(VkCommandBuffer cb) {
    record(cb, vk_wire::CmdKind::EndRenderPass, {});
}

VKAPI_ATTR void VKAPI_CALL stud_vkCmdBindPipeline(VkCommandBuffer cb, VkPipelineBindPoint bp,
                                                   VkPipeline pipeline) {
    StubScope stub_scope;
    vk_wire::FixedWriter<256> w;

    w.u32(bp);
    w.u64(to_u64(pipeline));
    record_bytes(cb, vk_wire::CmdKind::BindPipeline, w.data(), w.size());
}

VKAPI_ATTR void VKAPI_CALL stud_vkCmdBindDescriptorSets(
    VkCommandBuffer cb, VkPipelineBindPoint bp, VkPipelineLayout layout, uint32_t firstSet,
    uint32_t setCount, const VkDescriptorSet* pSets, uint32_t dynCount,
    const uint32_t* pDynOffsets) {
    StubScope stub_scope;
    vk_wire::FixedWriter<256> w;

    w.u32(bp);
    w.u64(to_u64(layout));
    w.u32(firstSet);
    w.u32(setCount);
    for (uint32_t i = 0; i < setCount; ++i) w.u64(to_u64(pSets[i]));
    w.u32(dynCount);
    for (uint32_t i = 0; i < dynCount; ++i) w.u32(pDynOffsets[i]);
    record_bytes(cb, vk_wire::CmdKind::BindDescriptorSets, w.data(), w.size());
}

VKAPI_ATTR void VKAPI_CALL stud_vkCmdBindVertexBuffers(VkCommandBuffer cb, uint32_t firstBinding,
                                                        uint32_t bindingCount,
                                                        const VkBuffer* pBuffers,
                                                        const VkDeviceSize* pOffsets) {
    StubScope stub_scope;
    vk_wire::FixedWriter<256> w;

    w.u32(firstBinding);
    w.u32(bindingCount);
    for (uint32_t i = 0; i < bindingCount; ++i) w.u64(to_u64(pBuffers[i]));
    for (uint32_t i = 0; i < bindingCount; ++i) w.u64(pOffsets != nullptr ? pOffsets[i] : 0);
    record_bytes(cb, vk_wire::CmdKind::BindVertexBuffers, w.data(), w.size());
}

VKAPI_ATTR void VKAPI_CALL stud_vkCmdBindIndexBuffer(VkCommandBuffer cb, VkBuffer buffer,
                                                      VkDeviceSize offset, VkIndexType type) {
    StubScope stub_scope;
    vk_wire::FixedWriter<256> w;

    w.u64(to_u64(buffer));
    w.u64(offset);
    w.u32(type);
    record_bytes(cb, vk_wire::CmdKind::BindIndexBuffer, w.data(), w.size());
}

VKAPI_ATTR void VKAPI_CALL stud_vkCmdDraw(VkCommandBuffer cb, uint32_t vertexCount,
                                           uint32_t instanceCount, uint32_t firstVertex,
                                           uint32_t firstInstance) {
    StubScope stub_scope;
    vk_wire::FixedWriter<256> w;

    w.u32(vertexCount);
    w.u32(instanceCount);
    w.u32(firstVertex);
    w.u32(firstInstance);
    record_bytes(cb, vk_wire::CmdKind::Draw, w.data(), w.size());
}

VKAPI_ATTR void VKAPI_CALL stud_vkCmdDrawIndexed(VkCommandBuffer cb, uint32_t indexCount,
                                                  uint32_t instanceCount, uint32_t firstIndex,
                                                  int32_t vertexOffset, uint32_t firstInstance) {
    StubScope stub_scope;
    vk_wire::FixedWriter<256> w;

    w.u32(indexCount);
    w.u32(instanceCount);
    w.u32(firstIndex);
    w.u32(static_cast<uint32_t>(vertexOffset));
    w.u32(firstInstance);
    record_bytes(cb, vk_wire::CmdKind::DrawIndexed, w.data(), w.size());
}

VKAPI_ATTR void VKAPI_CALL stud_vkCmdDispatch(VkCommandBuffer cb, uint32_t x, uint32_t y,
                                               uint32_t z) {
    StubScope stub_scope;
    vk_wire::FixedWriter<256> w;

    w.u32(x);
    w.u32(y);
    w.u32(z);
    record_bytes(cb, vk_wire::CmdKind::Dispatch, w.data(), w.size());
}

VKAPI_ATTR void VKAPI_CALL stud_vkCmdSetViewport(VkCommandBuffer cb, uint32_t first,
                                                  uint32_t count, const VkViewport* pViewports) {
    StubScope stub_scope;
    vk_wire::FixedWriter<256> w;

    w.u32(first);
    w.u32(count);
    for (uint32_t i = 0; i < count; ++i) {
        w.f32(pViewports[i].x);
        w.f32(pViewports[i].y);
        w.f32(pViewports[i].width);
        w.f32(pViewports[i].height);
        w.f32(pViewports[i].minDepth);
        w.f32(pViewports[i].maxDepth);
    }
    record_bytes(cb, vk_wire::CmdKind::SetViewport, w.data(), w.size());
}

VKAPI_ATTR void VKAPI_CALL stud_vkCmdSetScissor(VkCommandBuffer cb, uint32_t first,
                                                 uint32_t count, const VkRect2D* pScissors) {
    StubScope stub_scope;
    vk_wire::FixedWriter<256> w;

    w.u32(first);
    w.u32(count);
    for (uint32_t i = 0; i < count; ++i) {
        w.u32(static_cast<uint32_t>(pScissors[i].offset.x));
        w.u32(static_cast<uint32_t>(pScissors[i].offset.y));
        w.u32(pScissors[i].extent.width);
        w.u32(pScissors[i].extent.height);
    }
    record_bytes(cb, vk_wire::CmdKind::SetScissor, w.data(), w.size());
}

VKAPI_ATTR void VKAPI_CALL stud_vkCmdPipelineBarrier(
    VkCommandBuffer cb, VkPipelineStageFlags srcStage, VkPipelineStageFlags dstStage,
    VkDependencyFlags depFlags, uint32_t memCount, const VkMemoryBarrier* pMem,
    uint32_t bufCount, const VkBufferMemoryBarrier* pBuf, uint32_t imgCount,
    const VkImageMemoryBarrier* pImg) {
    std::vector<uint8_t>& in = wire_scratch();
    vk_wire::Writer w(in);
    w.u32(srcStage);
    w.u32(dstStage);
    w.u32(depFlags);
    w.u32(memCount);
    for (uint32_t i = 0; i < memCount; ++i) {
        w.u32(pMem[i].srcAccessMask);
        w.u32(pMem[i].dstAccessMask);
    }
    w.u32(bufCount);
    for (uint32_t i = 0; i < bufCount; ++i) {
        w.u32(pBuf[i].srcAccessMask);
        w.u32(pBuf[i].dstAccessMask);
        w.u32(pBuf[i].srcQueueFamilyIndex);
        w.u32(pBuf[i].dstQueueFamilyIndex);
        w.u64(to_u64(pBuf[i].buffer));
        w.u64(pBuf[i].offset);
        w.u64(pBuf[i].size);
    }
    w.u32(imgCount);
    for (uint32_t i = 0; i < imgCount; ++i) {
        w.u32(pImg[i].srcAccessMask);
        w.u32(pImg[i].dstAccessMask);
        w.u32(pImg[i].oldLayout);
        w.u32(pImg[i].newLayout);
        w.u32(pImg[i].srcQueueFamilyIndex);
        w.u32(pImg[i].dstQueueFamilyIndex);
        w.u64(to_u64(pImg[i].image));
        w.u32(pImg[i].subresourceRange.aspectMask);
        w.u32(pImg[i].subresourceRange.baseMipLevel);
        w.u32(pImg[i].subresourceRange.levelCount);
        w.u32(pImg[i].subresourceRange.baseArrayLayer);
        w.u32(pImg[i].subresourceRange.layerCount);
    }
    record(cb, vk_wire::CmdKind::PipelineBarrier, in);
}

VKAPI_ATTR void VKAPI_CALL stud_vkCmdCopyBuffer(VkCommandBuffer cb, VkBuffer src, VkBuffer dst,
                                                 uint32_t count, const VkBufferCopy* pRegions) {
    std::vector<uint8_t>& in = wire_scratch();
    vk_wire::Writer w(in);
    w.u64(to_u64(src));
    w.u64(to_u64(dst));
    w.u32(count);
    for (uint32_t i = 0; i < count; ++i) {
        w.u64(pRegions[i].srcOffset);
        w.u64(pRegions[i].dstOffset);
        w.u64(pRegions[i].size);
    }
    record(cb, vk_wire::CmdKind::CopyBuffer, in);
}

namespace {
// A host-visible, coherent memory type. Coherent specifically: Stud's own
// mapped-memory tracking ships these bytes to the device at submit, the
// same path every other mapped write already takes, so nothing extra has
// to be flushed by hand.
bool find_host_visible_memory_type(uint32_t type_bits, uint32_t& out_index) {
    VkPhysicalDeviceMemoryProperties props{};
    stud_vkGetPhysicalDeviceMemoryProperties(g_decode_physical_device, &props);
    const VkMemoryPropertyFlags want =
        VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
    for (uint32_t i = 0; i < props.memoryTypeCount; ++i) {
        if ((type_bits & (1u << i)) == 0) continue;
        if ((props.memoryTypes[i].propertyFlags & want) == want) {
            out_index = i;
            return true;
        }
    }
    return false;
}

// A sub-allocation of a command buffer's scratch: where to write the
// decoded bytes, and the buffer/offset the copy has to name.
struct ScratchSlice {
    VkBuffer buffer = VK_NULL_HANDLE;
    VkDeviceMemory memory = VK_NULL_HANDLE;
    uint8_t* host = nullptr;
    VkDeviceSize offset = 0;
};

bool allocate_scratch_buffer(VkDeviceSize size, ScratchBuffer& out) {
    ScratchBuffer sb;
    VkBufferCreateInfo bci{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
    bci.size = size;
    bci.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
    bci.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    if (stud_vkCreateBuffer(g_decode_device, &bci, nullptr, &sb.buffer) != VK_SUCCESS) return false;
    VkMemoryRequirements req{};
    stud_vkGetBufferMemoryRequirements(g_decode_device, sb.buffer, &req);
    uint32_t type_index = 0;
    if (!find_host_visible_memory_type(req.memoryTypeBits, type_index)) return false;
    // Said once: whether the decoded bytes land in memory the host shares
    // with this process, or in a staging copy that has to cross the
    // socket on every submit. A 4096x4096 texture decodes to 64MB, so
    // which of the two this is decides most of a texture burst's cost.
    static bool announced = false;
    if (!announced) {
        announced = true;
        std::printf("stud: vulkan-client: texture decode scratch uses memory type %u\n",
                    type_index);
        std::fflush(stdout);
    }
    VkMemoryAllocateInfo mai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    mai.allocationSize = req.size;
    mai.memoryTypeIndex = type_index;
    if (stud_vkAllocateMemory(g_decode_device, &mai, nullptr, &sb.memory) != VK_SUCCESS) {
        return false;
    }
    if (stud_vkBindBufferMemory(g_decode_device, sb.buffer, sb.memory, 0) != VK_SUCCESS) {
        return false;
    }
    void* mapped = nullptr;
    // The real size, never VK_WHOLE_SIZE: this client cannot map that (it
    // has no length to allocate a barrier for) and returns failure, which
    // silently cost every decode its scratch and put raw compressed bytes
    // on screen.
    if (stud_vkMapMemory(g_decode_device, sb.memory, 0, req.size, 0, &mapped) != VK_SUCCESS) {
        return false;
    }
    sb.mapped = static_cast<uint8_t*>(mapped);
    sb.size = req.size;
    out = sb;
    return true;
}

bool acquire_scratch(VkCommandBuffer cb, VkDeviceSize size, ScratchSlice& out) {
    if (g_decode_device == VK_NULL_HANDLE || size == 0) return false;
    ScratchPool& pool = scratch_pools()[to_u64(cb)];
    // Keep every sub-allocation comfortably aligned: a buffer-image copy
    // offset must suit the texel size, which is at most 4 bytes here.
    constexpr VkDeviceSize kAlign = 16;
    VkDeviceSize offset = (pool.used + kAlign - 1) & ~(kAlign - 1);
    if (pool.current.mapped == nullptr || offset + size > pool.current.size) {
        VkDeviceSize want = pool.current.size * 2;
        if (want < offset + size) want = offset + size;
        if (want < (1u << 20)) want = 1u << 20;
        ScratchBuffer grown;
        if (!allocate_scratch_buffer(want, grown)) return false;
        if (pool.current.mapped != nullptr) pool.retired.push_back(pool.current);
        pool.current = grown;
        pool.used = 0;
        offset = 0;
    }
    out.buffer = pool.current.buffer;
    out.memory = pool.current.memory;
    out.host = pool.current.mapped + offset;
    out.offset = offset;
    pool.used = offset + size;
    return true;
}

// The engine's own bytes for a region of a buffer it filled through a
// mapping. Null if the buffer is not backed by memory this process has
// mapped, in which case the copy cannot be decoded and is passed through
// untouched rather than guessed at.
const uint8_t* mapped_bytes_for(VkBuffer buffer, uint64_t offset, uint64_t length) {
    // buffer_bindings() is written under emulation_mutex (vkBindBufferMemory,
    // vkDestroyBuffer) and mapped_ranges() under mapped_mutex, and this reads
    // both. Taken in that order everywhere they are held together: the decode
    // path already holds emulation_mutex when it goes back through
    // vkMapMemory, which takes mapped_mutex.
    std::lock_guard<std::recursive_mutex> emu_lock(emulation_mutex());
    std::lock_guard<std::recursive_mutex> lock(mapped_mutex());
    auto bit = buffer_bindings().find(to_u64(buffer));
    if (bit == buffer_bindings().end()) return nullptr;
    // Shared with the host: the engine wrote straight into the file both
    // processes map, so the bytes are simply there. This has to be checked
    // before the staged mappings, a shared allocation deliberately has
    // no MappedRange, and missing that made every compressed texture
    // decode fail and blank itself.
    {
        auto shared = shared_allocations().find(bit->second.memory);
        if (shared != shared_allocations().end() && shared->second.address != nullptr) {
            const uint64_t start = bit->second.offset + offset;
            if (start + length > shared->second.length) return nullptr;
            return static_cast<const uint8_t*>(shared->second.address) + start;
        }
    }
    auto mit = mapped_ranges().find(bit->second.memory);
    if (mit == mapped_ranges().end()) return nullptr;
    MappedRange& range = mit->second;
    const uint64_t start = bit->second.offset + offset;
    if (start < range.offset) return nullptr;
    const uint64_t local = start - range.offset;
    if (local + length > range.length()) return nullptr;
    return range.bytes() + local;
}

void arm_pending_readbacks(const std::vector<VkCommandBuffer>& submitted) {
    std::lock_guard<std::recursive_mutex> lock(emulation_mutex());
    auto& recorded = recorded_readbacks();
    if (recorded.empty()) return;
    auto& awaiting = awaiting_readbacks();
    for (size_t i = 0; i < recorded.size();) {
        const bool in_this_submit =
            std::find(submitted.begin(), submitted.end(), recorded[i].cb) != submitted.end();
        if (in_this_submit) {
            awaiting.push_back(recorded[i]);
            recorded.erase(recorded.begin() + static_cast<long>(i));
        } else {
            ++i;
        }
    }
    g_have_readbacks.store(!awaiting.empty(), std::memory_order_relaxed);
}

void fetch_pending_readbacks() {
    if (!g_have_readbacks.load(std::memory_order_relaxed)) return;
    std::vector<PendingReadback> take;
    {
        std::lock_guard<std::recursive_mutex> lock(emulation_mutex());
        take.swap(awaiting_readbacks());
        g_have_readbacks.store(false, std::memory_order_relaxed);
    }
    if (take.empty()) return;
    std::vector<uint8_t> bytes;
    for (const PendingReadback& rb : take) {
        // The engine's own mapping of this allocation is where the result
        // has to end up. Taken fresh each time: an allocation can be
        // unmapped between the copy and the wait.
        uint8_t* dst = nullptr;
        uint64_t dst_length = 0;
        {
            std::lock_guard<std::recursive_mutex> lock(mapped_mutex());
            auto mit = mapped_ranges().find(rb.memory);
            if (mit == mapped_ranges().end()) continue;
            MappedRange& range = mit->second;
            if (rb.offset < range.offset) continue;
            const uint64_t local = rb.offset - range.offset;
            if (local >= range.length()) continue;
            dst = range.bytes() + local;
            dst_length = range.length() - local;
        }
        if (dst == nullptr) continue;
        const uint64_t want = std::min<uint64_t>(rb.size, dst_length);
        if (want == 0) continue;
        bytes.assign(static_cast<size_t>(want), 0);
        uint64_t a[8] = {0, rb.memory, rb.offset, want};
        uint32_t written = 0;
        const uint64_t r = stud::render_client::connection().call(
            CallId::VkReadMappedMemory, a, nullptr, 0, bytes.data(),
            static_cast<uint32_t>(bytes.size()), &written);
        if (static_cast<VkResult>(static_cast<int32_t>(r)) != VK_SUCCESS || written == 0) {
            static bool warned = false;
            if (!warned) {
                warned = true;
                std::fprintf(stderr,
                             "stud: vulkan-client: could not read a GPU readback back from the "
                             "host (result %d, %u bytes)\n",
                             static_cast<int>(static_cast<int32_t>(r)), written);
            }
            continue;
        }
        // Writing through the engine's own mapping, so the write barrier
        // sees it exactly as it sees the engine's writes: these pages are
        // marked dirty and go back to the host on the next flush. That is
        // one redundant send of bytes the host already has, only for a
        // readback, and correctness first.
        std::memcpy(dst, bytes.data(), std::min<size_t>(written, static_cast<size_t>(want)));
    }
}

// Decodes everything recorded since the last submit.
//
// This runs at submit rather than at record time on purpose. A copy is
// recorded before the engine is obliged to have finished filling its
// staging buffer, only the submit is a real ordering point, so
// decoding at record time reads whatever happens to be there. That is
// invisible for most textures, whose data is already complete, and
// leaves a handful corrupt, which is exactly the symptom that survived
// the first version of this code.
// Decoding runs across several threads, because it is genuinely a lot of
// work to land in one frame: measured at 62.7 ms for 72 MB in a single
// submit when a batch of textures streams in, which is precisely the frame
// spike that survived every other fix. ETC and EAC blocks are independent
// of one another, so a level splits by rows of blocks with no seams,
// PVRTC does not (its texels interpolate across block boundaries), so it
// is handed over whole.
struct DecodeBand {
    VkFormat format = VK_FORMAT_UNDEFINED;
    const uint8_t* src = nullptr;  // first block row of this band
    uint8_t* dst = nullptr;        // first texel row of this band
    uint64_t row_pitch = 0;
    uint32_t width = 0;
    uint32_t height = 0;
};

// A small persistent pool. These threads only ever touch the pixels handed
// to them and never make a JNI call, so they need none of the attachment
// every other Stud-spawned thread does.
class DecodePool {
public:
    static DecodePool& get() {
        static DecodePool pool;
        return pool;
    }

    void run(std::vector<DecodeBand> bands) {
        if (bands.empty()) return;
        // One batch object per call, handed to the workers by shared
        // pointer. The completion counter counts BANDS, not workers.
        //
        // It used to count workers: run() set remaining_ to the worker
        // count and waited for each to decrement once. A worker that was
        // still finishing the previous batch when the next one was posted
        // saw the generation jump by two and decremented once for both,
        // so remaining_ never reached zero and run() span on
        // std::this_thread::yield() forever, at 100% of a core, with the
        // engine's rendering stopped behind it. Rare while submits were
        // synchronous and one frame apart; immediate once they were
        // queued back to back. Counting the work itself cannot miss:
        // whatever the workers do or do not pick up, the calling thread
        // drains the rest, so the count always reaches zero.
        auto batch = std::make_shared<Batch>();
        const size_t count = bands.size();
        // The batch OWNS its bands. It used to point at the caller's own
        // vector, which run() then destroyed the moment the count reached
        // zero, while a worker that woke late was still inside
        // work_on() and about to read it. That is a use-after-free on a
        // destroyed std::vector, and it read back as a near-null fault
        // (addr=0x1e0, 0x320, ...) that Stud's own recovery answered by
        // killing the thread. Every occurrence cost a pool thread, and
        // when the thread it killed was the one draining the deferred
        // submit queue, presentation stopped for good: the frozen window
        // this was reported as. The shared_ptr keeps the bands alive for
        // as long as any worker can still reach them.
        batch->bands = std::move(bands);
        batch->outstanding.store(count, std::memory_order_release);
        {
            std::lock_guard<std::mutex> lock(mutex_);
            current_ = batch;
            ++generation_;
        }
        wake_.notify_all();
        work_on(*batch);  // the calling thread takes its share rather than idling
        while (batch->outstanding.load(std::memory_order_acquire) != 0) {
            std::this_thread::yield();
        }
        std::lock_guard<std::mutex> lock(mutex_);
        if (current_ == batch) current_.reset();
    }

private:
    struct Batch {
        std::vector<DecodeBand> bands;
        std::atomic<size_t> next{0};
        std::atomic<size_t> outstanding{0};
    };

    DecodePool() {
        // The engine keeps about one and a half cores busy and the rest of
        // this machine sits idle, so a decode burst may as well use them.
        // Two cores are left alone for the engine and the render host.
        unsigned n = std::thread::hardware_concurrency();
        if (n < 2) n = 2;
        if (n > 4) n -= 2;
        if (n > 16) n = 16;
        workers_ = n - 1;
        for (unsigned i = 0; i < workers_; ++i) {
            std::thread([this] {
                uint64_t seen = 0;
                for (;;) {
                    std::shared_ptr<Batch> batch;
                    {
                        std::unique_lock<std::mutex> lock(mutex_);
                        wake_.wait(lock, [&] { return generation_ != seen; });
                        seen = generation_;
                        batch = current_;
                    }
                    if (batch) work_on(*batch);
                }
            }).detach();
        }
    }

    static void work_on(Batch& batch) {
        const std::vector<DecodeBand>& bands = batch.bands;
        for (;;) {
            const size_t i = batch.next.fetch_add(1, std::memory_order_relaxed);
            if (i >= bands.size()) return;
            const DecodeBand& b = bands[i];
            stud::texture_decode::decode(b.format, b.src, b.width, b.height, b.dst, b.row_pitch);
            batch.outstanding.fetch_sub(1, std::memory_order_release);
        }
    }

    unsigned workers_ = 0;
    std::mutex mutex_;
    std::condition_variable wake_;
    uint64_t generation_ = 0;
    std::shared_ptr<Batch> current_;
};

// Ships what a decode just wrote into the scratch.
//
// vkQueueSubmit flushes the engine's mapped memory and THEN runs the
// decodes. It has to be that order, because that flush must keep
// program order with the engine's own writes. But the scratch a decode
// writes into is mapped memory too, and it is written after the flush,
// so nothing sent it until the next submit came along. One submit late
// is too late: the copy that reads those bytes belongs to THIS submit.
// On a cold Home the result was a character wearing the skybox; joining
// a game and coming back appeared to "fix" it, because by then the
// bytes had arrived a submit behind and the texture was re-uploaded.
//
// Deliberately NOT push_mapped_bytes(): that sends the dirty pages and
// then re-arms the whole allocation, and doing that from the decode
// thread while the engine's own thread is re-arming every mapping at
// submit loses dirty pages between the two, which is a far worse bug
// than the one being fixed. This is Stud's own scratch and the decode
// just wrote it, so there is nothing to work out: send it and leave the
// barrier alone. The pages stay dirty, which costs one redundant send at
// the engine's next flush and cannot lose anything.
void send_decoded_scratch(uint64_t memory) {
    std::lock_guard<std::recursive_mutex> lock(mapped_mutex());
    auto it = mapped_ranges().find(memory);
    if (it == mapped_ranges().end()) return;  // shared with the host: already visible
    MappedRange& m = it->second;
    const std::size_t len = m.length();
    if (len > 0) send_mapped_run(memory, m, 0, len);
}

// Decoding a texture when the copy is RECORDED rather than when it is
// submitted.
//
// Measured, with STUD_TEX_DECODE_TRACE on a real session: "texture
// decode took 38.3 ms this submit (8.33 MB)", against frame intervals
// of 39.1, 56.4 and 61.2ms in the flight recorder, whose gap sat
// between the frame's last barrier and its submit. The decode is
// already off the engine's own thread -- it runs on the writer thread
// -- but it is ordered immediately ahead of the submit it feeds, so its
// whole cost lands on the frame anyway.
//
// Moving it earlier is safe only if the bytes it reads are the bytes
// the copy will read. A copy's source only has to be correct when the
// copy EXECUTES, so the engine may legitimately keep writing after
// recording it, and run_pending_decodes defers for exactly that reason.
// So this does not assume; it records an XXH3-128 of the source and the
// submit compares. Equal means the speculative decode describes the
// bytes the copy will read and the work is already done; different
// means it is thrown away and the ordinary path runs, which is no worse
// than today.
//
// The residual risk is a source rewritten to something else and then
// restored to its original bytes entirely within this window, which
// would match the hash while the decode read neither state. Staging
// buffers are written once and copied; nothing in the engine does that.
//
// OFF by default. This is the path that produced corrupted textures
// once already, and a change here earns its default by being measured,
// not by being reasoned about.
bool early_decode_enabled() {
    static const bool on = std::getenv("STUD_TEX_EARLY_DECODE") != nullptr;
    return on;
}

class EarlyDecoder {
public:
    static EarlyDecoder& instance() {
        static EarlyDecoder d;
        return d;
    }

    // Queues one decode. Returns immediately; the caller is the engine's
    // own recording thread and must not wait for pixels.
    void submit(const PendingDecode& pd, const uint8_t* src) {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            jobs_.push_back(Job{pd, src});
        }
        wake_.notify_one();
    }

    // Waits only for the jobs this submit is about to read.
    //
    // Draining everything was the first version and it was worse than no
    // speculation at all: a submit ended up waiting on decodes for
    // command buffers it does not carry, which is work that had every
    // right to still be running. Measured at 32ms to decode 0.56MB,
    // against 38ms for 8.33MB before the change -- almost all of it
    // waiting.
    //
    // Scoped this way, a job for a later submit keeps decoding in the
    // background, which is the entire point: its cost lands on no frame
    // at all.
    void drain_for(const std::vector<VkCommandBuffer>& submitted) {
        std::unique_lock<std::mutex> lock(mutex_);
        done_.wait(lock, [&] {
            for (const Job& j : jobs_) {
                if (std::find(submitted.begin(), submitted.end(), j.pd.cb) != submitted.end()) {
                    return false;
                }
            }
            return !(running_ && std::find(submitted.begin(), submitted.end(), running_cb_) !=
                                     submitted.end());
        });
    }

    // The hash this source decoded to, if it was decoded here.
    bool result_for(const uint8_t* src, uint64_t* high, uint64_t* low) {
        std::lock_guard<std::mutex> lock(mutex_);
        auto it = results_.find(src);
        if (it == results_.end()) return false;
        *high = it->second.first;
        *low = it->second.second;
        results_.erase(it);
        return true;
    }

private:
    struct Job {
        PendingDecode pd;
        const uint8_t* src;
    };

    EarlyDecoder() : thread_([this] { loop(); }) { thread_.detach(); }

    void loop() {
        for (;;) {
            Job job;
            {
                std::unique_lock<std::mutex> lock(mutex_);
                wake_.wait(lock, [this] { return !jobs_.empty(); });
                job = jobs_.front();
                jobs_.erase(jobs_.begin());
                running_ = true;
                running_cb_ = job.pd.cb;
            }
            const PendingDecode& pd = job.pd;
            bool ok = true;
            for (uint32_t layer = 0; layer < pd.layers && ok; ++layer) {
                ok = stud::texture_decode::decode(
                    pd.format, job.src + layer * pd.src_layer_bytes, pd.width, pd.height,
                    pd.dst + layer * pd.dst_layer_bytes, pd.row_pitch);
            }
            // Hashed AFTER the decode, so a match at submit means the
            // bytes did not move between this decode finishing and the
            // copy being submitted.
            XXH128_hash_t h{};
            if (ok) {
                h = XXH3_128bits(job.src, pd.src_layer_bytes * pd.layers);
            }
            {
                std::lock_guard<std::mutex> lock(mutex_);
                if (ok) results_[job.src] = {h.high64, h.low64};
                running_ = false;
            }
            done_.notify_all();
        }
    }

    std::mutex mutex_;
    std::condition_variable wake_;
    std::condition_variable done_;
    std::vector<Job> jobs_;
    std::map<const uint8_t*, std::pair<uint64_t, uint64_t>> results_;
    bool running_ = false;
    // Which command buffer the in-flight job belongs to, so a submit can
    // tell whether it has to wait for it.
    VkCommandBuffer running_cb_ = VK_NULL_HANDLE;
    std::thread thread_;
};

void run_pending_decodes(const std::vector<VkCommandBuffer>& submitted) {
    // The lock covers only the bookkeeping. Running the decode itself
    // under it would hold the engine out of vkCreateImage and every
    // vkCmdCopyBufferToImage for the whole burst, 15ms, measured,
    // which is the opposite of the point now that this runs off the
    // engine's own thread.
    std::unique_lock<std::recursive_mutex> lock(emulation_mutex());
    auto& pending = pending_decodes();
    if (pending.empty()) return;
    // Texture decoding happens here, on the thread that submits, so a
    // burst of streamed-in textures lands entirely inside one frame. That
    // is a plausible source of frame spikes, so anything expensive says so
    // rather than being guessed at.
    const auto decode_t0 = std::chrono::steady_clock::now();
    // Only what is actually decoded in this pass. Counting every pending
    // entry, including the ones belonging to command buffers this submit
    // does not carry, reported work that never happened, which is what
    // made a static Home screen look like it decoded 88GB in 100s.
    uint64_t decoded_bytes = 0;
    // Speculative decodes reused, and thrown away because the source
    // moved. Reported together below so the trade is visible rather than
    // assumed.
    size_t early_reused = 0;
    size_t early_stale = 0;
    // Nothing speculative is read until every queued job has been
    // attempted; a job still running has recorded no hash, so it would
    // simply be decoded again here.
    if (early_decode_enabled()) EarlyDecoder::instance().drain_for(submitted);
    // Which scratch allocations this pass wrote into, so they can be sent
    // once the decode has actually produced the bytes.
    std::vector<uint64_t> scratch_memories;
    std::vector<PendingDecode> keep;
    std::vector<DecodeBand> bands;
    for (const PendingDecode& pd : pending) {
        // Only the buffers in this submit. Decoding another command
        // buffer's copies here would read its staging data before its own
        // submit, which is the very ordering problem this defers for.
        if (std::find(submitted.begin(), submitted.end(), pd.cb) == submitted.end()) {
            keep.push_back(pd);
            continue;
        }
        const uint8_t* bytes =
            mapped_bytes_for(pd.src, pd.src_offset, pd.src_layer_bytes * pd.layers);
        const bool ok = bytes != nullptr && stud::texture_decode::is_emulated(pd.format);
        // Decoded already, when the copy was recorded. Kept apart from
        // `ok`, which means "this could be decoded at all" and whose
        // false branch ZEROES the scratch on purpose so a decoder that
        // failed shows as an obviously blank texture. Folding the two
        // together is what turned every reused texture black.
        bool reused = false;
        // Already decoded when this copy was recorded?
        //
        // Only if the source still hashes to what it did then. The
        // engine is allowed to have written it since, and if it did the
        // speculative pixels describe bytes that no longer exist, so
        // they are dropped and the ordinary path below runs.
        if (ok && pd.speculated) {
            uint64_t high = 0;
            uint64_t low = 0;
            if (EarlyDecoder::instance().result_for(bytes, &high, &low)) {
                const XXH128_hash_t now =
                    XXH3_128bits(bytes, pd.src_layer_bytes * pd.layers);
                if (now.high64 == high && now.low64 == low) {
                    // The pixels are already in scratch. Everything below
                    // -- the bands, the pool, the wait -- is skipped.
                    early_reused += 1;
                    decoded_bytes += pd.dst_layer_bytes * pd.layers;
                    if (pd.scratch_memory != VK_NULL_HANDLE) {
                        const uint64_t mem = to_u64(pd.scratch_memory);
                        if (std::find(scratch_memories.begin(), scratch_memories.end(), mem) ==
                            scratch_memories.end()) {
                            scratch_memories.push_back(mem);
                        }
                    }
                    reused = true;  // the pixels are already in scratch
                } else {
                    early_stale += 1;
                }
            }
        }
        if (ok && !reused) {
            decoded_bytes += pd.dst_layer_bytes * pd.layers;
            if (pd.scratch_memory != VK_NULL_HANDLE) {
                const uint64_t mem = to_u64(pd.scratch_memory);
                if (std::find(scratch_memories.begin(), scratch_memories.end(), mem) ==
                    scratch_memories.end()) {
                    scratch_memories.push_back(mem);
                }
            }
            // Split into bands rather than decoding here: the pool below
            // runs them all at once, which is what keeps a large batch
            // from landing entirely inside one frame.
            const uint64_t row_pitch =
                pd.row_pitch != 0
                    ? pd.row_pitch
                    : stud::texture_decode::row_pitch_for_texels(pd.format, pd.width);
            constexpr uint32_t kBandRows = 128;  // whole blocks, 32 of them
            const bool splittable = !stud::texture_decode::is_pvrtc(pd.format);
            for (uint32_t layer = 0; layer < pd.layers; ++layer) {
                const uint8_t* src_layer = bytes + pd.src_layer_bytes * layer;
                uint8_t* dst_layer = pd.dst + pd.dst_layer_bytes * layer;
                const uint32_t step = splittable ? kBandRows : pd.height;
                for (uint32_t y = 0; y < pd.height; y += step) {
                    DecodeBand b;
                    b.format = pd.format;
                    b.src = src_layer + static_cast<uint64_t>(y / 4) * row_pitch;
                    // Where this band's rows really start. A transcoded
                    // level is stored as blocks, so this is not y times a
                    // row of texels, getting that wrong wrote each band
                    // past the end of the one before it.
                    b.dst = dst_layer + stud::texture_decode::decoded_row_offset(pd.format,
                                                                                 pd.width, y);
                    b.row_pitch = row_pitch;
                    b.width = pd.width;
                    b.height = (y + step <= pd.height) ? step : pd.height - y;
                    bands.push_back(b);
                }
            }
        }
        if (!ok) {
            // The copy is already recorded, so the image will show
            // whatever the scratch holds. Zero it: a blank texture is
            // obviously wrong, where undecoded bytes look like a decoder
            // that half works.
            std::memset(pd.dst, 0, static_cast<size_t>(pd.dst_layer_bytes) * pd.layers);
            static bool warned = false;
            if (!warned) {
                warned = true;
                std::fprintf(stderr,
                             "stud: vulkan-client: a compressed texture upload could not be "
                             "decoded (format=%u %ux%u layers=%u)\n",
                             static_cast<unsigned>(pd.format), pd.width, pd.height, pd.layers);
            }
        }
    }
    static const bool decode_trace = std::getenv("STUD_TEX_DECODE_TRACE") != nullptr;
    if (decode_trace && !bands.empty()) {
        // Group by image so a burst says whether it is many textures
        // arriving once or one texture arriving many times.
        std::map<uint64_t, std::pair<uint64_t, uint32_t>> by_image;  // image -> (bytes, uploads)
        for (const PendingDecode& pd : pending) {
            if (std::find(submitted.begin(), submitted.end(), pd.cb) == submitted.end()) continue;
            auto& e = by_image[pd.image];
            e.first += pd.dst_layer_bytes * pd.layers;
            e.second += 1;
        }
        std::fprintf(stderr, "stud: decode burst: %zu images,", by_image.size());
        int shown = 0;
        for (const auto& kv : by_image) {
            if (shown++ >= 4) break;
            std::fprintf(stderr, " [img %llx %llu KB x%u]",
                         (unsigned long long)kv.first,
                         (unsigned long long)(kv.second.first / 1024), kv.second.second);
        }
        std::fprintf(stderr, "\n");
    }

    pending.swap(keep);
    // Every band at once, across the pool, with the bookkeeping lock
    // released: the bands point at the source staging and the scratch,
    // neither of which this map guards.
    lock.unlock();
    DecodePool::get().run(std::move(bands));

    // The decode has produced the bytes; now send them. See
    // send_decoded_scratch() for why this cannot wait for the next
    // submit's flush and why it does not go through the dirty tracking.
    {
        std::lock_guard<std::recursive_mutex> send_lock(emulation_mutex());
        for (uint64_t memory : scratch_memories) send_decoded_scratch(memory);
    }
    const double ms = std::chrono::duration<double, std::milli>(
                          std::chrono::steady_clock::now() - decode_t0)
                          .count();
    if (frame_timing_enabled()) {
        frame_timing().f_decode += ms;
        frame_timing().decode_ms += ms;
        frame_timing().decode_bytes += decoded_bytes;
    }
    // Behind the same switch as the burst breakdown above. Unconditional,
    // this wrote an unbuffered line per stalling submit, 447 of them in
    // one real session, through the session-log tee, at exactly the moment
    // the frame was already late. Investigation output, armed by default.
    if (decode_trace && (early_reused != 0 || early_stale != 0)) {
        std::fprintf(stderr,
                     "stud: early decode: %zu reused, %zu thrown away (source moved)\n",
                     early_reused, early_stale);
    }
    if (decode_trace && ms >= 2.0) {
        std::fprintf(stderr, "stud: texture decode took %.1f ms this submit (%.2f MB)\n", ms,
                     static_cast<double>(decoded_bytes) / (1024.0 * 1024.0));
    }
}
}  // namespace

VKAPI_ATTR void VKAPI_CALL stud_vkCmdCopyBufferToImage(VkCommandBuffer cb, VkBuffer src,
                                                        VkImage dst, VkImageLayout layout,
                                                        uint32_t count,
                                                        const VkBufferImageCopy* pRegions) {
    // An emulated image really exists as an uncompressed one, so the
    // compressed bytes the engine staged have to be decoded before the
    // device ever sees them. The decode happens here, at record time,
    // because that is when the data is known to be complete: the engine
    // cannot record a copy of texels it has not produced yet.
    std::vector<VkBufferImageCopy> decoded_regions;
    VkBuffer decode_src = VK_NULL_HANDLE;
    // Set when the copy cannot be decoded. Recording it anyway would put
    // compressed bytes into an uncompressed image, which is every texture
    // turned to noise, a texture that never arrives is the lesser harm,
    // and the warning says which happened.
    bool skip_copy = false;
    if (count > 0 && pRegions != nullptr) {
        std::lock_guard<std::recursive_mutex> lock(emulation_mutex());
        auto it = emulated_images().find(to_u64(dst));
        if (it != emulated_images().end()) {
            const VkFormat compressed = it->second.compressed;
            // Size the scratch for every region first, so one buffer
            // serves the whole copy and mip chains do not each take one.
            // A region can carry more than one image: an array or cube
            // texture copies several layers at once (a skybox is six), and
            // a 3D texture several slices. They sit back to back in the
            // buffer, so every size here is per-layer and multiplied out.
            // Decoding only the first layer left the rest as noise, which
            // is what "some textures are still broken" looked like.
            std::vector<uint64_t> offsets(count, 0);
            std::vector<uint32_t> layer_counts(count, 1);
            uint64_t total = 0;
            // A copy's bufferOffset has to be a multiple of 4 and of the
            // texel size. Packing regions back to back breaks that the
            // moment one of them is an odd number of 2-byte texels (a
            // small EAC R11 mip), so each starts on a 16-byte boundary.
            auto align16 = [](uint64_t v) { return (v + 15u) & ~static_cast<uint64_t>(15u); };
            for (uint32_t i = 0; i < count; ++i) {
                const VkExtent3D& e = pRegions[i].imageExtent;
                const uint32_t layers =
                    (pRegions[i].imageSubresource.layerCount == 0
                         ? 1u
                         : pRegions[i].imageSubresource.layerCount) *
                    (e.depth == 0 ? 1u : e.depth);
                layer_counts[i] = layers;
                offsets[i] = align16(total);
                total = offsets[i] +
                        stud::texture_decode::decoded_size(compressed, e.width, e.height) * layers;
            }
            ScratchSlice scratch;
            const bool have_scratch = total > 0 && acquire_scratch(cb, total, scratch);
            if (!have_scratch) {
                skip_copy = true;
                // Nowhere to decode into. Passing the copy through would
                // put raw compressed bytes into an uncompressed image.
                // Every texture noise, so say so loudly rather than
                // quietly corrupting the frame.
                static bool warned = false;
                if (!warned) {
                    warned = true;
                    std::fprintf(stderr,
                                 "stud: vulkan-client: no scratch for a %llu-byte texture decode "
                                 "(device=%p physical=%p)\n",
                                 static_cast<unsigned long long>(total),
                                 static_cast<void*>(g_decode_device),
                                 static_cast<void*>(g_decode_physical_device));
                }
            }
            if (have_scratch) {
                bool all_decoded = true;
                decoded_regions.assign(pRegions, pRegions + count);
                for (uint32_t i = 0; i < count; ++i) {
                    const VkExtent3D& e = pRegions[i].imageExtent;
                    const uint32_t layers = layer_counts[i];
                    // A copy may describe a source whose rows of blocks
                    // are padded out to a wider row, and whose layers are
                    // spaced by a taller image, rather than being tightly
                    // packed. Both are given in texels of the compressed
                    // image, so they convert to block rows.
                    const uint32_t row_texels = pRegions[i].bufferRowLength != 0
                                                    ? pRegions[i].bufferRowLength
                                                    : e.width;
                    const uint32_t image_texels = pRegions[i].bufferImageHeight != 0
                                                      ? pRegions[i].bufferImageHeight
                                                      : e.height;
                    PendingDecode pd;
                    pd.cb = cb;
                    pd.src = src;
                    pd.src_offset = pRegions[i].bufferOffset;
                    pd.row_pitch =
                        stud::texture_decode::row_pitch_for_texels(compressed, row_texels);
                    pd.src_layer_bytes =
                        stud::texture_decode::encoded_size(compressed, row_texels, image_texels);
                    pd.dst_layer_bytes =
                        stud::texture_decode::decoded_size(compressed, e.width, e.height);
                    pd.dst = scratch.host + offsets[i];
                    pd.scratch_memory = scratch.memory;
                    pd.format = compressed;
                    pd.width = e.width;
                    pd.height = e.height;
                    pd.layers = layers;
                    pd.image = to_u64(dst);
                    // Start it now rather than at submit. The copy will
                    // not execute until the submit either way, and the
                    // hash recorded here is what lets that submit trust
                    // the result. See EarlyDecoder.
                    if (early_decode_enabled()) {
                        const uint8_t* early = mapped_bytes_for(
                            pd.src, pd.src_offset, pd.src_layer_bytes * pd.layers);
                        if (early != nullptr && stud::texture_decode::is_emulated(pd.format)) {
                            pd.speculated = true;
                            EarlyDecoder::instance().submit(pd, early);
                        }
                    }
                    pending_decodes().push_back(pd);

                    decoded_regions[i].bufferOffset = scratch.offset + offsets[i];
                    // Tightly packed, so the implicit "same as the image
                    // extent" row length is exactly right.
                    decoded_regions[i].bufferRowLength = 0;
                    decoded_regions[i].bufferImageHeight = 0;
                }
                if (all_decoded) {
                    decode_src = scratch.buffer;
                } else {
                    decoded_regions.clear();
                    skip_copy = true;
                    static bool warned = false;
                    if (!warned) {
                        warned = true;
                        std::fprintf(stderr,
                                     "stud: vulkan-client: could not decode a compressed texture "
                                     "upload; dropping it\n");
                    }
                }
            }
        }
    }
    if (skip_copy) return;
    if (decode_src != VK_NULL_HANDLE) {
        src = decode_src;
        pRegions = decoded_regions.data();
    }

    std::vector<uint8_t>& in = wire_scratch();
    vk_wire::Writer w(in);
    w.u64(to_u64(src));
    w.u64(to_u64(dst));
    w.u32(layout);
    w.u32(count);
    for (uint32_t i = 0; i < count; ++i) {
        const auto& c = pRegions[i];
        w.u64(c.bufferOffset);
        w.u32(c.bufferRowLength);
        w.u32(c.bufferImageHeight);
        w.u32(c.imageSubresource.aspectMask);
        w.u32(c.imageSubresource.mipLevel);
        w.u32(c.imageSubresource.baseArrayLayer);
        w.u32(c.imageSubresource.layerCount);
        w.u32(static_cast<uint32_t>(c.imageOffset.x));
        w.u32(static_cast<uint32_t>(c.imageOffset.y));
        w.u32(static_cast<uint32_t>(c.imageOffset.z));
        w.u32(c.imageExtent.width);
        w.u32(c.imageExtent.height);
        w.u32(c.imageExtent.depth);
    }
    record(cb, vk_wire::CmdKind::CopyBufferToImage, in);
}

// The readback direction. Same wire shape as the upload above, a
// VkBufferImageCopy carries the same fields whichever way the data
// travels, so the host replays it with the same reader.
//
// Where the result lands: the engine's own destination buffer. When its
// memory is one of the allocations Stud shares with render-host (the
// usual case for anything host-visible), the GPU writes into the very
// pages this process has mapped, so the engine reads the result with no
// transport at all. When it is not, render-host says so once rather than
// letting the engine read whatever was in its own copy.
VKAPI_ATTR void VKAPI_CALL stud_vkCmdCopyImageToBuffer(VkCommandBuffer cb, VkImage src,
                                                        VkImageLayout srcLayout, VkBuffer dst,
                                                        uint32_t count,
                                                        const VkBufferImageCopy* pRegions) {
    std::vector<uint8_t> in;
    vk_wire::Writer w(in);
    w.u64(to_u64(src));
    w.u64(to_u64(dst));
    w.u32(static_cast<uint32_t>(srcLayout));
    w.u32(count);
    for (uint32_t i = 0; i < count; ++i) {
        const auto& c = pRegions[i];
        w.u64(c.bufferOffset);
        w.u32(c.bufferRowLength);
        w.u32(c.bufferImageHeight);
        w.u32(c.imageSubresource.aspectMask);
        w.u32(c.imageSubresource.mipLevel);
        w.u32(c.imageSubresource.baseArrayLayer);
        w.u32(c.imageSubresource.layerCount);
        w.u32(static_cast<uint32_t>(c.imageOffset.x));
        w.u32(static_cast<uint32_t>(c.imageOffset.y));
        w.u32(static_cast<uint32_t>(c.imageOffset.z));
        w.u32(c.imageExtent.width);
        w.u32(c.imageExtent.height);
        w.u32(c.imageExtent.depth);
    }
    // Where the GPU's result cannot reach the engine on its own, remember
    // to go and get it; see PendingReadback.
    {
        std::lock_guard<std::recursive_mutex> emu_lock(emulation_mutex());
        auto bit = buffer_bindings().find(to_u64(dst));
        if (bit != buffer_bindings().end() && bit->second.size != 0) {
            std::lock_guard<std::recursive_mutex> lock(mapped_mutex());
            const bool shared = shared_allocations().count(bit->second.memory) != 0;
            const bool mapped = mapped_ranges().count(bit->second.memory) != 0;
            if (!shared && mapped) {
                recorded_readbacks().push_back(
                    PendingReadback{cb, bit->second.memory, bit->second.offset, bit->second.size});
                static bool said = false;
                if (!said) {
                    said = true;
                    std::printf("stud: vulkan-client: this device's readbacks land in memory the "
                                "host could not share; fetching them back explicitly\n");
                    std::fflush(stdout);
                }
            }
        }
    }
    record(cb, vk_wire::CmdKind::CopyImageToBuffer, in);
}

VKAPI_ATTR void VKAPI_CALL stud_vkCmdCopyImage(VkCommandBuffer cb, VkImage src,
                                                VkImageLayout srcLayout, VkImage dst,
                                                VkImageLayout dstLayout, uint32_t count,
                                                const VkImageCopy* pRegions) {
    std::vector<uint8_t>& in = wire_scratch();
    vk_wire::Writer w(in);
    w.u64(to_u64(src));
    w.u32(srcLayout);
    w.u64(to_u64(dst));
    w.u32(dstLayout);
    w.u32(count);
    auto write_subres = [&w](const VkImageSubresourceLayers& s) {
        w.u32(s.aspectMask);
        w.u32(s.mipLevel);
        w.u32(s.baseArrayLayer);
        w.u32(s.layerCount);
    };
    for (uint32_t i = 0; i < count; ++i) {
        const auto& c = pRegions[i];
        write_subres(c.srcSubresource);
        w.u32(static_cast<uint32_t>(c.srcOffset.x));
        w.u32(static_cast<uint32_t>(c.srcOffset.y));
        w.u32(static_cast<uint32_t>(c.srcOffset.z));
        write_subres(c.dstSubresource);
        w.u32(static_cast<uint32_t>(c.dstOffset.x));
        w.u32(static_cast<uint32_t>(c.dstOffset.y));
        w.u32(static_cast<uint32_t>(c.dstOffset.z));
        w.u32(c.extent.width);
        w.u32(c.extent.height);
        w.u32(c.extent.depth);
    }
    record(cb, vk_wire::CmdKind::CopyImage, in);
}

VKAPI_ATTR void VKAPI_CALL stud_vkCmdBlitImage(VkCommandBuffer cb, VkImage src,
                                                VkImageLayout srcLayout, VkImage dst,
                                                VkImageLayout dstLayout, uint32_t count,
                                                const VkImageBlit* pRegions, VkFilter filter) {
    std::vector<uint8_t>& in = wire_scratch();
    vk_wire::Writer w(in);
    w.u64(to_u64(src));
    w.u32(srcLayout);
    w.u64(to_u64(dst));
    w.u32(dstLayout);
    w.u32(count);
    auto write_subres = [&w](const VkImageSubresourceLayers& s) {
        w.u32(s.aspectMask);
        w.u32(s.mipLevel);
        w.u32(s.baseArrayLayer);
        w.u32(s.layerCount);
    };
    for (uint32_t i = 0; i < count; ++i) {
        const auto& b = pRegions[i];
        write_subres(b.srcSubresource);
        for (int j = 0; j < 2; ++j) {
            w.u32(static_cast<uint32_t>(b.srcOffsets[j].x));
            w.u32(static_cast<uint32_t>(b.srcOffsets[j].y));
            w.u32(static_cast<uint32_t>(b.srcOffsets[j].z));
        }
        write_subres(b.dstSubresource);
        for (int j = 0; j < 2; ++j) {
            w.u32(static_cast<uint32_t>(b.dstOffsets[j].x));
            w.u32(static_cast<uint32_t>(b.dstOffsets[j].y));
            w.u32(static_cast<uint32_t>(b.dstOffsets[j].z));
        }
    }
    w.u32(filter);
    record(cb, vk_wire::CmdKind::BlitImage, in);
}

// VkImageResolve has exactly VkImageCopy's layout (src subresource and
// offset, dst subresource and offset, extent), so it travels in exactly
// vkCmdCopyImage's wire shape.
VKAPI_ATTR void VKAPI_CALL stud_vkCmdResolveImage(VkCommandBuffer cb, VkImage src,
                                                   VkImageLayout srcLayout, VkImage dst,
                                                   VkImageLayout dstLayout, uint32_t count,
                                                   const VkImageResolve* pRegions) {
    std::vector<uint8_t>& in = wire_scratch();
    vk_wire::Writer w(in);
    w.u64(to_u64(src));
    w.u32(srcLayout);
    w.u64(to_u64(dst));
    w.u32(dstLayout);
    w.u32(count);
    auto write_subres = [&w](const VkImageSubresourceLayers& s) {
        w.u32(s.aspectMask);
        w.u32(s.mipLevel);
        w.u32(s.baseArrayLayer);
        w.u32(s.layerCount);
    };
    for (uint32_t i = 0; i < count; ++i) {
        const auto& c = pRegions[i];
        write_subres(c.srcSubresource);
        w.u32(static_cast<uint32_t>(c.srcOffset.x));
        w.u32(static_cast<uint32_t>(c.srcOffset.y));
        w.u32(static_cast<uint32_t>(c.srcOffset.z));
        write_subres(c.dstSubresource);
        w.u32(static_cast<uint32_t>(c.dstOffset.x));
        w.u32(static_cast<uint32_t>(c.dstOffset.y));
        w.u32(static_cast<uint32_t>(c.dstOffset.z));
        w.u32(c.extent.width);
        w.u32(c.extent.height);
        w.u32(c.extent.depth);
    }
    record(cb, vk_wire::CmdKind::ResolveImage, in);
}

VKAPI_ATTR void VKAPI_CALL stud_vkCmdResetQueryPool(VkCommandBuffer cb, VkQueryPool pool,
                                                     uint32_t first, uint32_t count) {
    std::vector<uint8_t>& in = wire_scratch();
    vk_wire::Writer w(in);
    w.u64(to_u64(pool));
    w.u32(first);
    w.u32(count);
    record(cb, vk_wire::CmdKind::ResetQueryPool, in);
}

VKAPI_ATTR void VKAPI_CALL stud_vkCmdWriteTimestamp(VkCommandBuffer cb,
                                                     VkPipelineStageFlagBits stage,
                                                     VkQueryPool pool, uint32_t query) {
    std::vector<uint8_t>& in = wire_scratch();
    vk_wire::Writer w(in);
    w.u32(stage);
    w.u64(to_u64(pool));
    w.u32(query);
    record(cb, vk_wire::CmdKind::WriteTimestamp, in);
}

// One table for both resolvers. Roblox resolves instance-level commands
// through vkGetInstanceProcAddr and device-level ones through
// vkGetDeviceProcAddr, and a command implemented here has to be found by
// whichever one asks, a real bug caught live: the device commands were
// implemented but only the instance resolver consulted the table, so
// every one of them still resolved to an unimplemented stub.
PFN_vkVoidFunction lookup_command(const char* pName) {
    struct Entry {
        const char* name = nullptr;
        PFN_vkVoidFunction fn = nullptr;
    };
    static const Entry kCommands[] = {
        {"vkEnumerateInstanceVersion", reinterpret_cast<PFN_vkVoidFunction>(&stud_vkEnumerateInstanceVersion)},
        {"vkEnumerateInstanceExtensionProperties",
         reinterpret_cast<PFN_vkVoidFunction>(&stud_vkEnumerateInstanceExtensionProperties)},
        {"vkEnumerateInstanceLayerProperties",
         reinterpret_cast<PFN_vkVoidFunction>(&stud_vkEnumerateInstanceLayerProperties)},
        {"vkCreateInstance", reinterpret_cast<PFN_vkVoidFunction>(&stud_vkCreateInstance)},
        {"vkEnumeratePhysicalDevices",
         reinterpret_cast<PFN_vkVoidFunction>(&stud_vkEnumeratePhysicalDevices)},
        {"vkGetPhysicalDeviceProperties",
         reinterpret_cast<PFN_vkVoidFunction>(&stud_vkGetPhysicalDeviceProperties)},
        {"vkGetPhysicalDeviceFeatures",
         reinterpret_cast<PFN_vkVoidFunction>(&stud_vkGetPhysicalDeviceFeatures)},
        {"vkGetPhysicalDeviceMemoryProperties",
         reinterpret_cast<PFN_vkVoidFunction>(&stud_vkGetPhysicalDeviceMemoryProperties)},
        {"vkGetPhysicalDeviceQueueFamilyProperties",
         reinterpret_cast<PFN_vkVoidFunction>(&stud_vkGetPhysicalDeviceQueueFamilyProperties)},
        {"vkEnumerateDeviceExtensionProperties",
         reinterpret_cast<PFN_vkVoidFunction>(&stud_vkEnumerateDeviceExtensionProperties)},
        {"vkGetPhysicalDeviceFeatures2",
         reinterpret_cast<PFN_vkVoidFunction>(&stud_vkGetPhysicalDeviceFeatures2)},
        {"vkGetPhysicalDeviceFeatures2KHR",
         reinterpret_cast<PFN_vkVoidFunction>(&stud_vkGetPhysicalDeviceFeatures2)},
        {"vkCreateDevice", reinterpret_cast<PFN_vkVoidFunction>(&stud_vkCreateDevice)},
        {"vkGetPhysicalDeviceFormatProperties",
         reinterpret_cast<PFN_vkVoidFunction>(&stud_vkGetPhysicalDeviceFormatProperties)},
        {"vkGetPhysicalDeviceImageFormatProperties",
         reinterpret_cast<PFN_vkVoidFunction>(&stud_vkGetPhysicalDeviceImageFormatProperties)},
        {"vkGetDeviceQueue", reinterpret_cast<PFN_vkVoidFunction>(&stud_vkGetDeviceQueue)},
        {"vkCreateCommandPool",
         reinterpret_cast<PFN_vkVoidFunction>(&stud_vkCreateCommandPool)},
        {"vkCreateSemaphore", reinterpret_cast<PFN_vkVoidFunction>(&stud_vkCreateSemaphore)},
        {"vkCreateFence", reinterpret_cast<PFN_vkVoidFunction>(&stud_vkCreateFence)},
        {"vkCreateQueryPool", reinterpret_cast<PFN_vkVoidFunction>(&stud_vkCreateQueryPool)},
        {"vkCreatePipelineCache",
         reinterpret_cast<PFN_vkVoidFunction>(&stud_vkCreatePipelineCache)},
        {"vkGetPipelineCacheData",
         reinterpret_cast<PFN_vkVoidFunction>(&stud_vkGetPipelineCacheData)},
        {"vkDestroyPipelineCache",
         reinterpret_cast<PFN_vkVoidFunction>(&stud_vkDestroyPipelineCache)},
        {"vkCreateImage", reinterpret_cast<PFN_vkVoidFunction>(&stud_vkCreateImage)},
        {"vkGetImageMemoryRequirements",
         reinterpret_cast<PFN_vkVoidFunction>(&stud_vkGetImageMemoryRequirements)},
        {"vkGetPhysicalDeviceSurfaceCapabilitiesKHR",
         reinterpret_cast<PFN_vkVoidFunction>(&stud_vkGetPhysicalDeviceSurfaceCapabilitiesKHR)},
        {"vkDeviceWaitIdle", reinterpret_cast<PFN_vkVoidFunction>(&stud_vkDeviceWaitIdle)},
        {"vkCreateBuffer", reinterpret_cast<PFN_vkVoidFunction>(&stud_vkCreateBuffer)},
        {"vkGetBufferMemoryRequirements",
         reinterpret_cast<PFN_vkVoidFunction>(&stud_vkGetBufferMemoryRequirements)},
        {"vkBindBufferMemory", reinterpret_cast<PFN_vkVoidFunction>(&stud_vkBindBufferMemory)},
        {"vkCreateImageView", reinterpret_cast<PFN_vkVoidFunction>(&stud_vkCreateImageView)},
        {"vkCreateShaderModule",
         reinterpret_cast<PFN_vkVoidFunction>(&stud_vkCreateShaderModule)},
        {"vkDestroyBuffer", reinterpret_cast<PFN_vkVoidFunction>(&stud_vkDestroyBuffer)},
        {"vkDestroyImage", reinterpret_cast<PFN_vkVoidFunction>(&stud_vkDestroyImage)},
        {"vkDestroyImageView", reinterpret_cast<PFN_vkVoidFunction>(&stud_vkDestroyImageView)},
        {"vkDestroyShaderModule",
         reinterpret_cast<PFN_vkVoidFunction>(&stud_vkDestroyShaderModule)},
        {"vkDestroySemaphore", reinterpret_cast<PFN_vkVoidFunction>(&stud_vkDestroySemaphore)},
        {"vkDestroyFence", reinterpret_cast<PFN_vkVoidFunction>(&stud_vkDestroyFence)},
        {"vkDestroyCommandPool",
         reinterpret_cast<PFN_vkVoidFunction>(&stud_vkDestroyCommandPool)},
        {"vkDestroyQueryPool", reinterpret_cast<PFN_vkVoidFunction>(&stud_vkDestroyQueryPool)},
        {"vkDestroySwapchainKHR",
         reinterpret_cast<PFN_vkVoidFunction>(&stud_vkDestroySwapchainKHR)},
        {"vkDestroySurfaceKHR", reinterpret_cast<PFN_vkVoidFunction>(&stud_vkDestroySurfaceKHR)},
        {"vkDestroyDevice", reinterpret_cast<PFN_vkVoidFunction>(&stud_vkDestroyDevice)},
        {"vkDestroyInstance", reinterpret_cast<PFN_vkVoidFunction>(&stud_vkDestroyInstance)},
        {"vkCreateRenderPass", reinterpret_cast<PFN_vkVoidFunction>(&stud_vkCreateRenderPass)},
        {"vkCreateFramebuffer", reinterpret_cast<PFN_vkVoidFunction>(&stud_vkCreateFramebuffer)},
        {"vkCreateSampler", reinterpret_cast<PFN_vkVoidFunction>(&stud_vkCreateSampler)},
        {"vkCreateDescriptorSetLayout",
         reinterpret_cast<PFN_vkVoidFunction>(&stud_vkCreateDescriptorSetLayout)},
        {"vkCreatePipelineLayout",
         reinterpret_cast<PFN_vkVoidFunction>(&stud_vkCreatePipelineLayout)},
        {"vkCreateDescriptorPool",
         reinterpret_cast<PFN_vkVoidFunction>(&stud_vkCreateDescriptorPool)},
        {"vkAllocateDescriptorSets",
         reinterpret_cast<PFN_vkVoidFunction>(&stud_vkAllocateDescriptorSets)},
        {"vkResetDescriptorPool",
         reinterpret_cast<PFN_vkVoidFunction>(&stud_vkResetDescriptorPool)},
        {"vkCreateDescriptorUpdateTemplate",
         reinterpret_cast<PFN_vkVoidFunction>(&stud_vkCreateDescriptorUpdateTemplate)},
        {"vkCreateDescriptorUpdateTemplateKHR",
         reinterpret_cast<PFN_vkVoidFunction>(&stud_vkCreateDescriptorUpdateTemplate)},
        {"vkUpdateDescriptorSetWithTemplate",
         reinterpret_cast<PFN_vkVoidFunction>(&stud_vkUpdateDescriptorSetWithTemplate)},
        {"vkUpdateDescriptorSetWithTemplateKHR",
         reinterpret_cast<PFN_vkVoidFunction>(&stud_vkUpdateDescriptorSetWithTemplate)},
        {"vkCreateGraphicsPipelines",
         reinterpret_cast<PFN_vkVoidFunction>(&stud_vkCreateGraphicsPipelines)},
        {"vkCreateComputePipelines",
         reinterpret_cast<PFN_vkVoidFunction>(&stud_vkCreateComputePipelines)},
        {"vkAllocateCommandBuffers",
         reinterpret_cast<PFN_vkVoidFunction>(&stud_vkAllocateCommandBuffers)},
        {"vkBeginCommandBuffer",
         reinterpret_cast<PFN_vkVoidFunction>(&stud_vkBeginCommandBuffer)},
        {"vkEndCommandBuffer", reinterpret_cast<PFN_vkVoidFunction>(&stud_vkEndCommandBuffer)},
        {"vkResetCommandPool", reinterpret_cast<PFN_vkVoidFunction>(&stud_vkResetCommandPool)},
        {"vkQueueSubmit", reinterpret_cast<PFN_vkVoidFunction>(&stud_vkQueueSubmit)},
        {"vkWaitForFences", reinterpret_cast<PFN_vkVoidFunction>(&stud_vkWaitForFences)},
        {"vkResetFences", reinterpret_cast<PFN_vkVoidFunction>(&stud_vkResetFences)},
        {"vkAcquireNextImageKHR",
         reinterpret_cast<PFN_vkVoidFunction>(&stud_vkAcquireNextImageKHR)},
        {"vkQueuePresentKHR", reinterpret_cast<PFN_vkVoidFunction>(&stud_vkQueuePresentKHR)},
        {"vkGetRefreshCycleDurationGOOGLE",
         reinterpret_cast<PFN_vkVoidFunction>(&stud_vkGetRefreshCycleDurationGOOGLE)},
        {"vkGetPastPresentationTimingGOOGLE",
         reinterpret_cast<PFN_vkVoidFunction>(&stud_vkGetPastPresentationTimingGOOGLE)},
        {"vkGetQueryPoolResults",
         reinterpret_cast<PFN_vkVoidFunction>(&stud_vkGetQueryPoolResults)},
        {"vkCmdBeginRenderPass",
         reinterpret_cast<PFN_vkVoidFunction>(&stud_vkCmdBeginRenderPass)},
        {"vkCmdEndRenderPass", reinterpret_cast<PFN_vkVoidFunction>(&stud_vkCmdEndRenderPass)},
        {"vkCmdBindPipeline", reinterpret_cast<PFN_vkVoidFunction>(&stud_vkCmdBindPipeline)},
        {"vkCmdBindDescriptorSets",
         reinterpret_cast<PFN_vkVoidFunction>(&stud_vkCmdBindDescriptorSets)},
        {"vkCmdBindVertexBuffers",
         reinterpret_cast<PFN_vkVoidFunction>(&stud_vkCmdBindVertexBuffers)},
        {"vkCmdBindIndexBuffer",
         reinterpret_cast<PFN_vkVoidFunction>(&stud_vkCmdBindIndexBuffer)},
        {"vkCmdDraw", reinterpret_cast<PFN_vkVoidFunction>(&stud_vkCmdDraw)},
        {"vkCmdDrawIndexed", reinterpret_cast<PFN_vkVoidFunction>(&stud_vkCmdDrawIndexed)},
        {"vkCmdDispatch", reinterpret_cast<PFN_vkVoidFunction>(&stud_vkCmdDispatch)},
        {"vkCmdSetViewport", reinterpret_cast<PFN_vkVoidFunction>(&stud_vkCmdSetViewport)},
        {"vkCmdSetScissor", reinterpret_cast<PFN_vkVoidFunction>(&stud_vkCmdSetScissor)},
        {"vkCmdPipelineBarrier",
         reinterpret_cast<PFN_vkVoidFunction>(&stud_vkCmdPipelineBarrier)},
        {"vkCmdCopyBuffer", reinterpret_cast<PFN_vkVoidFunction>(&stud_vkCmdCopyBuffer)},
        {"vkCmdCopyBufferToImage",
         reinterpret_cast<PFN_vkVoidFunction>(&stud_vkCmdCopyBufferToImage)},
        {"vkCmdCopyImageToBuffer",
         reinterpret_cast<PFN_vkVoidFunction>(&stud_vkCmdCopyImageToBuffer)},
        {"vkCmdCopyImage", reinterpret_cast<PFN_vkVoidFunction>(&stud_vkCmdCopyImage)},
        {"vkCmdBlitImage", reinterpret_cast<PFN_vkVoidFunction>(&stud_vkCmdBlitImage)},
        {"vkCmdResolveImage", reinterpret_cast<PFN_vkVoidFunction>(&stud_vkCmdResolveImage)},
        {"vkCmdResetQueryPool",
         reinterpret_cast<PFN_vkVoidFunction>(&stud_vkCmdResetQueryPool)},
        {"vkCmdWriteTimestamp",
         reinterpret_cast<PFN_vkVoidFunction>(&stud_vkCmdWriteTimestamp)},
        {"vkDestroySampler", reinterpret_cast<PFN_vkVoidFunction>(&stud_vkDestroySampler)},
        {"vkDestroyRenderPass", reinterpret_cast<PFN_vkVoidFunction>(&stud_vkDestroyRenderPass)},
        {"vkDestroyPipeline", reinterpret_cast<PFN_vkVoidFunction>(&stud_vkDestroyPipeline)},
        {"vkDestroyPipelineLayout",
         reinterpret_cast<PFN_vkVoidFunction>(&stud_vkDestroyPipelineLayout)},
        {"vkDestroyDescriptorSetLayout",
         reinterpret_cast<PFN_vkVoidFunction>(&stud_vkDestroyDescriptorSetLayout)},
        {"vkDestroyDescriptorPool",
         reinterpret_cast<PFN_vkVoidFunction>(&stud_vkDestroyDescriptorPool)},
        {"vkDestroyDescriptorUpdateTemplate",
         reinterpret_cast<PFN_vkVoidFunction>(&stud_vkDestroyDescriptorUpdateTemplate)},
        {"vkDestroyDescriptorUpdateTemplateKHR",
         reinterpret_cast<PFN_vkVoidFunction>(&stud_vkDestroyDescriptorUpdateTemplate)},
        {"vkDestroyFramebuffer",
         reinterpret_cast<PFN_vkVoidFunction>(&stud_vkDestroyFramebuffer)},

        {"vkAllocateMemory", reinterpret_cast<PFN_vkVoidFunction>(&stud_vkAllocateMemory)},
        {"vkBindImageMemory", reinterpret_cast<PFN_vkVoidFunction>(&stud_vkBindImageMemory)},
        {"vkFreeMemory", reinterpret_cast<PFN_vkVoidFunction>(&stud_vkFreeMemory)},
        {"vkMapMemory", reinterpret_cast<PFN_vkVoidFunction>(&stud_vkMapMemory)},
        {"vkUnmapMemory", reinterpret_cast<PFN_vkVoidFunction>(&stud_vkUnmapMemory)},
        {"vkFlushMappedMemoryRanges",
         reinterpret_cast<PFN_vkVoidFunction>(&stud_vkFlushMappedMemoryRanges)},
        {"vkInvalidateMappedMemoryRanges",
         reinterpret_cast<PFN_vkVoidFunction>(&stud_vkInvalidateMappedMemoryRanges)},
        {"vkGetPhysicalDeviceSurfaceFormatsKHR",
         reinterpret_cast<PFN_vkVoidFunction>(&stud_vkGetPhysicalDeviceSurfaceFormatsKHR)},
        {"vkGetPhysicalDeviceSurfacePresentModesKHR",
         reinterpret_cast<PFN_vkVoidFunction>(&stud_vkGetPhysicalDeviceSurfacePresentModesKHR)},
        {"vkGetPhysicalDeviceSurfaceSupportKHR",
         reinterpret_cast<PFN_vkVoidFunction>(&stud_vkGetPhysicalDeviceSurfaceSupportKHR)},
        {"vkCreateSwapchainKHR",
         reinterpret_cast<PFN_vkVoidFunction>(&stud_vkCreateSwapchainKHR)},
        {"vkGetSwapchainImagesKHR",
         reinterpret_cast<PFN_vkVoidFunction>(&stud_vkGetSwapchainImagesKHR)},
        {"vkGetPhysicalDeviceImageFormatProperties2",
         reinterpret_cast<PFN_vkVoidFunction>(&stud_vkGetPhysicalDeviceImageFormatProperties2)},
        {"vkGetPhysicalDeviceImageFormatProperties2KHR",
         reinterpret_cast<PFN_vkVoidFunction>(&stud_vkGetPhysicalDeviceImageFormatProperties2)},
        {"vkCreateAndroidSurfaceKHR",
         reinterpret_cast<PFN_vkVoidFunction>(&stud_vkCreateAndroidSurfaceKHR)},
    };
    for (const Entry& e : kCommands) {
        if (std::strcmp(pName, e.name) == 0) return e.fn;
    }
    return nullptr;
}

VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL vkGetInstanceProcAddr(VkInstance /*instance*/,
                                                                const char* pName) {
    trace("vkGetInstanceProcAddr", pName);
    if (pName == nullptr) return nullptr;

    // STUD_DISABLE_VULKAN=1 declines everything, which makes the engine
    // report "Unable to load Vulkan API" and fall back to its GLES
    // renderer. Declining is spec-legal, so this is an honest "not
    // available" rather than a fault injected to force a path.
    //
    // Worth keeping even once Vulkan is fully working: it is the one
    // switch that answers "is this a Vulkan problem or not" in a single
    // run, and it leaves a route back to the GL path if a Vulkan-side
    // regression makes the window unusable.
    static const bool vulkan_disabled = [] {
        const char* v = std::getenv("STUD_DISABLE_VULKAN");
        return v != nullptr && std::string_view(v) == "1";
    }();
    if (vulkan_disabled) return nullptr;

    if (PFN_vkVoidFunction fn = lookup_command(pName)) return fn;

    if (std::strcmp(pName, "vkGetDeviceProcAddr") == 0) {
        return reinterpret_cast<PFN_vkVoidFunction>(
            +[](VkDevice /*device*/, const char* inner_name) -> PFN_vkVoidFunction {
                trace("vkGetDeviceProcAddr", inner_name);
                if (inner_name == nullptr) return nullptr;
                if (PFN_vkVoidFunction fn = lookup_command(inner_name)) return fn;
                // Anything the real driver does not have is answered
                // null, exactly as that driver would; see
                // host_has_proc() for why claiming otherwise actively
                // misleads the engine.
                if (!host_has_proc(inner_name)) return nullptr;
                // Device-level commands the driver DOES have go through
                // the SAME named-stub table as the instance resolver.
                // Returning nullptr for these was a real crash: the
                // engine resolves its device dispatch table up front and
                // calls through it without null-checking, so the first
                // device call jumped to address zero (live-caught right
                // after the engine printed its own "Vulkan Android
                // Device" banner).
                return unimplemented_stub_for(inner_name);
            });
    }

    // Everything past instance creation is unimplemented, but do NOT
    // hand back nullptr. Roblox resolves its whole ~590-command dispatch
    // table up front and then calls through it without null-checking, so
    // a null slot becomes a jump to address zero on the render thread
    // (live-caught: `sigsegv addr=0x0 ... pc=0x0`, recovered only because
    // trap_recovery classifies it as near_null).
    //
    // Return a named stub instead, exactly as the GL path already does
    // for unimplemented entry points. It reports which command was
    // genuinely CALLED, as opposed to merely resolved, which all 590
    // are, and returns VK_ERROR_INITIALIZATION_FAILED, a real error
    // code the engine can act on rather than a crash. That distinction
    // is what makes the remaining work tractable: only the called set
    // has to be built, and this names it.
    // Same honesty rule as the device resolver above: a command the real
    // driver does not have is answered null.
    if (!host_has_proc(pName)) return nullptr;
    return unimplemented_stub_for(pName);
}

}  // extern "C"
