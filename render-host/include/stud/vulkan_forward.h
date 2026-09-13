// Wire format for the Vulkan commands forwarded from Process B's
// libvulkan.so.1 stub to Process C, where the real driver lives.
//
// Why forward at all, rather than loading a real Vulkan driver in
// Process B: the vendor GPU driver must never run inside a process
// sharing bionic or foreign TLS (see the engineering notes' non-negotiable
// constraints -- that is the failure class this whole architecture
// exists to avoid). Process C is ordinary glibc, so the real driver is
// at home there, and Process B only ever sees Stud's own stub.
//
// Vulkan suits this transport far better than GLES did. Nearly every
// hot call is a vkCmd* that records into a command buffer and returns
// void, which is exactly what the protocol's kNoReply pipelining is for
// -- where GLES forced a blocking round-trip on glGetError alone 856
// times a frame. The costly parts here are the ones that genuinely need
// an answer: memory mapping, swapchain acquire, and queue submit.
//
// Structs are flattened rather than memcpy'd wholesale: a real
// VkInstanceCreateInfo is a pointer graph (pApplicationInfo, two arrays
// of C strings, a pNext chain), none of which survives a socket. Only
// the fields the real engine actually sets are carried, and every one
// of them is documented against the command it belongs to.

#ifndef STUD_VULKAN_FORWARD_H
#define STUD_VULKAN_FORWARD_H

#include <cstdint>
#include <cstring>
#include <vector>

namespace stud::render_host::vk_wire {

// Both sides agree on these sizes so the property arrays can travel as
// plain fixed-stride blocks. They match the real Vulkan header
// constants; static_asserts on the host keep that honest.
inline constexpr uint32_t kMaxExtensionNameSize = 256;
inline constexpr uint32_t kMaxDescriptionSize = 256;

// VkExtensionProperties, flattened. Fixed stride, so an array of these
// is just a block of bytes.
struct ExtensionProperties {
    char extension_name[kMaxExtensionNameSize];
    uint32_t spec_version;
};

// VkLayerProperties, flattened.
struct LayerProperties {
    char layer_name[kMaxExtensionNameSize];
    uint32_t spec_version;
    uint32_t implementation_version;
    char description[kMaxDescriptionSize];
};

// Header of the in-buffer for VkCreateInstance. Followed by, in order:
//   - application_name_len bytes of application name (no NUL)
//   - engine_name_len bytes of engine name (no NUL)
//   - enabled_layer_count NUL-terminated layer names
//   - enabled_extension_count NUL-terminated extension names
//
// has_application_info distinguishes a real VkApplicationInfo from a
// null pApplicationInfo, which is legal and means "no preference".
struct CreateInstanceHeader {
    uint32_t flags;
    uint32_t has_application_info;
    uint32_t application_version;
    uint32_t engine_version;
    uint32_t api_version;
    uint32_t application_name_len;
    uint32_t engine_name_len;
    uint32_t enabled_layer_count;
    uint32_t enabled_extension_count;
};

// ---- pNext chains ----------------------------------------------------
//
// A pNext chain is a linked list of structs that cannot travel as
// pointers, and its nodes cannot be sized from sType alone without a
// table. Both processes compile this one, so they agree by construction
// -- important because they build against different Vulkan headers (the
// NDK's in Process B, the system's in Process C).
//
// Only the sTypes the engine is actually observed to use are listed. An
// unknown sType is reported by name-less id and dropped rather than
// guessed at, which is a safe degradation: the driver then simply does
// not see that extension struct.
// vkCreateDevice's in-buffer: this header, then
//   - queue_create_count x {flags, family_index, queue_count} plus that
//     many float priorities each
//   - enabled_layer_count NUL-terminated layer names
//   - enabled_extension_count NUL-terminated extension names
//   - VkPhysicalDeviceFeatures body, if has_enabled_features
//   - chain_node_count x (ChainNodeHeader + body)
struct CreateDeviceHeader {
    uint32_t flags;
    uint32_t queue_create_count;
    uint32_t enabled_layer_count;
    uint32_t enabled_extension_count;
    uint32_t has_enabled_features;
    uint32_t enabled_features_size;
    uint32_t chain_node_count;
};

struct DeviceQueueCreateHeader {
    uint32_t flags;
    uint32_t queue_family_index;
    uint32_t queue_count;
};

// vkCreateImage's in-buffer: this header, then queue_family_count
// uint32 indices, then chain_node_count flattened pNext nodes.
struct CreateImageHeader {
    uint32_t flags;
    uint32_t image_type;
    uint32_t format;
    uint32_t extent_width;
    uint32_t extent_height;
    uint32_t extent_depth;
    uint32_t mip_levels;
    uint32_t array_layers;
    uint32_t samples;
    uint32_t tiling;
    uint32_t usage;
    uint32_t sharing_mode;
    uint32_t queue_family_count;
    uint32_t initial_layout;
    uint32_t chain_node_count;
};

// vkCreateSwapchainKHR's in-buffer: this header, then
// queue_family_count uint32 indices.
struct CreateSwapchainHeader {
    uint64_t surface;
    uint64_t old_swapchain;
    uint32_t flags;
    uint32_t min_image_count;
    uint32_t image_format;
    uint32_t image_color_space;
    uint32_t width;
    uint32_t height;
    uint32_t image_array_layers;
    uint32_t image_usage;
    uint32_t sharing_mode;
    uint32_t queue_family_count;
    uint32_t pre_transform;
    uint32_t composite_alpha;
    uint32_t present_mode;
    uint32_t clipped;
};

// Which kind of object a VkDestroyHandle call refers to. One call id
// covers them all: every vkDestroyX has the same shape (device, handle,
// allocator) and returns nothing.
enum class DestroyKind : uint32_t {
    Buffer = 1,
    Image,
    ImageView,
    ShaderModule,
    Semaphore,
    Fence,
    CommandPool,
    QueryPool,
    Swapchain,
    Surface,
    Device,
    Instance,
    PipelineCache,
    Sampler,
    RenderPass,
    Framebuffer,
    Pipeline,
    PipelineLayout,
    DescriptorSetLayout,
    DescriptorPool,
    Event,
    // Appended, so every kind above keeps its value.
    DescriptorUpdateTemplate,
};

struct ChainNodeHeader {
    uint32_t s_type;
    uint32_t size;  // bytes of the struct body that follows, header included
};

// Returns 0 for an sType this build does not know how to size.
uint32_t chain_node_size(uint32_t s_type);

// Which vkCmd* a VkCmdRecord call carries. One call id for the whole
// recording family: they all take a command buffer, return nothing, and
// differ only in payload.
enum class CmdKind : uint32_t {
    BeginRenderPass = 1,
    EndRenderPass,
    BindPipeline,
    BindDescriptorSets,
    BindVertexBuffers,
    BindIndexBuffer,
    Draw,
    DrawIndexed,
    Dispatch,
    SetViewport,
    SetScissor,
    PipelineBarrier,
    CopyBuffer,
    CopyBufferToImage,
    CopyImage,
    BlitImage,
    ResetQueryPool,
    WriteTimestamp,
    // The multisample resolve: copies an MSAA image into the single-sample
    // one the frame is actually built from. Appended so every existing
    // kind keeps its value. Missing, it was a named no-op stub -- and the
    // engine turns MSAA on in game at small framebuffer sizes, so every
    // resolved frame came out black while Home, which does not resolve,
    // was fine.
    ResolveImage,
    // Appended, so every kind above keeps its value.
    CopyImageToBuffer,

};

// ---- compact serializer ---------------------------------------------
//
// The command-recording family is large and every member is a different
// mix of scalars and small arrays. Hand-writing a marshaller per command
// would be both enormous and easy to get subtly wrong in one direction
// only, so both sides share these two.
//
// Writer/Reader are deliberately dumb: fixed-width little-endian scalars
// and length-prefixed arrays, in the order written. Reader never reads
// past the end -- a truncated or mismatched message yields zeros rather
// than reading adjacent memory, which matters because the payload
// crosses a process boundary.
class Writer {
public:
    explicit Writer(std::vector<uint8_t>& out) : out_(out) {}

    template <typename T>
    void scalar(T v) {
        const auto* p = reinterpret_cast<const uint8_t*>(&v);
        out_.insert(out_.end(), p, p + sizeof(T));
    }
    void u32(uint32_t v) { scalar(v); }
    void u64(uint64_t v) { scalar(v); }
    void f32(float v) { scalar(v); }

    // Length-prefixed raw block.
    void bytes(const void* p, size_t n) {
        u32(static_cast<uint32_t>(n));
        if (n > 0 && p != nullptr) {
            const auto* b = static_cast<const uint8_t*>(p);
            out_.insert(out_.end(), b, b + n);
        }
    }

private:
    std::vector<uint8_t>& out_;
};

// The same wire format written into a fixed stack buffer.
//
// A command is small -- a draw is four integers -- but the engine issues
// over ten thousand of them per frame, and building each one through a
// std::vector's append path (capacity check, iterator machinery, a
// possible reallocation) is measurable at that rate: it was the largest
// single item in Stud's own share of the frame. Commands that fit go
// through here instead and touch no allocator at all; `overflowed()`
// reports the few that do not, so a caller can fall back rather than
// silently truncate.
template <size_t N>
class FixedWriter {
public:
    template <typename T>
    void scalar(T v) {
        append(&v, sizeof(T));
    }
    void u32(uint32_t v) { scalar(v); }
    void u64(uint64_t v) { scalar(v); }
    void f32(float v) { scalar(v); }
    void bytes(const void* p, size_t n) {
        u32(static_cast<uint32_t>(n));
        if (n > 0 && p != nullptr) append(p, n);
    }

    const uint8_t* data() const { return spilled_.empty() ? buf_ : spilled_.data(); }
    size_t size() const { return spilled_.empty() ? n_ : spilled_.size(); }

private:
    // Anything too big for the stack buffer spills to the heap rather than
    // being truncated. Silently dropping the tail of a command would
    // corrupt the stream in a way that is very hard to see, and the whole
    // point of the fixed buffer is the common small case -- a draw, a
    // bind -- not a guarantee about the rare large one.
    void append(const void* p, size_t n) {
        if (!spilled_.empty()) {
            const auto* b = static_cast<const uint8_t*>(p);
            spilled_.insert(spilled_.end(), b, b + n);
            return;
        }
        if (n_ + n <= N) {
            std::memcpy(buf_ + n_, p, n);
            n_ += n;
            return;
        }
        spilled_.assign(buf_, buf_ + n_);
        const auto* b = static_cast<const uint8_t*>(p);
        spilled_.insert(spilled_.end(), b, b + n);
    }

    uint8_t buf_[N];
    size_t n_ = 0;
    std::vector<uint8_t> spilled_;
};

class Reader {
public:
    Reader(const uint8_t* data, size_t size) : p_(data), end_(data + size) {}

    template <typename T>
    T scalar() {
        T v{};
        if (static_cast<size_t>(end_ - p_) >= sizeof(T)) {
            std::memcpy(&v, p_, sizeof(T));
            p_ += sizeof(T);
        } else {
            p_ = end_;
        }
        return v;
    }
    uint32_t u32() { return scalar<uint32_t>(); }
    uint64_t u64() { return scalar<uint64_t>(); }
    float f32() { return scalar<float>(); }

    // Returns a pointer into the message itself; valid while it is.
    const uint8_t* bytes(uint32_t* out_len) {
        const uint32_t n = u32();
        if (static_cast<size_t>(end_ - p_) < n) {
            p_ = end_;
            *out_len = 0;
            return nullptr;
        }
        const uint8_t* at = p_;
        p_ += n;
        *out_len = n;
        return at;
    }

    bool ok() const { return p_ <= end_; }

private:
    const uint8_t* p_;
    const uint8_t* end_;
};

}  // namespace stud::render_host::vk_wire

#endif  // STUD_VULKAN_FORWARD_H
