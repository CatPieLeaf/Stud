#include "owned_swapchain.h"

#include <dlfcn.h>
#include <drm_fourcc.h>
#include <fcntl.h>
#include <linux/dma-buf.h>
#include <poll.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/sysmacros.h>
#include <unistd.h>
#include <wayland-client.h>
#include <xcb/dri3.h>
#include <xcb/present.h>
#include <xf86drm.h>

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <mutex>
#include <set>
#include <type_traits>
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

// What one X server offers, through DRI3 and Present on the connection the
// driver was handed for the surface.
struct XServer {
    xcb_connection_t* conn = nullptr;
    // The GPU the server composites on: the device DRI3 hands out.
    dev_t main_device = 0;
    bool usable = false;
};

// Where a surface's frames go: a Wayland surface, or an X11 window.
struct Target {
    Compositor* compositor = nullptr;
    wl_surface* surface = nullptr;
    XServer* x = nullptr;
    uint32_t window = 0;
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
    // X11: the pixmap over the same memory, and the dma-buf itself, which
    // carries the copy's fence to the server and the server's back.
    uint32_t pixmap = 0;
    int dmabuf = -1;
    bool held = false;  // presented, and no IdleNotify for it yet
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
// render-host can have several -- one per engine device -- so nothing
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
    // X11: the Present events this swapchain's pixmaps report on.
    uint32_t eid = 0;
    xcb_special_event_t* events = nullptr;
    uint32_t serial = 0;
    uint8_t depth = 0;
};

struct State {
    std::mutex mutex;
    std::map<wl_display*, Compositor> compositors;
    std::map<xcb_connection_t*, XServer> x_servers;
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

// --- the X server -------------------------------------------------------

// libxcb, its DRI3 and Present halves, and libX11-xcb, opened rather than
// linked: the layer is loaded on Wayland-only machines too, and a layer
// that fails to load takes the whole Vulkan instance down with it.
struct Xcb {
    bool loaded = false;
    decltype(&xcb_get_extension_data) get_extension_data = nullptr;
    decltype(&xcb_generate_id) generate_id = nullptr;
    decltype(&xcb_flush) flush = nullptr;
    decltype(&xcb_request_check) request_check = nullptr;
    decltype(&xcb_get_geometry) get_geometry = nullptr;
    decltype(&xcb_get_geometry_reply) get_geometry_reply = nullptr;
    decltype(&xcb_free_pixmap) free_pixmap = nullptr;
    decltype(&xcb_register_for_special_xge) register_special = nullptr;
    decltype(&xcb_unregister_for_special_event) unregister_special = nullptr;
    decltype(&xcb_poll_for_special_event) poll_special = nullptr;
    xcb_extension_t* dri3_id = nullptr;
    decltype(&xcb_dri3_query_version) dri3_query_version = nullptr;
    decltype(&xcb_dri3_query_version_reply) dri3_query_version_reply = nullptr;
    decltype(&xcb_dri3_open) dri3_open = nullptr;
    decltype(&xcb_dri3_open_reply) dri3_open_reply = nullptr;
    decltype(&xcb_dri3_open_reply_fds) dri3_open_reply_fds = nullptr;
    decltype(&xcb_dri3_get_supported_modifiers) get_modifiers = nullptr;
    decltype(&xcb_dri3_get_supported_modifiers_reply) get_modifiers_reply = nullptr;
    decltype(&xcb_dri3_get_supported_modifiers_window_modifiers) window_modifiers = nullptr;
    decltype(&xcb_dri3_get_supported_modifiers_window_modifiers_length)
        window_modifiers_length = nullptr;
    decltype(&xcb_dri3_get_supported_modifiers_screen_modifiers) screen_modifiers = nullptr;
    decltype(&xcb_dri3_get_supported_modifiers_screen_modifiers_length)
        screen_modifiers_length = nullptr;
    decltype(&xcb_dri3_pixmap_from_buffers_checked) pixmap_from_buffers = nullptr;
    xcb_extension_t* present_id = nullptr;
    decltype(&xcb_present_query_version) present_query_version = nullptr;
    decltype(&xcb_present_query_version_reply) present_query_version_reply = nullptr;
    decltype(&xcb_present_select_input_checked) select_input_checked = nullptr;
    decltype(&xcb_present_select_input) select_input = nullptr;
    decltype(&xcb_present_pixmap) present_pixmap = nullptr;
    xcb_connection_t* (*xlib_connection)(void* display) = nullptr;  // XGetXCBConnection
};

const Xcb& xcb() {
    static const Xcb x = [] {
        Xcb x;
        void* core = dlopen("libxcb.so.1", RTLD_NOW | RTLD_LOCAL);
        void* dri3 = dlopen("libxcb-dri3.so.0", RTLD_NOW | RTLD_LOCAL);
        void* present = dlopen("libxcb-present.so.0", RTLD_NOW | RTLD_LOCAL);
        void* xlib = dlopen("libX11-xcb.so.1", RTLD_NOW | RTLD_LOCAL);
        if (core == nullptr || dri3 == nullptr || present == nullptr) return x;
        bool all = true;
        auto load = [&all](void* lib, auto& field, const char* name) {
            field = reinterpret_cast<std::remove_reference_t<decltype(field)>>(dlsym(lib, name));
            if (field == nullptr) all = false;
        };
        load(core, x.get_extension_data, "xcb_get_extension_data");
        load(core, x.generate_id, "xcb_generate_id");
        load(core, x.flush, "xcb_flush");
        load(core, x.request_check, "xcb_request_check");
        load(core, x.get_geometry, "xcb_get_geometry");
        load(core, x.get_geometry_reply, "xcb_get_geometry_reply");
        load(core, x.free_pixmap, "xcb_free_pixmap");
        load(core, x.register_special, "xcb_register_for_special_xge");
        load(core, x.unregister_special, "xcb_unregister_for_special_event");
        load(core, x.poll_special, "xcb_poll_for_special_event");
        load(dri3, x.dri3_id, "xcb_dri3_id");
        load(dri3, x.dri3_query_version, "xcb_dri3_query_version");
        load(dri3, x.dri3_query_version_reply, "xcb_dri3_query_version_reply");
        load(dri3, x.dri3_open, "xcb_dri3_open");
        load(dri3, x.dri3_open_reply, "xcb_dri3_open_reply");
        load(dri3, x.dri3_open_reply_fds, "xcb_dri3_open_reply_fds");
        load(dri3, x.get_modifiers, "xcb_dri3_get_supported_modifiers");
        load(dri3, x.get_modifiers_reply, "xcb_dri3_get_supported_modifiers_reply");
        load(dri3, x.window_modifiers, "xcb_dri3_get_supported_modifiers_window_modifiers");
        load(dri3, x.window_modifiers_length,
             "xcb_dri3_get_supported_modifiers_window_modifiers_length");
        load(dri3, x.screen_modifiers, "xcb_dri3_get_supported_modifiers_screen_modifiers");
        load(dri3, x.screen_modifiers_length,
             "xcb_dri3_get_supported_modifiers_screen_modifiers_length");
        load(dri3, x.pixmap_from_buffers, "xcb_dri3_pixmap_from_buffers_checked");
        load(present, x.present_id, "xcb_present_id");
        load(present, x.present_query_version, "xcb_present_query_version");
        load(present, x.present_query_version_reply, "xcb_present_query_version_reply");
        load(present, x.select_input_checked, "xcb_present_select_input_checked");
        load(present, x.select_input, "xcb_present_select_input");
        load(present, x.present_pixmap, "xcb_present_pixmap");
        // Only an Xlib surface needs it; an XCB one names its connection.
        if (xlib != nullptr) {
            x.xlib_connection = reinterpret_cast<decltype(x.xlib_connection)>(
                dlsym(xlib, "XGetXCBConnection"));
        }
        x.loaded = all;
        return x;
    }();
    return x;
}

void open_x_server(XServer& x, xcb_connection_t* conn, uint32_t window) {
    x.conn = conn;
    const Xcb& f = xcb();
    if (!f.loaded) {
        std::printf("stud-render-host: libxcb's DRI3 and Present libraries are not installed; "
                    "the driver presents\n");
        std::fflush(stdout);
        return;
    }
    // A request for an extension the server lacks is not an error reply
    // but a dead connection, and this one is the driver's: asked first.
    for (xcb_extension_t* ext : {f.dri3_id, f.present_id}) {
        const xcb_query_extension_reply_t* have = f.get_extension_data(conn, ext);
        if (have == nullptr || have->present == 0) {
            std::printf("stud-render-host: the X server has no %s; the driver presents\n",
                        ext == f.dri3_id ? "DRI3" : "Present");
            std::fflush(stdout);
            return;
        }
    }
    // DRI3 1.2 is the first with layouts (modifiers) and multi-plane
    // buffers. Present 1.0 already has everything used here.
    auto* dri3 = f.dri3_query_version_reply(
        conn, f.dri3_query_version(conn, XCB_DRI3_MAJOR_VERSION, XCB_DRI3_MINOR_VERSION),
        nullptr);
    const bool dri3_ok =
        dri3 != nullptr && (dri3->major_version > 1 || dri3->minor_version >= 2);
    std::free(dri3);
    auto* present = f.present_query_version_reply(
        conn, f.present_query_version(conn, XCB_PRESENT_MAJOR_VERSION, XCB_PRESENT_MINOR_VERSION),
        nullptr);
    const bool present_ok = present != nullptr;
    std::free(present);
    if (!dri3_ok || !present_ok) {
        std::printf("stud-render-host: the X server's DRI3 is older than 1.2; the driver "
                    "presents\n");
        std::fflush(stdout);
        return;
    }

    auto* geometry = f.get_geometry_reply(conn, f.get_geometry(conn, window), nullptr);
    if (geometry == nullptr) return;
    const xcb_window_t root = geometry->root;
    std::free(geometry);
    auto* opened = f.dri3_open_reply(conn, f.dri3_open(conn, root, 0), nullptr);
    if (opened == nullptr || opened->nfd < 1) {
        std::free(opened);
        std::printf("stud-render-host: the X server would not name its GPU; the driver "
                    "presents\n");
        std::fflush(stdout);
        return;
    }
    const int fd = f.dri3_open_reply_fds(conn, opened)[0];
    std::free(opened);
    struct stat st {};
    const bool named = fstat(fd, &st) == 0;
    close(fd);
    if (!named) return;
    x.main_device = st.st_rdev;
    x.usable = true;
    std::printf("stud-render-host: the X server composites on %u:%u\n", major(x.main_device),
                minor(x.main_device));
    std::fflush(stdout);
}

// The X11 window depth a swapchain format is shown at, and the DRM format
// and Vulkan format of the buffer it is copied into. DRI3 has no format of
// its own: a depth-24 pixmap is XRGB8888 and a depth-30 one XRGB2101010,
// so an engine format in any other channel order has no bitwise match.
bool x_format_for(VkFormat format, uint8_t* depth, uint32_t* fourcc, VkFormat* buffer_format) {
    switch (format) {
        case VK_FORMAT_B8G8R8A8_UNORM:
        case VK_FORMAT_B8G8R8A8_SRGB:
            *depth = 24;
            *fourcc = DRM_FORMAT_XRGB8888;
            *buffer_format = VK_FORMAT_B8G8R8A8_UNORM;
            return true;
        case VK_FORMAT_A2R10G10B10_UNORM_PACK32:
            *depth = 30;
            *fourcc = DRM_FORMAT_XRGB2101010;
            *buffer_format = format;
            return true;
        default:
            return false;
    }
}

// What the server imports for this window at this depth. The window's own
// list is what it can show without a copy of its own; the screen's, what
// it can show at all.
std::set<std::pair<uint32_t, uint64_t>> x_importable(const XServer& x, uint32_t window,
                                                     uint8_t depth, uint32_t fourcc) {
    std::set<std::pair<uint32_t, uint64_t>> out;
    const Xcb& f = xcb();
    auto* geometry = f.get_geometry_reply(x.conn, f.get_geometry(x.conn, window), nullptr);
    const bool same_depth = geometry != nullptr && geometry->depth == depth;
    std::free(geometry);
    if (!same_depth) return out;
    auto* mods = f.get_modifiers_reply(x.conn, f.get_modifiers(x.conn, window, depth, 32), nullptr);
    if (mods == nullptr) return out;
    const uint64_t* list = f.window_modifiers(mods);
    int n = f.window_modifiers_length(mods);
    if (n == 0) {
        list = f.screen_modifiers(mods);
        n = f.screen_modifiers_length(mods);
    }
    for (int i = 0; i < n; ++i) out.insert({fourcc, list[i]});
    std::free(mods);
    return out;
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

// The layouts this GPU can export a copy target in that the compositor or
// X server also imports, with how many memory planes each has.
std::map<uint64_t, uint32_t> shared_modifiers(const Device& d,
                                              const std::set<std::pair<uint32_t, uint64_t>>& importable,
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
        if (importable.count({fourcc, m.drmFormatModifier}) == 0) continue;
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

// A buffer's image, in one of the shared layouts, exported as a dma-buf.
struct Exported {
    int fd = -1;
    uint64_t modifier = 0;
    uint32_t planes = 0;
    VkSubresourceLayout layout[4]{};
};

bool export_buffer_image(const Device& d, const Swapchain& sc, VkFormat format,
                         const std::map<uint64_t, uint32_t>& modifiers, Buffer& b,
                         Exported& e) {
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
    if (planes == modifiers.end() || planes->second > 4) return false;
    e.modifier = chosen.drmFormatModifier;
    e.planes = planes->second;
    static const VkImageAspectFlagBits kPlane[] = {
        VK_IMAGE_ASPECT_MEMORY_PLANE_0_BIT_EXT, VK_IMAGE_ASPECT_MEMORY_PLANE_1_BIT_EXT,
        VK_IMAGE_ASPECT_MEMORY_PLANE_2_BIT_EXT, VK_IMAGE_ASPECT_MEMORY_PLANE_3_BIT_EXT};
    for (uint32_t p = 0; p < e.planes; ++p) {
        VkImageSubresource sub{};
        sub.aspectMask = kPlane[p];
        d.vk.vkGetImageSubresourceLayout(d.device, b.image, &sub, &e.layout[p]);
    }

    VkMemoryGetFdInfoKHR gi{};
    gi.sType = VK_STRUCTURE_TYPE_MEMORY_GET_FD_INFO_KHR;
    gi.memory = b.memory;
    gi.handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT;
    return d.vk.vkGetMemoryFdKHR(d.device, &gi, &e.fd) == VK_SUCCESS;
}

bool make_wayland_buffer(Swapchain& sc, Buffer& b, Exported& e) {
    Compositor& c = *sc.target.compositor;
    const int fd = e.fd;
    e.fd = -1;
    zwp_linux_buffer_params_v1* params = zwp_linux_dmabuf_v1_create_params(c.dmabuf);
    for (uint32_t p = 0; p < e.planes; ++p) {
        // libwayland duplicates the fd when it marshals, so one serves
        // every plane and is closed below.
        zwp_linux_buffer_params_v1_add(params, fd, p, static_cast<uint32_t>(e.layout[p].offset),
                                       static_cast<uint32_t>(e.layout[p].rowPitch),
                                       static_cast<uint32_t>(e.modifier >> 32),
                                       static_cast<uint32_t>(e.modifier));
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
                    static_cast<unsigned long long>(e.modifier));
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

// X11 has no explicit sync before DRI3 1.4, so a buffer's fences travel
// in the dma-buf itself (implicit sync): the copy's completion is put into
// it on present, and whatever the server's GPU still does with it is read
// back out before reuse. Both are the kernel's dma-buf sync-file calls.
// Asked once per buffer here, so a kernel without them leaves the driver
// presenting instead of failing on the first frame.
bool dmabuf_fences_work(int dmabuf) {
    dma_buf_export_sync_file out{};
    out.flags = DMA_BUF_SYNC_WRITE;
    out.fd = -1;
    if (ioctl(dmabuf, DMA_BUF_IOCTL_EXPORT_SYNC_FILE, &out) != 0) return false;
    close(out.fd);
    return true;
}

bool make_x11_buffer(Swapchain& sc, Buffer& b, Exported& e) {
    const Xcb& f = xcb();
    xcb_connection_t* conn = sc.target.x->conn;
    b.dmabuf = e.fd;
    e.fd = -1;
    if (!dmabuf_fences_work(b.dmabuf)) {
        std::printf("stud-render-host: this kernel cannot pass fences through a dma-buf; "
                    "the driver presents\n");
        std::fflush(stdout);
        return false;
    }
    // xcb closes the fds it sends, so the server gets copies and the
    // dma-buf stays open here.
    int32_t fds[4] = {-1, -1, -1, -1};
    uint32_t stride[4] = {};
    uint32_t offset[4] = {};
    for (uint32_t p = 0; p < e.planes; ++p) {
        fds[p] = dup(b.dmabuf);
        stride[p] = static_cast<uint32_t>(e.layout[p].rowPitch);
        offset[p] = static_cast<uint32_t>(e.layout[p].offset);
    }
    b.pixmap = f.generate_id(conn);
    xcb_generic_error_t* error = f.request_check(
        conn, f.pixmap_from_buffers(conn, b.pixmap, sc.target.window,
                                    static_cast<uint8_t>(e.planes),
                                    static_cast<uint16_t>(sc.extent.width),
                                    static_cast<uint16_t>(sc.extent.height), stride[0], offset[0],
                                    stride[1], offset[1], stride[2], offset[2], stride[3],
                                    offset[3], sc.depth, 32, e.modifier, fds));
    if (error != nullptr) {
        std::free(error);
        b.pixmap = 0;
        std::printf("stud-render-host: the X server refused a %ux%u buffer in layout 0x%llx\n",
                    sc.extent.width, sc.extent.height,
                    static_cast<unsigned long long>(e.modifier));
        std::fflush(stdout);
        return false;
    }
    return true;
}

// Every IdleNotify the server has sent: the pixmaps it no longer shows.
void drain_x_events(Swapchain& sc) {
    const Xcb& f = xcb();
    while (xcb_generic_event_t* event = f.poll_special(sc.target.x->conn, sc.events)) {
        const auto* present = reinterpret_cast<const xcb_present_generic_event_t*>(event);
        if (present->evtype == XCB_PRESENT_EVENT_IDLE_NOTIFY) {
            const auto* idle = reinterpret_cast<const xcb_present_idle_notify_event_t*>(event);
            for (Buffer& b : sc.buffers) {
                if (b.pixmap == idle->pixmap) b.held = false;
            }
        }
        std::free(event);
    }
}

// Whether the compositor or X server is done with a buffer. Polled, never
// waited for.
bool buffer_free(const Swapchain& sc, Buffer& b) {
    if (sc.target.x != nullptr) {
        if (b.held) return false;
        // Idle, and no GPU of the server's still reading it.
        dma_buf_export_sync_file out{};
        out.flags = DMA_BUF_SYNC_WRITE;
        out.fd = -1;
        if (ioctl(b.dmabuf, DMA_BUF_IOCTL_EXPORT_SYNC_FILE, &out) != 0) return true;
        pollfd pfd{out.fd, POLLIN, 0};
        const bool signalled = poll(&pfd, 1, 0) == 1;
        close(out.fd);
        return signalled;
    }
    if (b.point == 0) return true;
    uint32_t handle = b.syncobj;
    uint64_t point = b.point;
    return drmSyncobjTimelineWait(sc.target.compositor->drm_fd, &handle, &point, 1, 0, 0,
                                  nullptr) == 0;
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
    // The compositor or X server holds its own references to whatever it
    // still shows: destroying these only ends Stud's.
    xcb_connection_t* conn = sc->target.x != nullptr ? sc->target.x->conn : nullptr;
    if (sc->events != nullptr) {
        xcb().select_input(conn, sc->eid, sc->target.window, 0);
        xcb().unregister_special(conn, sc->events);
    }
    for (Buffer& b : sc->buffers) {
        if (b.pixmap != 0) xcb().free_pixmap(conn, b.pixmap);
        if (b.dmabuf >= 0) close(b.dmabuf);
        if (b.wl != nullptr) wl_buffer_destroy(b.wl);
        if (b.timeline != nullptr) wp_linux_drm_syncobj_timeline_v1_destroy(b.timeline);
        if (b.syncobj != 0) drmSyncobjDestroy(sc->target.compositor->drm_fd, b.syncobj);
        if (b.image != VK_NULL_HANDLE) d.vk.vkDestroyImage(d.device, b.image, nullptr);
        if (b.memory != VK_NULL_HANDLE) d.vk.vkFreeMemory(d.device, b.memory, nullptr);
    }
    if (sc->target.compositor != nullptr) wl_display_flush(sc->target.compositor->display);
    if (conn != nullptr) xcb().flush(conn);
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
    uint8_t depth = 0;
    VkFormat buffer_format = VK_FORMAT_UNDEFINED;
    std::map<uint64_t, uint32_t> modifiers;
    bool same_gpu = false;
    if (target != s.targets.end()) {
        const Target& t = target->second;
        if (t.compositor != nullptr && usable(*t.compositor) &&
            drm_format_for(ci->imageFormat, &fourcc, &buffer_format)) {
            same_gpu = on_gpu(d.physical_device, t.compositor->main_device);
            modifiers = shared_modifiers(d, t.compositor->importable, buffer_format, fourcc,
                                         ci->imageExtent);
        } else if (t.x != nullptr && t.x->usable &&
                   x_format_for(ci->imageFormat, &depth, &fourcc, &buffer_format)) {
            same_gpu = on_gpu(d.physical_device, t.x->main_device);
            modifiers = shared_modifiers(d, x_importable(*t.x, t.window, depth, fourcc),
                                         buffer_format, fourcc, ci->imageExtent);
        }
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
    sc->depth = depth;
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
        Exported e;
        ok = export_buffer_image(d, *sc, buffer_format, modifiers, b, e);
        if (ok) ok = sc->target.x != nullptr ? make_x11_buffer(*sc, b, e) : make_wayland_buffer(*sc, b, e);
        if (e.fd >= 0) close(e.fd);
    }
    if (ok && sc->target.x != nullptr) {
        // Present's events for this swapchain, on a queue of its own so
        // neither Xlib nor the driver ever sees them.
        const Xcb& f = xcb();
        xcb_connection_t* conn = sc->target.x->conn;
        sc->eid = f.generate_id(conn);
        xcb_generic_error_t* error = f.request_check(
            conn, f.select_input_checked(conn, sc->eid, sc->target.window,
                                         XCB_PRESENT_EVENT_MASK_IDLE_NOTIFY));
        ok = error == nullptr;
        std::free(error);
        if (ok) sc->events = f.register_special(conn, f.present_id, sc->eid, nullptr);
    } else if (ok && s.syncobj_surfaces.count(sc->target.surface) == 0) {
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
                    "the %s GPU, %s sync\n",
                    count, sc->same_gpu ? "device-memory" : "system-memory (another GPU)",
                    sc->target.x != nullptr ? "X server's" : "compositor's",
                    sc->target.x != nullptr ? "implicit" : "explicit");
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

// X11's half of a present: the copy's completion goes into the dma-buf
// for the server's GPU to wait on, and the pixmap is presented. No target
// time and no flags: the server shows it at the next refresh, and a later
// present for the same refresh replaces it, the latest frame winning as it
// does on Wayland. Takes `done`.
VkResult present_x11(Swapchain& sc, Buffer& buffer, int done) {
    if (done >= 0) {
        dma_buf_import_sync_file in{};
        in.flags = DMA_BUF_SYNC_WRITE;
        in.fd = done;
        const int r = ioctl(buffer.dmabuf, DMA_BUF_IOCTL_IMPORT_SYNC_FILE, &in);
        close(done);
        if (r != 0) return VK_ERROR_SURFACE_LOST_KHR;
    }
    const Xcb& f = xcb();
    xcb_connection_t* conn = sc.target.x->conn;
    buffer.held = true;
    f.present_pixmap(conn, sc.target.window, buffer.pixmap, ++sc.serial, 0, 0, 0, 0, 0, 0, 0,
                     XCB_PRESENT_OPTION_NONE, 0, 0, 0, 0, nullptr);
    f.flush(conn);
    return VK_SUCCESS;
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
    im.acquired = false;

    if (sc->target.x != nullptr) drain_x_events(*sc);
    Buffer* buffer = nullptr;
    const auto nb = static_cast<uint32_t>(sc->buffers.size());
    for (uint32_t step = 0; step < nb && buffer == nullptr; ++step) {
        const uint32_t b = (sc->next_buffer + step) % nb;
        if (buffer_free(*sc, sc->buffers[b])) {
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
            std::printf("stud-render-host: the %s still holds every buffer; dropping "
                        "the frame instead of waiting for one\n",
                        sc->target.x != nullptr ? "X server" : "compositor");
            std::fflush(stdout);
        }
        return VK_SUCCESS;
    }

    if (sc->target.x != nullptr) return present_x11(*sc, *buffer, done);

    // The copy's completion, as the buffer's acquire point.
    Compositor& c = *sc->target.compositor;
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

    // Made with the swapchain, and again here if forget_surface() has given
    // it back since: a present must never reach the compositor without one.
    wp_linux_drm_syncobj_surface_v1*& sync = s.syncobj_surfaces[sc->target.surface];
    if (sync == nullptr) {
        sync = wp_linux_drm_syncobj_manager_v1_get_surface(c.syncobj, sc->target.surface);
    }
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

void note_xcb_surface(VkSurfaceKHR surface, xcb_connection_t* connection, uint32_t window) {
    if (surface == VK_NULL_HANDLE || connection == nullptr || window == 0) return;
    State& s = state();
    std::lock_guard<std::mutex> lock(s.mutex);
    auto [it, fresh] = s.x_servers.try_emplace(connection);
    if (fresh) open_x_server(it->second, connection, window);
    Target t;
    t.x = &it->second;
    t.window = window;
    s.targets[surface] = t;
}

void note_xlib_surface(VkSurfaceKHR surface, void* display, unsigned long window) {
    if (display == nullptr || xcb().xlib_connection == nullptr) return;
    note_xcb_surface(surface, xcb().xlib_connection(display), static_cast<uint32_t>(window));
}

// A wl_surface takes one syncobj surface for its whole life, and asking
// for a second is a protocol error that ends the window's connection. So
// it is given back with the VkSurfaceKHR it was made for, and a wl_surface
// made later at the same address starts clean.
void forget_surface(VkSurfaceKHR surface) {
    State& s = state();
    std::lock_guard<std::mutex> lock(s.mutex);
    const auto it = s.targets.find(surface);
    if (it == s.targets.end()) return;
    const Target gone = it->second;
    s.targets.erase(it);
    if (gone.surface == nullptr) return;
    for (const auto& t : s.targets) {
        if (t.second.surface == gone.surface) return;
    }
    const auto sync = s.syncobj_surfaces.find(gone.surface);
    if (sync == s.syncobj_surfaces.end()) return;
    wp_linux_drm_syncobj_surface_v1_destroy(sync->second);
    s.syncobj_surfaces.erase(sync);
    wl_display_flush(gone.compositor->display);
}

void note_wayland_surface(VkSurfaceKHR surface, wl_display* display, wl_surface* target) {
    if (surface == VK_NULL_HANDLE || display == nullptr || target == nullptr) return;
    State& s = state();
    std::lock_guard<std::mutex> lock(s.mutex);
    auto [it, fresh] = s.compositors.try_emplace(display);
    if (fresh) open_compositor(it->second, display);
    Target t;
    t.compositor = &it->second;
    t.surface = target;
    s.targets[surface] = t;
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
