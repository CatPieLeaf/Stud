#include "owned_swapchain.h"

#include <drm_fourcc.h>
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/sysmacros.h>
#include <unistd.h>
#include <wayland-client.h>
#include <xf86drm.h>

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <map>
#include <mutex>
#include <set>
#include <utility>

#include "linux-dmabuf-v1-client-protocol.h"
#include "linux-drm-syncobj-v1-client-protocol.h"

namespace stud::render_host::owned_swapchain {
namespace {

// What one compositor connection offers. Everything Stud binds here lives
// on an event queue of its own, so neither the driver's default queue nor
// android-glue's ever sees these objects' events.
struct Compositor {
    wl_display* display = nullptr;
    wl_event_queue* queue = nullptr;
    zwp_linux_dmabuf_v1* dmabuf = nullptr;
    wp_linux_drm_syncobj_manager_v1* syncobj = nullptr;
    // The GPU the compositor composites on, from its dma-buf feedback.
    dev_t main_device = 0;
    bool have_main_device = false;
    // Every (DRM format, modifier) it says it can import.
    std::set<std::pair<uint32_t, uint64_t>> importable;
    // A render node of that GPU, for the timeline syncobjs explicit sync
    // passes. Any DRM device's syncobj can be shared; the compositor's own
    // is the one certain to support timelines if it offers the protocol.
    int drm_fd = -1;
    uint32_t scratch = 0;  // a binary syncobj sync_files are imported through
    // Feedback being parsed.
    const uint8_t* table = nullptr;
    size_t table_size = 0;
    bool feedback_done = false;
};

struct Target {
    Compositor* compositor = nullptr;
    wl_surface* surface = nullptr;
};

// A buffer the compositor gets: an exported image, its wl_buffer, and the
// timeline its acquire and release points live on.
struct Buffer {
    VkImage image = VK_NULL_HANDLE;
    VkDeviceMemory memory = VK_NULL_HANDLE;
    wl_buffer* wl = nullptr;
    uint32_t syncobj = 0;
    wp_linux_drm_syncobj_timeline_v1* timeline = nullptr;
    // The last release point handed to the compositor; 0 = never attached.
    uint64_t point = 0;
};

// An image the engine draws into.
struct Image {
    VkImage image = VK_NULL_HANDLE;
    VkDeviceMemory memory = VK_NULL_HANDLE;
    VkCommandBuffer cmd = VK_NULL_HANDLE;
    VkFence fence = VK_NULL_HANDLE;    // the last copy out of it
    VkSemaphore copied = VK_NULL_HANDLE;  // exportable: the same, as a sync_file
    int done_fd = -1;  // sync_file of the last copy out of it; -1 = nothing pending
    bool acquired = false;
};

// One VkDevice the layer sits on, and the next layer's commands for it.
// render-host can have several -- the engine's, ANGLE's own -- so nothing
// about a device is global.
struct Device {
    VolkDeviceTable vk{};
    VkDevice device = VK_NULL_HANDLE;
    VkPhysicalDevice physical_device = VK_NULL_HANDLE;
    uint32_t queue_family = 0;
    uint32_t foreign_family = VK_QUEUE_FAMILY_EXTERNAL;
};

struct Swapchain {
    Device* dev = nullptr;
    Target target;
    // Whether the compositor is on the GPU this device renders on.
    bool same_gpu = false;
    VkExtent2D extent{};
    VkFormat format = VK_FORMAT_UNDEFINED;
    uint32_t fourcc = 0;
    VkCommandPool pool = VK_NULL_HANDLE;
    std::vector<Image> images;
    std::vector<Buffer> buffers;
    uint32_t next_image = 0;
    uint32_t next_buffer = 0;
    uint64_t dropped = 0;
};

struct State {
    std::mutex mutex;
    std::map<wl_display*, Compositor> compositors;
    std::map<VkSurfaceKHR, Target> targets;
    std::map<wl_surface*, wp_linux_drm_syncobj_surface_v1*> syncobj_surfaces;
    // By dispatch key, which a device shares with its queues.
    std::map<void*, Device> devices;
    std::set<Swapchain*> swapchains;
};

State& state() {
    static State s;
    return s;
}

void* dispatch_key(const void* dispatchable) {
    return *static_cast<void* const*>(dispatchable);
}

Device* device_of(State& s, const void* dispatchable) {
    const auto it = s.devices.find(dispatch_key(dispatchable));
    return it == s.devices.end() ? nullptr : &it->second;
}

// --- the compositor ---------------------------------------------------

void on_global(void* data, wl_registry* registry, uint32_t name, const char* interface,
               uint32_t version) {
    auto* c = static_cast<Compositor*>(data);
    if (std::strcmp(interface, zwp_linux_dmabuf_v1_interface.name) == 0 && version >= 4) {
        c->dmabuf = static_cast<zwp_linux_dmabuf_v1*>(
            wl_registry_bind(registry, name, &zwp_linux_dmabuf_v1_interface, 4));
    } else if (std::strcmp(interface, wp_linux_drm_syncobj_manager_v1_interface.name) == 0) {
        c->syncobj = static_cast<wp_linux_drm_syncobj_manager_v1*>(
            wl_registry_bind(registry, name, &wp_linux_drm_syncobj_manager_v1_interface, 1));
    }
}
void on_global_remove(void*, wl_registry*, uint32_t) {}
const wl_registry_listener kRegistry = {on_global, on_global_remove};

void fb_done(void* data, zwp_linux_dmabuf_feedback_v1*) {
    static_cast<Compositor*>(data)->feedback_done = true;
}
void fb_format_table(void* data, zwp_linux_dmabuf_feedback_v1*, int32_t fd, uint32_t size) {
    auto* c = static_cast<Compositor*>(data);
    void* map = mmap(nullptr, size, PROT_READ, MAP_PRIVATE, fd, 0);
    close(fd);
    if (map == MAP_FAILED) return;
    c->table = static_cast<const uint8_t*>(map);
    c->table_size = size;
}
void fb_main_device(void* data, zwp_linux_dmabuf_feedback_v1*, wl_array* device) {
    auto* c = static_cast<Compositor*>(data);
    if (device->size != sizeof(dev_t)) return;
    std::memcpy(&c->main_device, device->data, sizeof(dev_t));
    c->have_main_device = true;
}
void fb_tranche_done(void*, zwp_linux_dmabuf_feedback_v1*) {}
void fb_tranche_target_device(void*, zwp_linux_dmabuf_feedback_v1*, wl_array*) {}
void fb_tranche_formats(void* data, zwp_linux_dmabuf_feedback_v1*, wl_array* indices) {
    auto* c = static_cast<Compositor*>(data);
    if (c->table == nullptr) return;
    // Each table entry: u32 format, u32 padding, u64 modifier.
    const auto* idx = static_cast<const uint16_t*>(indices->data);
    for (size_t i = 0; i < indices->size / sizeof(uint16_t); ++i) {
        const size_t at = static_cast<size_t>(idx[i]) * 16;
        if (at + 16 > c->table_size) continue;
        uint32_t format = 0;
        uint64_t modifier = 0;
        std::memcpy(&format, c->table + at, sizeof(format));
        std::memcpy(&modifier, c->table + at + 8, sizeof(modifier));
        c->importable.insert({format, modifier});
    }
}
void fb_tranche_flags(void*, zwp_linux_dmabuf_feedback_v1*, uint32_t) {}
const zwp_linux_dmabuf_feedback_v1_listener kFeedback = {
    fb_done, fb_format_table, fb_main_device, fb_tranche_done,
    fb_tranche_target_device, fb_tranche_formats, fb_tranche_flags};

void open_compositor(Compositor& c, wl_display* display) {
    c.display = display;
    c.queue = wl_display_create_queue(display);
    if (c.queue == nullptr) return;
    wl_registry* registry = wl_display_get_registry(display);
    wl_proxy_set_queue(reinterpret_cast<wl_proxy*>(registry), c.queue);
    wl_registry_add_listener(registry, &kRegistry, &c);
    wl_display_roundtrip_queue(display, c.queue);
    wl_registry_destroy(registry);
    if (c.dmabuf == nullptr || c.syncobj == nullptr) {
        std::printf("stud-render-host: the compositor has no %s; the driver presents\n",
                    c.dmabuf == nullptr ? "linux-dmabuf v4" : "linux-drm-syncobj");
        std::fflush(stdout);
        return;
    }

    zwp_linux_dmabuf_feedback_v1* feedback = zwp_linux_dmabuf_v1_get_default_feedback(c.dmabuf);
    zwp_linux_dmabuf_feedback_v1_add_listener(feedback, &kFeedback, &c);
    while (!c.feedback_done && wl_display_roundtrip_queue(display, c.queue) >= 0) {
    }
    zwp_linux_dmabuf_feedback_v1_destroy(feedback);
    if (c.table != nullptr) munmap(const_cast<uint8_t*>(c.table), c.table_size);
    c.table = nullptr;

    if (c.have_main_device) {
        drmDevicePtr dev = nullptr;
        if (drmGetDeviceFromDevId(c.main_device, 0, &dev) == 0) {
            const int node = (dev->available_nodes & (1 << DRM_NODE_RENDER)) != 0
                                 ? DRM_NODE_RENDER
                                 : DRM_NODE_PRIMARY;
            if ((dev->available_nodes & (1 << node)) != 0) {
                c.drm_fd = open(dev->nodes[node], O_RDWR | O_CLOEXEC);
            }
            drmFreeDevice(&dev);
        }
    }
    uint64_t timeline = 0;
    if (c.drm_fd >= 0 &&
        (drmGetCap(c.drm_fd, DRM_CAP_SYNCOBJ_TIMELINE, &timeline) != 0 || timeline == 0 ||
         drmSyncobjCreate(c.drm_fd, 0, &c.scratch) != 0)) {
        close(c.drm_fd);
        c.drm_fd = -1;
    }
    std::printf("stud-render-host: the compositor composites on %u:%u and imports %zu "
                "format/layout pairs%s\n",
                major(c.main_device), minor(c.main_device), c.importable.size(),
                c.drm_fd >= 0 ? "" : "; no timeline syncobjs on its GPU, the driver presents");
    std::fflush(stdout);
}

bool usable(const Compositor& c) {
    return c.dmabuf != nullptr && c.syncobj != nullptr && c.have_main_device &&
           c.drm_fd >= 0 && !c.importable.empty();
}

// --- formats and memory -----------------------------------------------

// The DRM format the compositor gets for an engine format, and the
// Vulkan format of the buffer it is copied into: the same bits, UNORM,
// since the copy is bitwise and the compositor reads them as UNORM. The
// X variants: the window is opaque, and alpha in it would show the
// desktop through.
bool drm_format_for(VkFormat format, uint32_t* fourcc, VkFormat* buffer_format) {
    switch (format) {
        case VK_FORMAT_B8G8R8A8_UNORM:
        case VK_FORMAT_B8G8R8A8_SRGB:
            *fourcc = DRM_FORMAT_XRGB8888;
            *buffer_format = VK_FORMAT_B8G8R8A8_UNORM;
            return true;
        case VK_FORMAT_R8G8B8A8_UNORM:
        case VK_FORMAT_R8G8B8A8_SRGB:
            *fourcc = DRM_FORMAT_XBGR8888;
            *buffer_format = VK_FORMAT_R8G8B8A8_UNORM;
            return true;
        case VK_FORMAT_A2R10G10B10_UNORM_PACK32:
            *fourcc = DRM_FORMAT_XRGB2101010;
            *buffer_format = format;
            return true;
        case VK_FORMAT_A2B10G10R10_UNORM_PACK32:
            *fourcc = DRM_FORMAT_XBGR2101010;
            *buffer_format = format;
            return true;
        default:
            return false;
    }
}

// The layouts this GPU can export a copy target in that the compositor
// also imports, with how many memory planes each has.
std::map<uint64_t, uint32_t> shared_modifiers(const Device& d, const Compositor& c,
                                              VkFormat format, uint32_t fourcc, VkExtent2D extent) {
    std::map<uint64_t, uint32_t> out;
    VkDrmFormatModifierPropertiesListEXT list{};
    list.sType = VK_STRUCTURE_TYPE_DRM_FORMAT_MODIFIER_PROPERTIES_LIST_EXT;
    VkFormatProperties2 props{};
    props.sType = VK_STRUCTURE_TYPE_FORMAT_PROPERTIES_2;
    props.pNext = &list;
    vkGetPhysicalDeviceFormatProperties2(d.physical_device, format, &props);
    std::vector<VkDrmFormatModifierPropertiesEXT> mods(list.drmFormatModifierCount);
    list.pDrmFormatModifierProperties = mods.data();
    vkGetPhysicalDeviceFormatProperties2(d.physical_device, format, &props);
    for (const auto& m : mods) {
        if ((m.drmFormatModifierTilingFeatures & VK_FORMAT_FEATURE_TRANSFER_DST_BIT) == 0) continue;
        if (c.importable.count({fourcc, m.drmFormatModifier}) == 0) continue;
        VkPhysicalDeviceImageDrmFormatModifierInfoEXT mi{};
        mi.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_IMAGE_DRM_FORMAT_MODIFIER_INFO_EXT;
        mi.drmFormatModifier = m.drmFormatModifier;
        mi.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
        VkPhysicalDeviceExternalImageFormatInfo ei{};
        ei.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTERNAL_IMAGE_FORMAT_INFO;
        ei.pNext = &mi;
        ei.handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT;
        VkPhysicalDeviceImageFormatInfo2 ii{};
        ii.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_IMAGE_FORMAT_INFO_2;
        ii.pNext = &ei;
        ii.format = format;
        ii.type = VK_IMAGE_TYPE_2D;
        ii.tiling = VK_IMAGE_TILING_DRM_FORMAT_MODIFIER_EXT;
        ii.usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT;
        VkExternalImageFormatProperties ep{};
        ep.sType = VK_STRUCTURE_TYPE_EXTERNAL_IMAGE_FORMAT_PROPERTIES;
        VkImageFormatProperties2 ip{};
        ip.sType = VK_STRUCTURE_TYPE_IMAGE_FORMAT_PROPERTIES_2;
        ip.pNext = &ep;
        if (vkGetPhysicalDeviceImageFormatProperties2(d.physical_device, &ii, &ip) != VK_SUCCESS) {
            continue;
        }
        if ((ep.externalMemoryProperties.externalMemoryFeatures &
             VK_EXTERNAL_MEMORY_FEATURE_EXPORTABLE_BIT) == 0) {
            continue;
        }
        if (ip.imageFormatProperties.maxExtent.width < extent.width ||
            ip.imageFormatProperties.maxExtent.height < extent.height) {
            continue;
        }
        out[m.drmFormatModifier] = m.drmFormatModifierPlaneCount;
    }
    return out;
}

// Device memory for the engine's images. For the compositor's buffers:
// the GPU's own memory when the compositor is on the same GPU, and system
// memory otherwise -- measured on an RTX 3050 + Iris Xe, the Intel GPU
// imports a buffer in NVIDIA's memory without complaint and then cannot
// read a byte of it.
int memory_type(const Device& d, uint32_t bits, bool device_local) {
    VkPhysicalDeviceMemoryProperties mp{};
    vkGetPhysicalDeviceMemoryProperties(d.physical_device, &mp);
    for (uint32_t i = 0; i < mp.memoryTypeCount; ++i) {
        if ((bits & (1u << i)) == 0) continue;
        const bool local =
            (mp.memoryTypes[i].propertyFlags & VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT) != 0;
        if (local == device_local) return static_cast<int>(i);
    }
    return -1;
}

bool bind_new_memory(const Device& d, VkImage image, bool device_local, bool exportable,
                     VkDeviceMemory* memory) {
    VkMemoryRequirements req{};
    d.vk.vkGetImageMemoryRequirements(d.device, image, &req);
    int type = memory_type(d, req.memoryTypeBits, device_local);
    if (type < 0 && device_local) type = memory_type(d, req.memoryTypeBits, false);
    if (type < 0) return false;
    VkExportMemoryAllocateInfo ex{};
    ex.sType = VK_STRUCTURE_TYPE_EXPORT_MEMORY_ALLOCATE_INFO;
    ex.handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT;
    VkMemoryDedicatedAllocateInfo dedicated{};
    dedicated.sType = VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO;
    dedicated.pNext = exportable ? &ex : nullptr;
    dedicated.image = image;
    VkMemoryAllocateInfo ai{};
    ai.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    ai.pNext = &dedicated;
    ai.allocationSize = req.size;
    ai.memoryTypeIndex = static_cast<uint32_t>(type);
    if (d.vk.vkAllocateMemory(d.device, &ai, nullptr, memory) != VK_SUCCESS) return false;
    return d.vk.vkBindImageMemory(d.device, image, *memory, 0) == VK_SUCCESS;
}

// --- buffers --------------------------------------------------------------

struct Created {
    wl_buffer* buffer = nullptr;
    bool answered = false;
};
void params_created(void* data, zwp_linux_buffer_params_v1*, wl_buffer* buffer) {
    auto* c = static_cast<Created*>(data);
    c->buffer = buffer;
    c->answered = true;
}
void params_failed(void* data, zwp_linux_buffer_params_v1*) {
    static_cast<Created*>(data)->answered = true;
}
const zwp_linux_buffer_params_v1_listener kParams = {params_created, params_failed};

bool make_buffer(const Device& d, Swapchain& sc, VkFormat format,
                 const std::map<uint64_t, uint32_t>& modifiers, Buffer& b) {
    std::vector<uint64_t> list;
    for (const auto& m : modifiers) list.push_back(m.first);
    VkImageDrmFormatModifierListCreateInfoEXT ml{};
    ml.sType = VK_STRUCTURE_TYPE_IMAGE_DRM_FORMAT_MODIFIER_LIST_CREATE_INFO_EXT;
    ml.drmFormatModifierCount = static_cast<uint32_t>(list.size());
    ml.pDrmFormatModifiers = list.data();
    VkExternalMemoryImageCreateInfo em{};
    em.sType = VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_IMAGE_CREATE_INFO;
    em.pNext = &ml;
    em.handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT;
    VkImageCreateInfo ii{};
    ii.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    ii.pNext = &em;
    ii.imageType = VK_IMAGE_TYPE_2D;
    ii.format = format;
    ii.extent = {sc.extent.width, sc.extent.height, 1};
    ii.mipLevels = 1;
    ii.arrayLayers = 1;
    ii.samples = VK_SAMPLE_COUNT_1_BIT;
    ii.tiling = VK_IMAGE_TILING_DRM_FORMAT_MODIFIER_EXT;
    ii.usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    ii.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    ii.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    if (d.vk.vkCreateImage(d.device, &ii, nullptr, &b.image) != VK_SUCCESS) return false;
    if (!bind_new_memory(d, b.image, sc.same_gpu, true, &b.memory)) {
        return false;
    }

    VkImageDrmFormatModifierPropertiesEXT chosen{};
    chosen.sType = VK_STRUCTURE_TYPE_IMAGE_DRM_FORMAT_MODIFIER_PROPERTIES_EXT;
    if (d.vk.vkGetImageDrmFormatModifierPropertiesEXT(d.device, b.image, &chosen) != VK_SUCCESS) {
        return false;
    }
    const auto planes = modifiers.find(chosen.drmFormatModifier);
    if (planes == modifiers.end()) return false;

    VkMemoryGetFdInfoKHR gi{};
    gi.sType = VK_STRUCTURE_TYPE_MEMORY_GET_FD_INFO_KHR;
    gi.memory = b.memory;
    gi.handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT;
    int fd = -1;
    if (d.vk.vkGetMemoryFdKHR(d.device, &gi, &fd) != VK_SUCCESS) return false;

    Compositor& c = *sc.target.compositor;
    zwp_linux_buffer_params_v1* params = zwp_linux_dmabuf_v1_create_params(c.dmabuf);
    static const VkImageAspectFlagBits kPlane[] = {
        VK_IMAGE_ASPECT_MEMORY_PLANE_0_BIT_EXT, VK_IMAGE_ASPECT_MEMORY_PLANE_1_BIT_EXT,
        VK_IMAGE_ASPECT_MEMORY_PLANE_2_BIT_EXT, VK_IMAGE_ASPECT_MEMORY_PLANE_3_BIT_EXT};
    for (uint32_t p = 0; p < planes->second && p < 4; ++p) {
        VkImageSubresource sub{};
        sub.aspectMask = kPlane[p];
        VkSubresourceLayout layout{};
        d.vk.vkGetImageSubresourceLayout(d.device, b.image, &sub, &layout);
        // libwayland duplicates the fd when it marshals, so one serves
        // every plane and is closed below.
        zwp_linux_buffer_params_v1_add(params, fd, p, static_cast<uint32_t>(layout.offset),
                                       static_cast<uint32_t>(layout.rowPitch),
                                       static_cast<uint32_t>(chosen.drmFormatModifier >> 32),
                                       static_cast<uint32_t>(chosen.drmFormatModifier));
    }
    // create, not create_immed: a buffer the compositor rejects is then a
    // `failed` event, not a protocol error that would take the whole
    // window's connection down with it.
    Created created;
    zwp_linux_buffer_params_v1_add_listener(params, &kParams, &created);
    zwp_linux_buffer_params_v1_create(params, static_cast<int32_t>(sc.extent.width),
                                      static_cast<int32_t>(sc.extent.height), sc.fourcc, 0);
    while (!created.answered && wl_display_roundtrip_queue(c.display, c.queue) >= 0) {
    }
    zwp_linux_buffer_params_v1_destroy(params);
    close(fd);
    if (created.buffer == nullptr) {
        std::printf("stud-render-host: the compositor refused a %ux%u buffer in layout "
                    "0x%llx\n", sc.extent.width, sc.extent.height,
                    static_cast<unsigned long long>(chosen.drmFormatModifier));
        std::fflush(stdout);
        return false;
    }
    b.wl = created.buffer;

    int timeline_fd = -1;
    if (drmSyncobjCreate(c.drm_fd, 0, &b.syncobj) != 0 ||
        drmSyncobjHandleToFD(c.drm_fd, b.syncobj, &timeline_fd) != 0) {
        return false;
    }
    b.timeline = wp_linux_drm_syncobj_manager_v1_import_timeline(c.syncobj, timeline_fd);
    close(timeline_fd);
    return b.timeline != nullptr;
}

// Whether the compositor is done with a buffer: never given to it, or its
// release point has signalled. Polled, never waited for.
bool buffer_free(const Compositor& c, Buffer& b) {
    if (b.point == 0) return true;
    uint32_t handle = b.syncobj;
    uint64_t point = b.point;
    return drmSyncobjTimelineWait(c.drm_fd, &handle, &point, 1, 0, 0, nullptr) == 0;
}

void destroy_swapchain_objects(Swapchain* sc) {
    const Device& d = *sc->dev;
    for (Image& im : sc->images) {
        if (im.fence != VK_NULL_HANDLE) {
            // Stud's own copy, GPU work only: nothing here waits on the
            // compositor.
            d.vk.vkWaitForFences(d.device, 1, &im.fence, VK_TRUE, UINT64_MAX);
            d.vk.vkDestroyFence(d.device, im.fence, nullptr);
        }
        if (im.copied != VK_NULL_HANDLE) d.vk.vkDestroySemaphore(d.device, im.copied, nullptr);
        if (im.image != VK_NULL_HANDLE) d.vk.vkDestroyImage(d.device, im.image, nullptr);
        if (im.memory != VK_NULL_HANDLE) d.vk.vkFreeMemory(d.device, im.memory, nullptr);
        if (im.done_fd >= 0) close(im.done_fd);
    }
    if (sc->pool != VK_NULL_HANDLE) d.vk.vkDestroyCommandPool(d.device, sc->pool, nullptr);
    // The compositor holds its own references to whatever it still shows:
    // destroying these only ends Stud's.
    for (Buffer& b : sc->buffers) {
        if (b.wl != nullptr) wl_buffer_destroy(b.wl);
        if (b.timeline != nullptr) wp_linux_drm_syncobj_timeline_v1_destroy(b.timeline);
        if (b.syncobj != 0) drmSyncobjDestroy(sc->target.compositor->drm_fd, b.syncobj);
        if (b.image != VK_NULL_HANDLE) d.vk.vkDestroyImage(d.device, b.image, nullptr);
        if (b.memory != VK_NULL_HANDLE) d.vk.vkFreeMemory(d.device, b.memory, nullptr);
    }
    if (sc->target.compositor != nullptr) wl_display_flush(sc->target.compositor->display);
    delete sc;
}

// Whether a DRM device is the GPU Vulkan renders on. Unknown counts as
// another GPU: system memory works on both, device memory only on one.
bool on_gpu(VkPhysicalDevice physical_device, dev_t device) {
    uint32_t n = 0;
    vkEnumerateDeviceExtensionProperties(physical_device, nullptr, &n, nullptr);
    std::vector<VkExtensionProperties> props(n);
    vkEnumerateDeviceExtensionProperties(physical_device, nullptr, &n, props.data());
    bool known = false;
    for (const auto& p : props) {
        if (std::strcmp(p.extensionName, VK_EXT_PHYSICAL_DEVICE_DRM_EXTENSION_NAME) == 0) known = true;
    }
    if (!known) return false;
    VkPhysicalDeviceDrmPropertiesEXT drm{};
    drm.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DRM_PROPERTIES_EXT;
    VkPhysicalDeviceProperties2 props2{};
    props2.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2;
    props2.pNext = &drm;
    vkGetPhysicalDeviceProperties2(physical_device, &props2);
    return (drm.hasPrimary != 0 &&
            device == makedev(static_cast<unsigned>(drm.primaryMajor),
                              static_cast<unsigned>(drm.primaryMinor))) ||
           (drm.hasRender != 0 &&
            device == makedev(static_cast<unsigned>(drm.renderMajor),
                              static_cast<unsigned>(drm.renderMinor)));
}

// --- the swapchain entry points ---------------------------------------

Swapchain* ours(VkSwapchainKHR handle) {
    auto* sc = reinterpret_cast<Swapchain*>(handle);
    return state().swapchains.count(sc) != 0 ? sc : nullptr;
}

VKAPI_ATTR VkResult VKAPI_CALL create_swapchain(VkDevice device,
                                                const VkSwapchainCreateInfoKHR* ci,
                                                const VkAllocationCallbacks* alloc,
                                                VkSwapchainKHR* out) {
    State& s = state();
    std::unique_lock<std::mutex> lock(s.mutex);
    Device& d = *device_of(s, device);
    const auto target = s.targets.find(ci->surface);
    uint32_t fourcc = 0;
    VkFormat buffer_format = VK_FORMAT_UNDEFINED;
    std::map<uint64_t, uint32_t> modifiers;
    bool same_gpu = false;
    if (target != s.targets.end() && usable(*target->second.compositor) &&
        drm_format_for(ci->imageFormat, &fourcc, &buffer_format)) {
        const Compositor& c = *target->second.compositor;
        same_gpu = on_gpu(d.physical_device, c.main_device);
        modifiers = shared_modifiers(d, c, buffer_format, fourcc, ci->imageExtent);
    }
    if (modifiers.empty()) {
        static int said = 0;
        if (said++ < 4) {
            std::printf("stud-render-host: no layout both this GPU and the compositor share for "
                        "format %d; the driver presents this swapchain\n",
                        static_cast<int>(ci->imageFormat));
            std::fflush(stdout);
        }
        lock.unlock();
        return d.vk.vkCreateSwapchainKHR(device, ci, alloc, out);
    }

    auto* sc = new Swapchain();
    sc->dev = &d;
    sc->same_gpu = same_gpu;
    sc->target = target->second;
    sc->extent = ci->imageExtent;
    sc->format = ci->imageFormat;
    sc->fourcc = fourcc;
    const uint32_t count = ci->minImageCount > 0 ? ci->minImageCount : 1;
    bool ok = true;

    VkCommandPoolCreateInfo pci{};
    pci.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
    pci.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    pci.queueFamilyIndex = d.queue_family;
    ok = d.vk.vkCreateCommandPool(d.device, &pci, nullptr, &sc->pool) == VK_SUCCESS;

    sc->images.resize(count);
    for (Image& im : sc->images) {
        if (!ok) break;
        VkImageCreateInfo ii{};
        ii.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
        if ((ci->flags & VK_SWAPCHAIN_CREATE_MUTABLE_FORMAT_BIT_KHR) != 0) {
            ii.flags |= VK_IMAGE_CREATE_MUTABLE_FORMAT_BIT;
        }
        ii.imageType = VK_IMAGE_TYPE_2D;
        ii.format = ci->imageFormat;
        ii.extent = {ci->imageExtent.width, ci->imageExtent.height, 1};
        ii.mipLevels = 1;
        ii.arrayLayers = ci->imageArrayLayers;
        ii.samples = VK_SAMPLE_COUNT_1_BIT;
        ii.tiling = VK_IMAGE_TILING_OPTIMAL;
        ii.usage = ci->imageUsage | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
        ii.sharingMode = ci->imageSharingMode;
        ii.queueFamilyIndexCount = ci->queueFamilyIndexCount;
        ii.pQueueFamilyIndices = ci->pQueueFamilyIndices;
        ii.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        ok = d.vk.vkCreateImage(d.device, &ii, nullptr, &im.image) == VK_SUCCESS &&
             bind_new_memory(d, im.image, true, false, &im.memory);

        VkCommandBufferAllocateInfo cbai{};
        cbai.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
        cbai.commandPool = sc->pool;
        cbai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        cbai.commandBufferCount = 1;
        ok = ok && d.vk.vkAllocateCommandBuffers(d.device, &cbai, &im.cmd) == VK_SUCCESS;
        VkFenceCreateInfo fci{};
        fci.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
        fci.flags = VK_FENCE_CREATE_SIGNALED_BIT;
        ok = ok && d.vk.vkCreateFence(d.device, &fci, nullptr, &im.fence) == VK_SUCCESS;
        VkExportSemaphoreCreateInfo esci{};
        esci.sType = VK_STRUCTURE_TYPE_EXPORT_SEMAPHORE_CREATE_INFO;
        esci.handleTypes = VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_SYNC_FD_BIT;
        VkSemaphoreCreateInfo sci{};
        sci.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;
        sci.pNext = &esci;
        ok = ok && d.vk.vkCreateSemaphore(d.device, &sci, nullptr, &im.copied) == VK_SUCCESS;
    }
    // As many buffers as the engine has images: the count the surface's
    // own minimum and the engine's request settled on, not one invented
    // here.
    sc->buffers.resize(count);
    for (Buffer& b : sc->buffers) {
        if (!ok) break;
        ok = make_buffer(d, *sc, buffer_format, modifiers, b);
    }
    if (ok && s.syncobj_surfaces.count(sc->target.surface) == 0) {
        s.syncobj_surfaces[sc->target.surface] = wp_linux_drm_syncobj_manager_v1_get_surface(
            sc->target.compositor->syncobj, sc->target.surface);
    }
    if (!ok) {
        std::printf("stud-render-host: could not build Stud's own swapchain; the driver "
                    "presents this one\n");
        std::fflush(stdout);
        destroy_swapchain_objects(sc);
        lock.unlock();
        return d.vk.vkCreateSwapchainKHR(device, ci, alloc, out);
    }

    s.swapchains.insert(sc);
    *out = reinterpret_cast<VkSwapchainKHR>(sc);
    static bool said = false;
    if (!said) {
        said = true;
        std::printf("stud-render-host: Stud presents its own frames: %u images, %s buffers on "
                    "the compositor's GPU, explicit sync\n",
                    count, sc->same_gpu ? "device-memory" : "system-memory (another GPU)");
        std::fflush(stdout);
    }
    return VK_SUCCESS;
}

VKAPI_ATTR void VKAPI_CALL destroy_swapchain(VkDevice device, VkSwapchainKHR handle,
                                             const VkAllocationCallbacks* alloc) {
    State& s = state();
    std::unique_lock<std::mutex> lock(s.mutex);
    Device& d = *device_of(s, device);
    Swapchain* sc = ours(handle);
    if (sc == nullptr) {
        lock.unlock();
        if (handle != VK_NULL_HANDLE) d.vk.vkDestroySwapchainKHR(device, handle, alloc);
        return;
    }
    s.swapchains.erase(sc);
    destroy_swapchain_objects(sc);
}

VKAPI_ATTR VkResult VKAPI_CALL get_swapchain_images(VkDevice device, VkSwapchainKHR handle,
                                                    uint32_t* count, VkImage* images) {
    State& s = state();
    std::unique_lock<std::mutex> lock(s.mutex);
    Device& d = *device_of(s, device);
    Swapchain* sc = ours(handle);
    if (sc == nullptr) {
        lock.unlock();
        return d.vk.vkGetSwapchainImagesKHR(device, handle, count, images);
    }
    const auto have = static_cast<uint32_t>(sc->images.size());
    if (images == nullptr) {
        *count = have;
        return VK_SUCCESS;
    }
    const uint32_t n = *count < have ? *count : have;
    for (uint32_t i = 0; i < n; ++i) images[i] = sc->images[i].image;
    *count = n;
    return n < have ? VK_INCOMPLETE : VK_SUCCESS;
}

// Hands the engine an image it is not holding. What it waits on is only
// Stud's own copy out of that image, imported into its semaphore and fence
// as a sync_file, so acquiring never depends on the compositor at all.
VKAPI_ATTR VkResult VKAPI_CALL acquire_next_image(VkDevice device, VkSwapchainKHR handle,
                                                  uint64_t timeout, VkSemaphore semaphore,
                                                  VkFence fence, uint32_t* index) {
    State& s = state();
    std::unique_lock<std::mutex> lock(s.mutex);
    Device& d = *device_of(s, device);
    Swapchain* sc = ours(handle);
    if (sc == nullptr) {
        lock.unlock();
        return d.vk.vkAcquireNextImageKHR(device, handle, timeout, semaphore, fence, index);
    }
    const auto n = static_cast<uint32_t>(sc->images.size());
    for (uint32_t step = 0; step < n; ++step) {
        const uint32_t i = (sc->next_image + step) % n;
        Image& im = sc->images[i];
        if (im.acquired) continue;
        // -1 is a sync_file that has already signalled. A successful
        // import takes the fd; a failed one leaves it to be closed here.
        const int fd = im.done_fd;
        im.done_fd = -1;
        int sem_fd = -1;
        int fence_fd = -1;
        if (semaphore != VK_NULL_HANDLE) sem_fd = fd;
        if (fence != VK_NULL_HANDLE) {
            fence_fd = semaphore != VK_NULL_HANDLE && fd >= 0 ? dup(fd) : fd;
        }
        if (semaphore == VK_NULL_HANDLE && fence == VK_NULL_HANDLE && fd >= 0) close(fd);
        VkResult r = VK_SUCCESS;
        if (semaphore != VK_NULL_HANDLE) {
            VkImportSemaphoreFdInfoKHR info{};
            info.sType = VK_STRUCTURE_TYPE_IMPORT_SEMAPHORE_FD_INFO_KHR;
            info.semaphore = semaphore;
            info.flags = VK_SEMAPHORE_IMPORT_TEMPORARY_BIT;
            info.handleType = VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_SYNC_FD_BIT;
            info.fd = sem_fd;
            r = d.vk.vkImportSemaphoreFdKHR(d.device, &info);
            if (r != VK_SUCCESS && sem_fd >= 0) close(sem_fd);
        }
        if (fence != VK_NULL_HANDLE) {
            VkImportFenceFdInfoKHR info{};
            info.sType = VK_STRUCTURE_TYPE_IMPORT_FENCE_FD_INFO_KHR;
            info.fence = fence;
            info.flags = VK_FENCE_IMPORT_TEMPORARY_BIT;
            info.handleType = VK_EXTERNAL_FENCE_HANDLE_TYPE_SYNC_FD_BIT;
            info.fd = fence_fd;
            const VkResult fr = r == VK_SUCCESS ? d.vk.vkImportFenceFdKHR(d.device, &info)
                                                : VK_ERROR_UNKNOWN;
            if (fr != VK_SUCCESS && fence_fd >= 0) close(fence_fd);
            if (r == VK_SUCCESS) r = fr;
        }
        if (r != VK_SUCCESS) return VK_ERROR_SURFACE_LOST_KHR;
        im.acquired = true;
        sc->next_image = (i + 1) % n;
        *index = i;
        return VK_SUCCESS;
    }
    // The engine holds every image. Its loop acquires one at a time, so
    // this is a misuse no waiting would end.
    return timeout == 0 ? VK_NOT_READY : VK_TIMEOUT;
}

void barrier(const VolkDeviceTable& vk, VkCommandBuffer cmd, VkImage image, VkImageLayout from, VkImageLayout to,
             VkAccessFlags src_access, VkAccessFlags dst_access, uint32_t src_family,
             uint32_t dst_family) {
    VkImageMemoryBarrier b{};
    b.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    b.srcAccessMask = src_access;
    b.dstAccessMask = dst_access;
    b.oldLayout = from;
    b.newLayout = to;
    b.srcQueueFamilyIndex = src_family;
    b.dstQueueFamilyIndex = dst_family;
    b.image = image;
    b.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, VK_REMAINING_ARRAY_LAYERS};
    vk.vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
                            VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 0, nullptr, 0, nullptr, 1, &b);
}

// One swapchain's share of a present. Runs under render-host's queue lock,
// which the caller holds for the whole present, so the submit and the
// commit are serialised with every other user of the queue and the
// content surface.
VkResult present_one(State& s, VkQueue queue, Swapchain* sc, uint32_t index,
                     const VkPresentInfoKHR* info) {
    const Device& d = *sc->dev;
    if (index >= sc->images.size()) return VK_ERROR_OUT_OF_DATE_KHR;
    Image& im = sc->images[index];
    Compositor& c = *sc->target.compositor;
    im.acquired = false;

    Buffer* buffer = nullptr;
    const auto nb = static_cast<uint32_t>(sc->buffers.size());
    for (uint32_t step = 0; step < nb && buffer == nullptr; ++step) {
        const uint32_t b = (sc->next_buffer + step) % nb;
        if (buffer_free(c, sc->buffers[b])) {
            buffer = &sc->buffers[b];
            sc->next_buffer = (b + 1) % nb;
        }
    }

    // Stud's own copy out of this image, GPU work only, long finished in
    // practice: the engine waited on it before drawing into the image.
    d.vk.vkWaitForFences(d.device, 1, &im.fence, VK_TRUE, UINT64_MAX);
    d.vk.vkResetFences(d.device, 1, &im.fence);

    VkCommandBuffer cmd = VK_NULL_HANDLE;
    if (buffer != nullptr) {
        cmd = im.cmd;
        d.vk.vkResetCommandBuffer(cmd, 0);
        VkCommandBufferBeginInfo bi{};
        bi.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
        bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        d.vk.vkBeginCommandBuffer(cmd, &bi);
        barrier(d.vk, cmd, im.image, VK_IMAGE_LAYOUT_PRESENT_SRC_KHR,
                VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_ACCESS_MEMORY_WRITE_BIT,
                VK_ACCESS_TRANSFER_READ_BIT, VK_QUEUE_FAMILY_IGNORED, VK_QUEUE_FAMILY_IGNORED);
        // Its old contents are never read: no ownership to take back from
        // the compositor, whose release point already said it is done.
        barrier(d.vk, cmd, buffer->image, VK_IMAGE_LAYOUT_UNDEFINED,
                VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 0, VK_ACCESS_TRANSFER_WRITE_BIT,
                VK_QUEUE_FAMILY_IGNORED, VK_QUEUE_FAMILY_IGNORED);
        VkImageCopy region{};
        region.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
        region.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
        region.extent = {sc->extent.width, sc->extent.height, 1};
        d.vk.vkCmdCopyImage(cmd, im.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, buffer->image,
                             VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);
        barrier(d.vk, cmd, im.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                VK_IMAGE_LAYOUT_PRESENT_SRC_KHR, 0, 0, VK_QUEUE_FAMILY_IGNORED,
                VK_QUEUE_FAMILY_IGNORED);
        // Handed to the compositor's GPU.
        barrier(d.vk, cmd, buffer->image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                VK_IMAGE_LAYOUT_GENERAL, VK_ACCESS_TRANSFER_WRITE_BIT, 0, d.queue_family,
                d.foreign_family);
        d.vk.vkEndCommandBuffer(cmd);
    }

    // With no free buffer the frame is dropped: the submit still consumes
    // what the engine signalled and still marks the image's copy done, so
    // the next acquire of it has something to wait on.
    std::vector<VkPipelineStageFlags> stages(info->waitSemaphoreCount,
                                             VK_PIPELINE_STAGE_ALL_COMMANDS_BIT);
    VkSubmitInfo si{};
    si.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    si.waitSemaphoreCount = info->waitSemaphoreCount;
    si.pWaitSemaphores = info->pWaitSemaphores;
    si.pWaitDstStageMask = stages.empty() ? nullptr : stages.data();
    si.commandBufferCount = cmd != VK_NULL_HANDLE ? 1 : 0;
    si.pCommandBuffers = cmd != VK_NULL_HANDLE ? &cmd : nullptr;
    si.signalSemaphoreCount = 1;
    si.pSignalSemaphores = &im.copied;
    VkResult r = d.vk.vkQueueSubmit(queue, 1, &si, im.fence);
    if (r != VK_SUCCESS) return r;

    VkSemaphoreGetFdInfoKHR gi{};
    gi.sType = VK_STRUCTURE_TYPE_SEMAPHORE_GET_FD_INFO_KHR;
    gi.semaphore = im.copied;
    gi.handleType = VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_SYNC_FD_BIT;
    int done = -1;
    r = d.vk.vkGetSemaphoreFdKHR(d.device, &gi, &done);
    if (r != VK_SUCCESS) return r;
    if (im.done_fd >= 0) close(im.done_fd);
    im.done_fd = done >= 0 ? dup(done) : -1;

    if (buffer == nullptr) {
        if (done >= 0) close(done);
        if (sc->dropped++ == 0) {
            std::printf("stud-render-host: the compositor still holds every buffer; dropping "
                        "the frame instead of waiting for one\n");
            std::fflush(stdout);
        }
        return VK_SUCCESS;
    }

    // The copy's completion, as the buffer's acquire point.
    const uint64_t acquire = buffer->point + 1;
    const uint64_t release = buffer->point + 2;
    int imported = -1;
    if (done >= 0) {
        imported = drmSyncobjImportSyncFile(c.drm_fd, c.scratch, done);
        if (imported == 0) {
            imported = drmSyncobjTransfer(c.drm_fd, buffer->syncobj, acquire, c.scratch, 0, 0);
        }
        close(done);
    } else {
        uint32_t handle = buffer->syncobj;
        uint64_t point = acquire;
        imported = drmSyncobjTimelineSignal(c.drm_fd, &handle, &point, 1);
    }
    if (imported != 0) return VK_ERROR_SURFACE_LOST_KHR;
    buffer->point = release;

    wp_linux_drm_syncobj_surface_v1* sync = s.syncobj_surfaces[sc->target.surface];
    wp_linux_drm_syncobj_surface_v1_set_acquire_point(sync, buffer->timeline,
                                                      static_cast<uint32_t>(acquire >> 32),
                                                      static_cast<uint32_t>(acquire));
    wp_linux_drm_syncobj_surface_v1_set_release_point(sync, buffer->timeline,
                                                      static_cast<uint32_t>(release >> 32),
                                                      static_cast<uint32_t>(release));
    wl_surface_attach(sc->target.surface, buffer->wl, 0, 0);
    if (wl_proxy_get_version(reinterpret_cast<wl_proxy*>(sc->target.surface)) >=
        WL_SURFACE_DAMAGE_BUFFER_SINCE_VERSION) {
        wl_surface_damage_buffer(sc->target.surface, 0, 0, INT32_MAX, INT32_MAX);
    } else {
        wl_surface_damage(sc->target.surface, 0, 0, INT32_MAX, INT32_MAX);
    }
    wl_surface_commit(sc->target.surface);
    wl_display_flush(c.display);
    // Nothing here listens for these objects' events; drained so they do
    // not pile up on the queue.
    wl_display_dispatch_queue_pending(c.display, c.queue);
    return VK_SUCCESS;
}

VKAPI_ATTR VkResult VKAPI_CALL queue_present(VkQueue queue, const VkPresentInfoKHR* info) {
    State& s = state();
    std::unique_lock<std::mutex> lock(s.mutex);
    Device& d = *device_of(s, queue);
    bool any_ours = false;
    for (uint32_t i = 0; i < info->swapchainCount; ++i) {
        if (ours(info->pSwapchains[i]) != nullptr) any_ours = true;
    }
    if (!any_ours) {
        lock.unlock();
        return d.vk.vkQueuePresentKHR(queue, info);
    }
    // The wait semaphores are consumed by the first swapchain's submit.
    VkPresentInfoKHR rest = *info;
    rest.waitSemaphoreCount = 0;
    VkResult overall = VK_SUCCESS;
    for (uint32_t i = 0; i < info->swapchainCount; ++i) {
        Swapchain* sc = ours(info->pSwapchains[i]);
        // A mixed present (one of Stud's, one of the driver's) is not
        // something Stud's single window ever issues.
        const VkResult r = sc != nullptr
                               ? present_one(s, queue, sc, info->pImageIndices[i],
                                             i == 0 ? info : &rest)
                               : VK_ERROR_SURFACE_LOST_KHR;
        if (info->pResults != nullptr) info->pResults[i] = r;
        if (r != VK_SUCCESS) overall = r;
    }
    return overall;
}

bool has_extension(VkPhysicalDevice physical_device, const char* name) {
    uint32_t n = 0;
    vkEnumerateDeviceExtensionProperties(physical_device, nullptr, &n, nullptr);
    std::vector<VkExtensionProperties> props(n);
    vkEnumerateDeviceExtensionProperties(physical_device, nullptr, &n, props.data());
    for (const auto& p : props) {
        if (std::strcmp(p.extensionName, name) == 0) return true;
    }
    return false;
}

const char* const kRequired[] = {
    VK_KHR_EXTERNAL_MEMORY_FD_EXTENSION_NAME,     VK_EXT_EXTERNAL_MEMORY_DMA_BUF_EXTENSION_NAME,
    VK_EXT_IMAGE_DRM_FORMAT_MODIFIER_EXTENSION_NAME, VK_KHR_EXTERNAL_SEMAPHORE_FD_EXTENSION_NAME,
    VK_KHR_EXTERNAL_FENCE_FD_EXTENSION_NAME,
};
// What those depend on before Vulkan 1.1/1.2 made them core; enabled where
// offered, which is harmless where they are core already.
const char* const kDependencies[] = {
    VK_KHR_EXTERNAL_MEMORY_EXTENSION_NAME,          VK_KHR_EXTERNAL_SEMAPHORE_EXTENSION_NAME,
    VK_KHR_EXTERNAL_FENCE_EXTENSION_NAME,           VK_KHR_IMAGE_FORMAT_LIST_EXTENSION_NAME,
    VK_KHR_BIND_MEMORY_2_EXTENSION_NAME,            VK_KHR_GET_MEMORY_REQUIREMENTS_2_EXTENSION_NAME,
    VK_KHR_SAMPLER_YCBCR_CONVERSION_EXTENSION_NAME, VK_KHR_DEDICATED_ALLOCATION_EXTENSION_NAME,
    VK_KHR_MAINTENANCE_1_EXTENSION_NAME,            VK_EXT_QUEUE_FAMILY_FOREIGN_EXTENSION_NAME,
};

}  // namespace

void note_wayland_surface(VkSurfaceKHR surface, wl_display* display, wl_surface* target) {
    if (surface == VK_NULL_HANDLE || display == nullptr || target == nullptr) return;
    State& s = state();
    std::lock_guard<std::mutex> lock(s.mutex);
    auto [it, fresh] = s.compositors.try_emplace(display);
    if (fresh) open_compositor(it->second, display);
    s.targets[surface] = {&it->second, target};
}

std::vector<std::string> device_extensions(VkPhysicalDevice physical_device) {
    for (const char* name : kRequired) {
        if (!has_extension(physical_device, name)) {
            std::printf("stud-render-host: the driver has no %s; it presents\n", name);
            std::fflush(stdout);
            return {};
        }
    }
    // Acquire hands the engine a sync_file; the driver has to take one.
    VkPhysicalDeviceExternalSemaphoreInfo si{};
    si.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTERNAL_SEMAPHORE_INFO;
    si.handleType = VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_SYNC_FD_BIT;
    VkExternalSemaphoreProperties sp{};
    sp.sType = VK_STRUCTURE_TYPE_EXTERNAL_SEMAPHORE_PROPERTIES;
    vkGetPhysicalDeviceExternalSemaphoreProperties(physical_device, &si, &sp);
    VkPhysicalDeviceExternalFenceInfo fi{};
    fi.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTERNAL_FENCE_INFO;
    fi.handleType = VK_EXTERNAL_FENCE_HANDLE_TYPE_SYNC_FD_BIT;
    VkExternalFenceProperties fp{};
    fp.sType = VK_STRUCTURE_TYPE_EXTERNAL_FENCE_PROPERTIES;
    vkGetPhysicalDeviceExternalFenceProperties(physical_device, &fi, &fp);
    const VkExternalSemaphoreFeatureFlags sem_need =
        VK_EXTERNAL_SEMAPHORE_FEATURE_EXPORTABLE_BIT | VK_EXTERNAL_SEMAPHORE_FEATURE_IMPORTABLE_BIT;
    if ((sp.externalSemaphoreFeatures & sem_need) != sem_need ||
        (fp.externalFenceFeatures & VK_EXTERNAL_FENCE_FEATURE_IMPORTABLE_BIT) == 0) {
        std::printf("stud-render-host: the driver cannot pass sync_files; it presents\n");
        std::fflush(stdout);
        return {};
    }
    std::vector<std::string> out(std::begin(kRequired), std::end(kRequired));
    for (const char* name : kDependencies) {
        if (has_extension(physical_device, name)) out.emplace_back(name);
    }
    return out;
}

bool install(const VolkDeviceTable& next, VkDevice device, VkPhysicalDevice physical_device,
             uint32_t queue_family) {
    State& s = state();
    std::lock_guard<std::mutex> lock(s.mutex);
    // A device made without VK_KHR_swapchain never presents: nothing to own.
    if (next.vkCreateSwapchainKHR == nullptr) return false;
    if (next.vkQueuePresentKHR == nullptr ||
        next.vkGetMemoryFdKHR == nullptr || next.vkGetSemaphoreFdKHR == nullptr ||
        next.vkImportSemaphoreFdKHR == nullptr || next.vkImportFenceFdKHR == nullptr ||
        next.vkGetImageDrmFormatModifierPropertiesEXT == nullptr) {
        std::printf("stud-render-host: the driver did not resolve the commands Stud's own "
                    "swapchain needs; it presents\n");
        std::fflush(stdout);
        return false;
    }
    Device& d = s.devices[dispatch_key(device)];
    d.vk = next;
    d.device = device;
    d.physical_device = physical_device;
    d.queue_family = queue_family;
    d.foreign_family = has_extension(physical_device, VK_EXT_QUEUE_FAMILY_FOREIGN_EXTENSION_NAME)
                           ? VK_QUEUE_FAMILY_FOREIGN_EXT
                           : VK_QUEUE_FAMILY_EXTERNAL;
    return true;
}

void forget_device(VkDevice device) {
    State& s = state();
    std::lock_guard<std::mutex> lock(s.mutex);
    // The engine destroys its swapchains first; any it left would be
    // pointing at a device that no longer exists either way.
    s.devices.erase(dispatch_key(device));
}

PFN_vkVoidFunction device_function(const char* name) {
    if (std::strcmp(name, "vkCreateSwapchainKHR") == 0) {
        return reinterpret_cast<PFN_vkVoidFunction>(create_swapchain);
    }
    if (std::strcmp(name, "vkDestroySwapchainKHR") == 0) {
        return reinterpret_cast<PFN_vkVoidFunction>(destroy_swapchain);
    }
    if (std::strcmp(name, "vkGetSwapchainImagesKHR") == 0) {
        return reinterpret_cast<PFN_vkVoidFunction>(get_swapchain_images);
    }
    if (std::strcmp(name, "vkAcquireNextImageKHR") == 0) {
        return reinterpret_cast<PFN_vkVoidFunction>(acquire_next_image);
    }
    if (std::strcmp(name, "vkQueuePresentKHR") == 0) {
        return reinterpret_cast<PFN_vkVoidFunction>(queue_present);
    }
    return nullptr;
}

}  // namespace stud::render_host::owned_swapchain
