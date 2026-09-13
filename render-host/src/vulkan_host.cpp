// Real Vulkan, host side. Process C is ordinary glibc, so the vendor
// driver is at home here -- which is exactly why the driver must never
// be loaded in Process B (see the engineering notes' non-negotiable
// constraints, and vulkan_forward.h for the transport's own rationale).
//
// The loader is dlopen'd rather than linked, matching how this process
// already loads ANGLE: render-host must keep starting on a machine with
// no usable Vulkan at all, and answer "no" honestly instead of failing
// to launch.

#include "stud/vulkan_host.h"
#include "stud/android_glue.h"
#include "stud/session_log.h"

#include <dlfcn.h>
#include <sys/syscall.h>
#include <unistd.h>

#include <chrono>
#include <cstdio>
#include <cstring>
#include <fcntl.h>
#include <sys/mman.h>
#include <unistd.h>
#include <mutex>
#include <set>
#include <unordered_map>
#include <atomic>
#include <map>
#include <set>
#include <string>
#include <algorithm>
#include <type_traits>
#include <vector>

#include <vulkan/vulkan_core.h>

#include "stud/vulkan_forward.h"

namespace stud::render_host {

// The one real window this process owns, supplied once at startup by
// main(). Kept as plain numbers so nothing here can accidentally create
// a window as a side effect of a query.
namespace {
// The extent each live swapchain was actually created with. Recorded so
// a resize can be seen; NOT used to force VK_ERROR_OUT_OF_DATE_KHR --
// see swapchain_is_out_of_date's own comment for the live result of
// trying that.
std::map<uint64_t, VkExtent2D> g_swapchain_extents;

// Atomic because they are written by whichever thread pumps Wayland and
// read by whichever thread is answering a surface-capabilities query --
// and after the secondary-connection change those are routinely
// different threads. Relaxed is enough: each is read on its own, and a
// reader that catches a resize one query late simply asks again.
std::atomic<uint32_t> g_window_width{0};
std::atomic<uint32_t> g_window_height{0};

// Whether this process offers Vulkan at all -- see the header. Set once,
// from main(), before any client can connect.
bool g_vulkan_enabled = true;

// The GPU the user picked in Settings, as an index into
// vkEnumeratePhysicalDevices' own order -- the same identifier the
// settings window enumerated against. Until now nothing read it back,
// so the choice did nothing at all.
uint32_t g_preferred_device_index = 0;

// Which WSI this process presents through, so instance creation asks for
// the extension that actually exists on this session. Set once from
// main(), before any client connects.
bool g_on_x11 = false;
}  // namespace

// Bumped every time the window changes size, so tracing that is capped
// at "the first N passes" can re-arm instead of only ever describing
// startup. A resize bug happens minutes into a session; a trace that
// stopped in the first second cannot see it.
std::atomic<uint32_t> g_resize_generation{0};

void vk_set_vulkan_enabled(bool enabled) { g_vulkan_enabled = enabled; }

void vk_set_preferred_device_index(uint32_t index) { g_preferred_device_index = index; }

void vk_set_on_x11(bool on_x11) { g_on_x11 = on_x11; }

void vk_set_window_size(uint32_t width, uint32_t height) {
    if (width != g_window_width.load(std::memory_order_relaxed) ||
        height != g_window_height.load(std::memory_order_relaxed)) {
        g_resize_generation.fetch_add(1, std::memory_order_relaxed);
    }
    g_window_width.store(width, std::memory_order_relaxed);
    g_window_height.store(height, std::memory_order_relaxed);
}

namespace {

namespace vk_wire = stud::render_host::vk_wire;

// The wire structs must stay byte-compatible with the real ones, since
// the property arrays travel as fixed-stride blocks.
static_assert(vk_wire::kMaxExtensionNameSize == VK_MAX_EXTENSION_NAME_SIZE,
              "wire extension-name size must match the real Vulkan constant");
static_assert(vk_wire::kMaxDescriptionSize == VK_MAX_DESCRIPTION_SIZE,
              "wire description size must match the real Vulkan constant");

struct Loader {
    void* handle = nullptr;
    PFN_vkGetInstanceProcAddr get_instance_proc_addr = nullptr;
    PFN_vkEnumerateInstanceVersion enumerate_instance_version = nullptr;
    PFN_vkEnumerateInstanceExtensionProperties enumerate_instance_extension_properties = nullptr;
    PFN_vkEnumerateInstanceLayerProperties enumerate_instance_layer_properties = nullptr;
    PFN_vkCreateInstance create_instance = nullptr;
    bool tried = false;

    // The instance this process created on the engine's behalf, plus the
    // instance-level commands resolved from it. Roblox creates exactly
    // one; keeping it here is what lets a later physical-device call
    // resolve its own entry points.
    VkInstance instance = VK_NULL_HANDLE;
    PFN_vkEnumeratePhysicalDevices enumerate_physical_devices = nullptr;
    PFN_vkGetPhysicalDeviceProperties get_physical_device_properties = nullptr;
    PFN_vkGetPhysicalDeviceFeatures get_physical_device_features = nullptr;
    PFN_vkGetPhysicalDeviceMemoryProperties get_physical_device_memory_properties = nullptr;
    PFN_vkGetPhysicalDeviceQueueFamilyProperties get_physical_device_queue_family_properties =
        nullptr;
    PFN_vkEnumerateDeviceExtensionProperties enumerate_device_extension_properties = nullptr;
    PFN_vkGetMemoryHostPointerPropertiesEXT get_memory_host_pointer_properties = nullptr;
    PFN_vkGetPhysicalDeviceFeatures2 get_physical_device_features2 = nullptr;
    PFN_vkCreateDevice create_device = nullptr;
    PFN_vkGetPhysicalDeviceFormatProperties get_physical_device_format_properties = nullptr;
    PFN_vkGetPhysicalDeviceImageFormatProperties get_physical_device_image_format_properties =
        nullptr;
    PFN_vkGetPhysicalDeviceSurfaceCapabilitiesKHR get_physical_device_surface_capabilities =
        nullptr;

    // Device-level commands, resolved from the device itself once it
    // exists -- the spec's own requirement, and what reaches the
    // driver's real implementations rather than loader trampolines.
    VkDevice device = VK_NULL_HANDLE;
    PFN_vkGetDeviceProcAddr get_device_proc_addr = nullptr;
    PFN_vkGetDeviceQueue get_device_queue = nullptr;
    PFN_vkCreateCommandPool create_command_pool = nullptr;
    PFN_vkCreateSemaphore create_semaphore = nullptr;
    PFN_vkCreateFence create_fence = nullptr;
    PFN_vkCreateQueryPool create_query_pool = nullptr;
    PFN_vkCreatePipelineCache create_pipeline_cache = nullptr;
    PFN_vkGetPipelineCacheData get_pipeline_cache_data = nullptr;
    PFN_vkDestroyPipelineCache destroy_pipeline_cache = nullptr;
    // The rest of the destroy family. Without these the objects the
    // engine creates for every pipeline it builds were never freed --
    // measured at 2668 unimplemented destroy calls across a few
    // sessions, which is GPU memory that grows with every join.
    PFN_vkDestroySampler destroy_sampler = nullptr;
    PFN_vkDestroyRenderPass destroy_render_pass = nullptr;
    PFN_vkDestroyPipeline destroy_pipeline = nullptr;
    PFN_vkDestroyPipelineLayout destroy_pipeline_layout = nullptr;
    PFN_vkDestroyDescriptorSetLayout destroy_descriptor_set_layout = nullptr;
    PFN_vkDestroyDescriptorPool destroy_descriptor_pool = nullptr;
    PFN_vkDestroyDescriptorUpdateTemplate destroy_descriptor_update_template = nullptr;
    PFN_vkCreateImage create_image = nullptr;
    PFN_vkGetImageMemoryRequirements get_image_memory_requirements = nullptr;
    PFN_vkAllocateMemory allocate_memory = nullptr;
    PFN_vkBindImageMemory bind_image_memory = nullptr;
    PFN_vkFreeMemory free_memory = nullptr;
    PFN_vkMapMemory map_memory = nullptr;
    PFN_vkUnmapMemory unmap_memory = nullptr;
    PFN_vkFlushMappedMemoryRanges flush_mapped_memory_ranges = nullptr;
    PFN_vkCreateSwapchainKHR create_swapchain = nullptr;
    PFN_vkGetSwapchainImagesKHR get_swapchain_images = nullptr;
    PFN_vkGetPhysicalDeviceSurfaceFormatsKHR get_surface_formats = nullptr;
    PFN_vkGetPhysicalDeviceSurfacePresentModesKHR get_surface_present_modes = nullptr;
    PFN_vkGetPhysicalDeviceSurfaceSupportKHR get_surface_support = nullptr;
    PFN_vkGetPhysicalDeviceImageFormatProperties2 get_image_format_properties2 = nullptr;
    PFN_vkDeviceWaitIdle device_wait_idle = nullptr;
    PFN_vkCreateBuffer create_buffer = nullptr;
    PFN_vkGetBufferMemoryRequirements get_buffer_memory_requirements = nullptr;
    PFN_vkBindBufferMemory bind_buffer_memory = nullptr;
    PFN_vkCreateImageView create_image_view = nullptr;
    PFN_vkCreateShaderModule create_shader_module = nullptr;
    PFN_vkDestroyBuffer destroy_buffer = nullptr;
    PFN_vkDestroyImage destroy_image = nullptr;
    PFN_vkDestroyImageView destroy_image_view = nullptr;
    PFN_vkDestroyShaderModule destroy_shader_module = nullptr;
    PFN_vkDestroySemaphore destroy_semaphore = nullptr;
    PFN_vkDestroyFence destroy_fence = nullptr;
    PFN_vkDestroyCommandPool destroy_command_pool = nullptr;
    PFN_vkDestroyQueryPool destroy_query_pool = nullptr;
    PFN_vkDestroySwapchainKHR destroy_swapchain = nullptr;
    PFN_vkDestroySurfaceKHR destroy_surface = nullptr;
    PFN_vkDestroyDevice destroy_device = nullptr;
    PFN_vkDestroyInstance destroy_instance = nullptr;
    PFN_vkCmdCopyImageToBuffer cmd_copy_image_to_buffer = nullptr;
    PFN_vkQueueWaitIdle queue_wait_idle = nullptr;
    VkPhysicalDevice physical_device = VK_NULL_HANDLE;
    VkCommandPool probe_pool = VK_NULL_HANDLE;
    VkQueue probe_queue = VK_NULL_HANDLE;
    PFN_vkCreateRenderPass create_render_pass = nullptr;
    PFN_vkCreateFramebuffer create_framebuffer = nullptr;
    PFN_vkDestroyFramebuffer destroy_framebuffer = nullptr;
    PFN_vkCreateSampler create_sampler = nullptr;
    PFN_vkCreatePipelineLayout create_pipeline_layout = nullptr;
    PFN_vkCreateDescriptorSetLayout create_descriptor_set_layout = nullptr;
    PFN_vkCreateDescriptorPool create_descriptor_pool = nullptr;
    PFN_vkAllocateDescriptorSets allocate_descriptor_sets = nullptr;
    PFN_vkResetDescriptorPool reset_descriptor_pool = nullptr;
    PFN_vkCreateDescriptorUpdateTemplate create_descriptor_update_template = nullptr;
    PFN_vkUpdateDescriptorSetWithTemplate update_descriptor_set_with_template = nullptr;
    PFN_vkCreateGraphicsPipelines create_graphics_pipelines = nullptr;
    PFN_vkCreateComputePipelines create_compute_pipelines = nullptr;
    PFN_vkAllocateCommandBuffers allocate_command_buffers = nullptr;
    PFN_vkBeginCommandBuffer begin_command_buffer = nullptr;
    PFN_vkEndCommandBuffer end_command_buffer = nullptr;
    PFN_vkResetCommandPool reset_command_pool = nullptr;
    PFN_vkQueueSubmit queue_submit = nullptr;
    PFN_vkWaitForFences wait_for_fences = nullptr;
    PFN_vkResetFences reset_fences = nullptr;
    PFN_vkAcquireNextImageKHR acquire_next_image = nullptr;
    PFN_vkQueuePresentKHR queue_present = nullptr;
    PFN_vkGetQueryPoolResults get_query_pool_results = nullptr;
    PFN_vkCmdBeginRenderPass cmd_begin_render_pass = nullptr;
    PFN_vkCmdEndRenderPass cmd_end_render_pass = nullptr;
    PFN_vkCmdBindPipeline cmd_bind_pipeline = nullptr;
    PFN_vkCmdBindDescriptorSets cmd_bind_descriptor_sets = nullptr;
    PFN_vkCmdBindVertexBuffers cmd_bind_vertex_buffers = nullptr;
    PFN_vkCmdBindIndexBuffer cmd_bind_index_buffer = nullptr;
    PFN_vkCmdDraw cmd_draw = nullptr;
    PFN_vkCmdDrawIndexed cmd_draw_indexed = nullptr;
    PFN_vkCmdDispatch cmd_dispatch = nullptr;
    PFN_vkCmdSetViewport cmd_set_viewport = nullptr;
    PFN_vkCmdSetScissor cmd_set_scissor = nullptr;
    PFN_vkCmdPipelineBarrier cmd_pipeline_barrier = nullptr;
    PFN_vkCmdCopyBuffer cmd_copy_buffer = nullptr;
    PFN_vkCmdCopyBufferToImage cmd_copy_buffer_to_image = nullptr;
    PFN_vkCmdCopyImage cmd_copy_image = nullptr;
    PFN_vkCmdBlitImage cmd_blit_image = nullptr;
    PFN_vkCmdResolveImage cmd_resolve_image = nullptr;
    PFN_vkCmdResetQueryPool cmd_reset_query_pool = nullptr;
    PFN_vkCmdWriteTimestamp cmd_write_timestamp = nullptr;

    // Layout of each descriptor-update template, needed to interpret the
    // opaque blob a template update carries.
    std::map<uint64_t, std::vector<VkDescriptorUpdateTemplateEntry>> template_entries;


    // Live host-side mappings, keyed by VkDeviceMemory. The engine writes
    // into its own staging copy in Process B; these are where those bytes
    // land.
    std::map<uint64_t, void*> mapped;

    // Which objects lead to the screen. A frame can look perfectly busy
    // -- thousands of draws, successful presents -- and still be black if
    // nothing the engine renders ever targets a swapchain image, so this
    // follows image -> view -> framebuffer and reports whether any render
    // pass or blit actually writes to one.
    std::set<uint64_t> swapchain_images;
    // Which images belong to which swapchain, and which images belonged
    // to one that has since been destroyed.
    //
    // vkDestroySwapchainKHR frees its images with it. An application is
    // not supposed to use them afterwards -- but a resize makes the
    // engine do exactly that: vkAcquireNextImageKHR fails with
    // VK_ERROR_OUT_OF_DATE_KHR, the engine tears the swapchain down, and
    // a command buffer already in flight still records a barrier against
    // the image index it never successfully acquired. On a real device
    // that call goes straight into the driver in the same process and is
    // undefined behaviour; here it crosses a socket first, which means
    // Stud can decline to pass a handle it knows is dead instead of
    // segfaulting the driver in render-host.
    std::map<uint64_t, std::vector<uint64_t>> swapchain_image_list;
    std::set<uint64_t> retired_images;
    std::set<uint64_t> swapchain_views;
    std::set<uint64_t> swapchain_framebuffers;

    // Whether the command buffer currently being recorded is inside a
    // render pass that targets the screen, and what happens in there.
    bool in_screen_pass = false;
    uint64_t screen_draws = 0;
    // Draws in the render pass currently being recorded, whichever pass
    // that is. The screen counters only ever described the final
    // composite; a scene that goes black while the UI still works needs
    // the count for the pass that renders the scene.
    uint64_t pass_draws = 0;
    int traced_pass = -1;
    uint64_t screen_binds = 0;
};

Loader& loader() {
    static Loader l;
    if (!l.tried) {
        l.tried = true;
        // Same order the Vulkan spec expects a loader to be found in.
        for (const char* name : {"libvulkan.so.1", "libvulkan.so"}) {
            l.handle = ::dlopen(name, RTLD_NOW | RTLD_LOCAL);
            if (l.handle != nullptr) break;
        }
        if (l.handle == nullptr) {
            std::printf("stud-render-host: no real Vulkan loader (%s)\n",
                        ::dlerror() != nullptr ? "dlopen failed" : "unknown");
            std::fflush(stdout);
            return l;
        }
        l.get_instance_proc_addr = reinterpret_cast<PFN_vkGetInstanceProcAddr>(
            ::dlsym(l.handle, "vkGetInstanceProcAddr"));
        if (l.get_instance_proc_addr == nullptr) return l;
        auto global = [&l](const char* n) {
            return l.get_instance_proc_addr(VK_NULL_HANDLE, n);
        };
        l.enumerate_instance_version =
            reinterpret_cast<PFN_vkEnumerateInstanceVersion>(global("vkEnumerateInstanceVersion"));
        l.enumerate_instance_extension_properties =
            reinterpret_cast<PFN_vkEnumerateInstanceExtensionProperties>(
                global("vkEnumerateInstanceExtensionProperties"));
        l.enumerate_instance_layer_properties =
            reinterpret_cast<PFN_vkEnumerateInstanceLayerProperties>(
                global("vkEnumerateInstanceLayerProperties"));
        l.create_instance =
            reinterpret_cast<PFN_vkCreateInstance>(global("vkCreateInstance"));
        std::printf("stud-render-host: real Vulkan loader ready\n");
        std::fflush(stdout);
    }
    return l;
}

}  // namespace

uint64_t vk_enumerate_instance_version(std::vector<uint8_t>& out, uint32_t* out_len) {
    Loader& l = loader();
    // vkEnumerateInstanceVersion is Vulkan 1.1+. Its absence is not an
    // error: a 1.0 loader simply has no such command, and the honest
    // answer is the 1.0 version number.
    uint32_t version = VK_API_VERSION_1_0;
    VkResult r = VK_SUCCESS;
    if (l.enumerate_instance_version != nullptr) {
        r = l.enumerate_instance_version(&version);
    } else if (l.get_instance_proc_addr == nullptr) {
        return static_cast<uint64_t>(static_cast<int32_t>(VK_ERROR_INITIALIZATION_FAILED));
    }
    out.resize(sizeof(version));
    std::memcpy(out.data(), &version, sizeof(version));
    *out_len = sizeof(version);
    return static_cast<uint64_t>(static_cast<int32_t>(r));
}

uint64_t vk_enumerate_instance_extension_properties(const std::vector<uint8_t>& in,
                                                     uint32_t capacity,
                                                     std::vector<uint8_t>& out,
                                                     uint32_t* out_len) {
    Loader& l = loader();
    if (l.enumerate_instance_extension_properties == nullptr) {
        return static_cast<uint64_t>(static_cast<int32_t>(VK_ERROR_INITIALIZATION_FAILED));
    }
    std::string layer;
    if (!in.empty()) layer.assign(reinterpret_cast<const char*>(in.data()));
    const char* layer_ptr = layer.empty() ? nullptr : layer.c_str();

    // Always ask the driver for the true count first, so the reply can
    // report it even when the caller's array is too small -- that is what
    // the real two-call idiom needs in order to work.
    uint32_t count = 0;
    VkResult r = l.enumerate_instance_extension_properties(layer_ptr, &count, nullptr);
    if (r != VK_SUCCESS && r != VK_INCOMPLETE) {
        return static_cast<uint64_t>(static_cast<int32_t>(r));
    }

    std::vector<VkExtensionProperties> props;
    uint32_t returned = 0;
    if (capacity > 0 && count > 0) {
        returned = count < capacity ? count : capacity;
        props.resize(returned);
        r = l.enumerate_instance_extension_properties(layer_ptr, &returned, props.data());
    }

    const uint32_t reported = capacity > 0 ? returned : count;
    out.resize(sizeof(uint32_t) + sizeof(vk_wire::ExtensionProperties) * returned);
    std::memcpy(out.data(), &reported, sizeof(reported));
    for (uint32_t i = 0; i < returned; ++i) {
        vk_wire::ExtensionProperties w{};
        std::snprintf(w.extension_name, sizeof(w.extension_name), "%s", props[i].extensionName);
        w.spec_version = props[i].specVersion;
        std::memcpy(out.data() + sizeof(uint32_t) + i * sizeof(w), &w, sizeof(w));
    }
    *out_len = static_cast<uint32_t>(out.size());
    return static_cast<uint64_t>(static_cast<int32_t>(r));
}

uint64_t vk_enumerate_instance_layer_properties(uint32_t capacity, std::vector<uint8_t>& out,
                                                 uint32_t* out_len) {
    Loader& l = loader();
    if (l.enumerate_instance_layer_properties == nullptr) {
        return static_cast<uint64_t>(static_cast<int32_t>(VK_ERROR_INITIALIZATION_FAILED));
    }
    uint32_t count = 0;
    VkResult r = l.enumerate_instance_layer_properties(&count, nullptr);
    if (r != VK_SUCCESS && r != VK_INCOMPLETE) {
        return static_cast<uint64_t>(static_cast<int32_t>(r));
    }

    std::vector<VkLayerProperties> props;
    uint32_t returned = 0;
    if (capacity > 0 && count > 0) {
        returned = count < capacity ? count : capacity;
        props.resize(returned);
        r = l.enumerate_instance_layer_properties(&returned, props.data());
    }

    const uint32_t reported = capacity > 0 ? returned : count;
    out.resize(sizeof(uint32_t) + sizeof(vk_wire::LayerProperties) * returned);
    std::memcpy(out.data(), &reported, sizeof(reported));
    for (uint32_t i = 0; i < returned; ++i) {
        vk_wire::LayerProperties w{};
        std::snprintf(w.layer_name, sizeof(w.layer_name), "%s", props[i].layerName);
        w.spec_version = props[i].specVersion;
        w.implementation_version = props[i].implementationVersion;
        std::snprintf(w.description, sizeof(w.description), "%s", props[i].description);
        std::memcpy(out.data() + sizeof(uint32_t) + i * sizeof(w), &w, sizeof(w));
    }
    *out_len = static_cast<uint32_t>(out.size());
    return static_cast<uint64_t>(static_cast<int32_t>(r));
}

uint64_t vk_create_instance(const std::vector<uint8_t>& in, std::vector<uint8_t>& out,
                             uint32_t* out_len) {
    if (!g_vulkan_enabled) {
        // The user asked for OpenGL, so this process does not offer
        // Vulkan. VK_ERROR_INCOMPATIBLE_DRIVER is exactly what a device
        // with no usable Vulkan driver returns, and it is what makes the
        // engine fall back to its own GLES path -- no flag, no knowledge
        // of the engine's internals.
        std::printf("stud-render-host: vkCreateInstance refused -- graphics mode is OpenGL\n");
        std::fflush(stdout);
        return static_cast<uint64_t>(static_cast<int32_t>(VK_ERROR_INCOMPATIBLE_DRIVER));
    }
    Loader& l = loader();
    if (l.create_instance == nullptr) {
        return static_cast<uint64_t>(static_cast<int32_t>(VK_ERROR_INITIALIZATION_FAILED));
    }
    if (in.size() < sizeof(vk_wire::CreateInstanceHeader)) {
        return static_cast<uint64_t>(static_cast<int32_t>(VK_ERROR_INITIALIZATION_FAILED));
    }
    vk_wire::CreateInstanceHeader hdr{};
    std::memcpy(&hdr, in.data(), sizeof(hdr));

    size_t off = sizeof(hdr);
    auto take = [&in, &off](size_t n) -> std::string {
        if (off + n > in.size()) return {};
        std::string s(reinterpret_cast<const char*>(in.data() + off), n);
        off += n;
        return s;
    };
    const std::string app_name = take(hdr.application_name_len);
    const std::string engine_name = take(hdr.engine_name_len);
    auto take_cstr = [&in, &off]() -> std::string {
        if (off >= in.size()) return {};
        std::string s(reinterpret_cast<const char*>(in.data() + off));
        off += s.size() + 1;
        return s;
    };
    std::vector<std::string> layers;
    for (uint32_t i = 0; i < hdr.enabled_layer_count; ++i) layers.push_back(take_cstr());
    std::vector<std::string> extensions;
    for (uint32_t i = 0; i < hdr.enabled_extension_count; ++i) extensions.push_back(take_cstr());

    // The engine is an Android client, so it asks for
    // VK_KHR_android_surface. This process presents to Wayland, where
    // that extension does not exist and its absence would fail instance
    // creation outright. Substituting the Wayland surface extension is
    // the same interposition the surface-creation path already does --
    // the window belongs to Process C either way, and the engine never
    // sees which WSI is underneath.
    for (std::string& e : extensions) {
        if (e == "VK_KHR_android_surface") {
            e = g_on_x11 ? "VK_KHR_xlib_surface" : "VK_KHR_wayland_surface";
        }
    }

    std::vector<const char*> layer_ptrs;
    layer_ptrs.reserve(layers.size());
    for (const std::string& s : layers) layer_ptrs.push_back(s.c_str());
    std::vector<const char*> ext_ptrs;
    ext_ptrs.reserve(extensions.size());
    for (const std::string& s : extensions) ext_ptrs.push_back(s.c_str());

    VkApplicationInfo app{};
    app.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
    app.pApplicationName = app_name.empty() ? nullptr : app_name.c_str();
    app.applicationVersion = hdr.application_version;
    app.pEngineName = engine_name.empty() ? nullptr : engine_name.c_str();
    app.engineVersion = hdr.engine_version;
    app.apiVersion = hdr.api_version;

    VkInstanceCreateInfo ci{};
    ci.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
    ci.flags = hdr.flags;
    ci.pApplicationInfo = hdr.has_application_info != 0 ? &app : nullptr;
    ci.enabledLayerCount = static_cast<uint32_t>(layer_ptrs.size());
    ci.ppEnabledLayerNames = layer_ptrs.empty() ? nullptr : layer_ptrs.data();
    ci.enabledExtensionCount = static_cast<uint32_t>(ext_ptrs.size());
    ci.ppEnabledExtensionNames = ext_ptrs.empty() ? nullptr : ext_ptrs.data();

    VkInstance instance = VK_NULL_HANDLE;
    VkResult r = l.create_instance(&ci, nullptr, &instance);
    std::printf("stud-render-host: vkCreateInstance -> %d (layers=%zu extensions=%zu)\n",
                static_cast<int>(r), layer_ptrs.size(), ext_ptrs.size());
    std::fflush(stdout);
    if (r != VK_SUCCESS) return static_cast<uint64_t>(static_cast<int32_t>(r));

    // Resolve the instance-level commands now, from the instance itself
    // -- the spec's own requirement, and the only way to reach a driver's
    // real implementations rather than the loader's trampolines.
    l.instance = instance;
    auto inst = [&l](const char* n) { return l.get_instance_proc_addr(l.instance, n); };
    l.enumerate_physical_devices =
        reinterpret_cast<PFN_vkEnumeratePhysicalDevices>(inst("vkEnumeratePhysicalDevices"));
    l.get_physical_device_properties = reinterpret_cast<PFN_vkGetPhysicalDeviceProperties>(
        inst("vkGetPhysicalDeviceProperties"));
    l.get_physical_device_features =
        reinterpret_cast<PFN_vkGetPhysicalDeviceFeatures>(inst("vkGetPhysicalDeviceFeatures"));
    l.get_physical_device_memory_properties =
        reinterpret_cast<PFN_vkGetPhysicalDeviceMemoryProperties>(
            inst("vkGetPhysicalDeviceMemoryProperties"));
    l.get_physical_device_queue_family_properties =
        reinterpret_cast<PFN_vkGetPhysicalDeviceQueueFamilyProperties>(
            inst("vkGetPhysicalDeviceQueueFamilyProperties"));
    l.enumerate_device_extension_properties =
        reinterpret_cast<PFN_vkEnumerateDeviceExtensionProperties>(
            inst("vkEnumerateDeviceExtensionProperties"));
    // The engine asks for the KHR alias; either spelling resolves to the
    // same implementation, so accept whichever the driver exposes.
    l.get_physical_device_features2 = reinterpret_cast<PFN_vkGetPhysicalDeviceFeatures2>(
        inst("vkGetPhysicalDeviceFeatures2"));
    if (l.get_physical_device_features2 == nullptr) {
        l.get_physical_device_features2 = reinterpret_cast<PFN_vkGetPhysicalDeviceFeatures2>(
            inst("vkGetPhysicalDeviceFeatures2KHR"));
    }
    l.create_device = reinterpret_cast<PFN_vkCreateDevice>(inst("vkCreateDevice"));
    l.get_physical_device_format_properties =
        reinterpret_cast<PFN_vkGetPhysicalDeviceFormatProperties>(
            inst("vkGetPhysicalDeviceFormatProperties"));
    l.get_physical_device_image_format_properties =
        reinterpret_cast<PFN_vkGetPhysicalDeviceImageFormatProperties>(
            inst("vkGetPhysicalDeviceImageFormatProperties"));
    l.get_physical_device_surface_capabilities =
        reinterpret_cast<PFN_vkGetPhysicalDeviceSurfaceCapabilitiesKHR>(
            inst("vkGetPhysicalDeviceSurfaceCapabilitiesKHR"));
    l.get_device_proc_addr =
        reinterpret_cast<PFN_vkGetDeviceProcAddr>(inst("vkGetDeviceProcAddr"));
    l.destroy_surface = reinterpret_cast<PFN_vkDestroySurfaceKHR>(inst("vkDestroySurfaceKHR"));
    l.destroy_instance = reinterpret_cast<PFN_vkDestroyInstance>(inst("vkDestroyInstance"));
    l.get_surface_formats = reinterpret_cast<PFN_vkGetPhysicalDeviceSurfaceFormatsKHR>(
        inst("vkGetPhysicalDeviceSurfaceFormatsKHR"));
    l.get_surface_present_modes = reinterpret_cast<PFN_vkGetPhysicalDeviceSurfacePresentModesKHR>(
        inst("vkGetPhysicalDeviceSurfacePresentModesKHR"));
    l.get_surface_support = reinterpret_cast<PFN_vkGetPhysicalDeviceSurfaceSupportKHR>(
        inst("vkGetPhysicalDeviceSurfaceSupportKHR"));
    l.get_image_format_properties2 = reinterpret_cast<PFN_vkGetPhysicalDeviceImageFormatProperties2>(
        inst("vkGetPhysicalDeviceImageFormatProperties2"));
    if (l.get_image_format_properties2 == nullptr) {
        l.get_image_format_properties2 =
            reinterpret_cast<PFN_vkGetPhysicalDeviceImageFormatProperties2>(
                inst("vkGetPhysicalDeviceImageFormatProperties2KHR"));
    }

    uint64_t handle = static_cast<uint64_t>(reinterpret_cast<uintptr_t>(instance));
    out.resize(sizeof(handle));
    std::memcpy(out.data(), &handle, sizeof(handle));
    *out_len = sizeof(handle);
    return static_cast<uint64_t>(static_cast<int32_t>(VK_SUCCESS));
}

namespace {

// Every POD reply is prefixed with the size the host wrote, so a Vulkan
// header mismatch between the two processes is caught instead of being
// silently misread as garbage.
template <typename T>
uint64_t write_pod(const T& value, std::vector<uint8_t>& out, uint32_t* out_len) {
    const uint32_t size = static_cast<uint32_t>(sizeof(T));
    out.resize(sizeof(uint32_t) + size);
    std::memcpy(out.data(), &size, sizeof(size));
    std::memcpy(out.data() + sizeof(uint32_t), &value, size);
    *out_len = static_cast<uint32_t>(out.size());
    return static_cast<uint64_t>(static_cast<int32_t>(VK_SUCCESS));
}

}  // namespace

uint64_t vk_enumerate_physical_devices(uint32_t capacity, std::vector<uint8_t>& out,
                                        uint32_t* out_len) {
    Loader& l = loader();
    if (l.enumerate_physical_devices == nullptr || l.instance == VK_NULL_HANDLE) {
        return static_cast<uint64_t>(static_cast<int32_t>(VK_ERROR_INITIALIZATION_FAILED));
    }
    uint32_t count = 0;
    VkResult r = l.enumerate_physical_devices(l.instance, &count, nullptr);
    if (r != VK_SUCCESS && r != VK_INCOMPLETE) {
        return static_cast<uint64_t>(static_cast<int32_t>(r));
    }

    std::vector<VkPhysicalDevice> devices;
    uint32_t returned = 0;
    if (capacity > 0 && count > 0) {
        returned = count < capacity ? count : capacity;
        devices.resize(returned);
        r = l.enumerate_physical_devices(l.instance, &returned, devices.data());
    }

    // Honour the GPU chosen in Settings by putting it first. The engine
    // picks from this list itself and takes the first device it can use,
    // so ordering is the honest way to express a preference -- every
    // device is still offered, exactly as the driver reported it, and a
    // stale index simply leaves the order alone.
    if (g_preferred_device_index != 0 && g_preferred_device_index < devices.size()) {
        std::rotate(devices.begin(), devices.begin() + g_preferred_device_index,
                    devices.begin() + g_preferred_device_index + 1);
        std::printf("stud-render-host: preferring the GPU at index %u, as chosen in Settings\n",
                    g_preferred_device_index);
    }


    const uint32_t reported = capacity > 0 ? returned : count;
    out.resize(sizeof(uint32_t) + sizeof(uint64_t) * returned);
    std::memcpy(out.data(), &reported, sizeof(reported));
    for (uint32_t i = 0; i < returned; ++i) {
        uint64_t h = static_cast<uint64_t>(reinterpret_cast<uintptr_t>(devices[i]));
        std::memcpy(out.data() + sizeof(uint32_t) + i * sizeof(h), &h, sizeof(h));
    }
    *out_len = static_cast<uint32_t>(out.size());
    // The machine's GPUs do not change while Stud runs.
    static bool announced_device_count = false;
    if (!announced_device_count) {
        announced_device_count = true;
        std::printf("stud-render-host: vkEnumeratePhysicalDevices -> %d, %u device(s)\n",
        static_cast<int>(r), count);
        std::fflush(stdout);
    }
    return static_cast<uint64_t>(static_cast<int32_t>(r));
}

std::string vk_device_select_token_for_index(uint32_t index) {
    // Mesa selects its Vulkan device from MESA_VK_DEVICE_SELECT, which
    // names a GPU by vendor and device id rather than by position. Ask
    // the real loader what is at the position the user chose -- the same
    // enumeration order the settings window listed -- and translate.
    Loader& l = loader();
    if (l.create_instance == nullptr) return {};
    VkApplicationInfo app{};
    app.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
    app.apiVersion = VK_API_VERSION_1_0;
    VkInstanceCreateInfo ci{};
    ci.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
    ci.pApplicationInfo = &app;
    VkInstance instance = VK_NULL_HANDLE;
    if (l.create_instance(&ci, nullptr, &instance) != VK_SUCCESS) return {};
    auto inst = [&l, instance](const char* n) { return l.get_instance_proc_addr(instance, n); };
    auto enumerate = reinterpret_cast<PFN_vkEnumeratePhysicalDevices>(inst("vkEnumeratePhysicalDevices"));
    auto get_props = reinterpret_cast<PFN_vkGetPhysicalDeviceProperties>(inst("vkGetPhysicalDeviceProperties"));
    auto destroy = reinterpret_cast<PFN_vkDestroyInstance>(inst("vkDestroyInstance"));
    std::string token;
    if (enumerate != nullptr && get_props != nullptr) {
        uint32_t count = 0;
        enumerate(instance, &count, nullptr);
        std::vector<VkPhysicalDevice> devices(count);
        if (count > 0) enumerate(instance, &count, devices.data());
        if (index < devices.size()) {
            VkPhysicalDeviceProperties props{};
            get_props(devices[index], &props);
            char buf[32];
            std::snprintf(buf, sizeof(buf), "%x:%x", props.vendorID, props.deviceID);
            token = buf;
            std::printf("stud-render-host: GPU %u is %s (%s)\n", index, props.deviceName, buf);
            std::fflush(stdout);
        }
    }
    if (destroy != nullptr) destroy(instance, nullptr);
    return token;
}

uint64_t vk_get_physical_device_properties(uint64_t device, std::vector<uint8_t>& out,
                                            uint32_t* out_len) {
    Loader& l = loader();
    if (l.get_physical_device_properties == nullptr) {
        return static_cast<uint64_t>(static_cast<int32_t>(VK_ERROR_INITIALIZATION_FAILED));
    }
    VkPhysicalDeviceProperties props{};
    l.get_physical_device_properties(
        reinterpret_cast<VkPhysicalDevice>(static_cast<uintptr_t>(device)), &props);
    // Same: one line naming the device Stud renders on.
    static bool announced_device_name = false;
    if (!announced_device_name) {
        announced_device_name = true;
        std::printf("stud-render-host: Vulkan device: %s (API %u.%u.%u)\n", props.deviceName,
        VK_API_VERSION_MAJOR(props.apiVersion), VK_API_VERSION_MINOR(props.apiVersion),
        VK_API_VERSION_PATCH(props.apiVersion));
        std::fflush(stdout);
    }
    return write_pod(props, out, out_len);
}

uint64_t vk_get_physical_device_features(uint64_t device, std::vector<uint8_t>& out,
                                          uint32_t* out_len) {
    Loader& l = loader();
    if (l.get_physical_device_features == nullptr) {
        return static_cast<uint64_t>(static_cast<int32_t>(VK_ERROR_INITIALIZATION_FAILED));
    }
    VkPhysicalDeviceFeatures features{};
    l.get_physical_device_features(
        reinterpret_cast<VkPhysicalDevice>(static_cast<uintptr_t>(device)), &features);
    return write_pod(features, out, out_len);
}

uint64_t vk_get_physical_device_memory_properties(uint64_t device, std::vector<uint8_t>& out,
                                                   uint32_t* out_len) {
    Loader& l = loader();
    if (l.get_physical_device_memory_properties == nullptr) {
        return static_cast<uint64_t>(static_cast<int32_t>(VK_ERROR_INITIALIZATION_FAILED));
    }
    VkPhysicalDeviceMemoryProperties mem{};
    l.get_physical_device_memory_properties(
        reinterpret_cast<VkPhysicalDevice>(static_cast<uintptr_t>(device)), &mem);

    // Cap each heap at just under 4GiB, because the engine truncates
    // heap sizes to 32 bits and a real 4GiB heap therefore reads as
    // ZERO.
    //
    // Proven by its own log rather than assumed. This machine's host
    // heap is 24933841920 bytes and the engine reports 3459005440 --
    // exactly the low 32 bits. Its device heap is 4294967296
    // (0x100000000), whose low 32 bits are 0, and the engine duly logs
    // `heapIndex = 0, heapFlags = 1 (device), heapSize = 0` and then
    // falls back to `caps.videoMemory = 67108864`: it believes a 4GiB
    // GPU has 64MB. That is the worst possible answer -- it drives
    // texture and shadow resolution down and makes the engine stream and
    // evict constantly, which costs CPU every frame.
    //
    // Reporting 0xFFFFFFFF instead of the true size is the honest
    // choice here: it is the largest value the engine can actually
    // represent, so it is far closer to the truth than the zero it
    // derives on its own, and heap size is informational -- the driver,
    // not this number, enforces what can really be allocated. Only
    // sizes that would truncate are touched; anything already under
    // 4GiB is passed through exactly.
    constexpr VkDeviceSize kMax32 = 0xFFFFFFFFull;
    for (uint32_t i = 0; i < mem.memoryHeapCount; ++i) {
        if (mem.memoryHeaps[i].size > kMax32) {
            static std::set<uint32_t> announced_heaps;
            if (announced_heaps.insert(i).second) {
                std::printf("stud-render-host: heap %u is %llu bytes, reporting %llu -- the engine "
                            "truncates heap sizes to 32 bits and would read the real value as "
                            "%llu\n",
                            i, static_cast<unsigned long long>(mem.memoryHeaps[i].size),
                            static_cast<unsigned long long>(kMax32),
                            static_cast<unsigned long long>(mem.memoryHeaps[i].size & 0xFFFFFFFFull));
                std::fflush(stdout);
            }
            mem.memoryHeaps[i].size = kMax32;
        }
    }
    // The device's memory layout does not change, and the engine asks
    // for it on every renderer rebuild -- 86 times in one session, three
    // lines each. Said once.
    static bool announced_memory = false;
    if (!announced_memory) {
        announced_memory = true;
        std::printf("stud-render-host: memory: %u type(s), %u heap(s)\n", mem.memoryTypeCount,
                    mem.memoryHeapCount);
        std::fflush(stdout);
    }
    std::fflush(stdout);
    return write_pod(mem, out, out_len);
}

uint64_t vk_get_physical_device_queue_family_properties(uint64_t device, uint32_t capacity,
                                                         std::vector<uint8_t>& out,
                                                         uint32_t* out_len) {
    Loader& l = loader();
    if (l.get_physical_device_queue_family_properties == nullptr) {
        return static_cast<uint64_t>(static_cast<int32_t>(VK_ERROR_INITIALIZATION_FAILED));
    }
    VkPhysicalDevice pd = reinterpret_cast<VkPhysicalDevice>(static_cast<uintptr_t>(device));
    uint32_t count = 0;
    l.get_physical_device_queue_family_properties(pd, &count, nullptr);

    std::vector<VkQueueFamilyProperties> props;
    uint32_t returned = 0;
    if (capacity > 0 && count > 0) {
        returned = count < capacity ? count : capacity;
        props.resize(returned);
        l.get_physical_device_queue_family_properties(pd, &returned, props.data());
    }

    const uint32_t reported = capacity > 0 ? returned : count;
    const uint32_t stride = static_cast<uint32_t>(sizeof(VkQueueFamilyProperties));
    out.resize(sizeof(uint32_t) * 2 + stride * returned);
    std::memcpy(out.data(), &reported, sizeof(reported));
    std::memcpy(out.data() + sizeof(uint32_t), &stride, sizeof(stride));
    for (uint32_t i = 0; i < returned; ++i) {
        std::memcpy(out.data() + sizeof(uint32_t) * 2 + i * stride, &props[i], stride);
    }
    *out_len = static_cast<uint32_t>(out.size());
    return static_cast<uint64_t>(static_cast<int32_t>(VK_SUCCESS));
}

uint64_t vk_enumerate_device_extension_properties(uint64_t device, const std::vector<uint8_t>& in,
                                                   uint32_t capacity, std::vector<uint8_t>& out,
                                                   uint32_t* out_len) {
    Loader& l = loader();
    if (l.enumerate_device_extension_properties == nullptr) {
        return static_cast<uint64_t>(static_cast<int32_t>(VK_ERROR_INITIALIZATION_FAILED));
    }
    VkPhysicalDevice pd = reinterpret_cast<VkPhysicalDevice>(static_cast<uintptr_t>(device));
    std::string layer;
    if (!in.empty()) layer.assign(reinterpret_cast<const char*>(in.data()));
    const char* layer_ptr = layer.empty() ? nullptr : layer.c_str();

    uint32_t count = 0;
    VkResult r = l.enumerate_device_extension_properties(pd, layer_ptr, &count, nullptr);
    if (r != VK_SUCCESS && r != VK_INCOMPLETE) {
        return static_cast<uint64_t>(static_cast<int32_t>(r));
    }

    std::vector<VkExtensionProperties> props;
    uint32_t returned = 0;
    if (capacity > 0 && count > 0) {
        returned = count < capacity ? count : capacity;
        props.resize(returned);
        r = l.enumerate_device_extension_properties(pd, layer_ptr, &returned, props.data());
    }

    const uint32_t reported = capacity > 0 ? returned : count;
    out.resize(sizeof(uint32_t) + sizeof(vk_wire::ExtensionProperties) * returned);
    std::memcpy(out.data(), &reported, sizeof(reported));
    for (uint32_t i = 0; i < returned; ++i) {
        vk_wire::ExtensionProperties w{};
        std::snprintf(w.extension_name, sizeof(w.extension_name), "%s", props[i].extensionName);
        w.spec_version = props[i].specVersion;
        std::memcpy(out.data() + sizeof(uint32_t) + i * sizeof(w), &w, sizeof(w));
    }
    *out_len = static_cast<uint32_t>(out.size());
    return static_cast<uint64_t>(static_cast<int32_t>(r));
}

namespace {

// Rebuilds a pNext chain from the flattened wire form into locally
// allocated nodes, and links them. The storage outlives the call because
// the caller keeps `storage` alive.
void* rebuild_chain(const uint8_t* p, size_t len, uint32_t node_count,
                    std::vector<std::vector<uint8_t>>& storage) {
    void* head = nullptr;
    void* tail = nullptr;
    size_t off = 0;
    for (uint32_t i = 0; i < node_count; ++i) {
        if (off + sizeof(vk_wire::ChainNodeHeader) > len) break;
        vk_wire::ChainNodeHeader h{};
        std::memcpy(&h, p + off, sizeof(h));
        off += sizeof(h);
        if (h.size == 0 || off + h.size > len) break;

        const uint32_t local = vk_wire::chain_node_size(h.s_type);
        if (local == 0 || local != h.size) {
            // Unknown here, or the two builds disagree about its length.
            // Dropping the node is the safe degradation: the driver
            // simply does not see that extension struct.
            std::printf("stud-render-host: dropping pNext node sType=%u (host size %u, wire %u)\n",
                        h.s_type, local, h.size);
            std::fflush(stdout);
            off += h.size;
            continue;
        }
        storage.emplace_back(p + off, p + off + h.size);
        off += h.size;

        auto* node = reinterpret_cast<VkBaseOutStructure*>(storage.back().data());
        node->pNext = nullptr;
        if (head == nullptr) {
            head = node;
        } else {
            reinterpret_cast<VkBaseOutStructure*>(tail)->pNext = node;
        }
        tail = node;
    }
    return head;
}

// Flattens a chain back out, in the same order, for the reply.
void flatten_chain(const void* head, std::vector<uint8_t>& out) {
    for (auto* n = static_cast<const VkBaseInStructure*>(head); n != nullptr;
         n = n->pNext) {
        const uint32_t size = vk_wire::chain_node_size(static_cast<uint32_t>(n->sType));
        if (size == 0) continue;
        vk_wire::ChainNodeHeader h{static_cast<uint32_t>(n->sType), size};
        const size_t at = out.size();
        out.resize(at + sizeof(h) + size);
        std::memcpy(out.data() + at, &h, sizeof(h));
        std::memcpy(out.data() + at + sizeof(h), n, size);
    }
}

}  // namespace

uint64_t vk_get_physical_device_features2(uint64_t device, const std::vector<uint8_t>& in,
                                           uint32_t node_count, std::vector<uint8_t>& out,
                                           uint32_t* out_len) {
    Loader& l = loader();
    if (l.get_physical_device_features2 == nullptr) {
        return static_cast<uint64_t>(static_cast<int32_t>(VK_ERROR_INITIALIZATION_FAILED));
    }
    std::vector<std::vector<uint8_t>> storage;
    void* chain = rebuild_chain(in.data(), in.size(), node_count, storage);

    // The engine's own chain always starts with VkPhysicalDeviceFeatures2
    // itself; anything after it is an extension query hanging off it.
    VkPhysicalDeviceFeatures2 features{};
    features.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2;
    features.pNext = chain;
    l.get_physical_device_features2(
        reinterpret_cast<VkPhysicalDevice>(static_cast<uintptr_t>(device)), &features);

    // Reply: the base struct, then every node the driver filled in.
    const uint32_t base = static_cast<uint32_t>(sizeof(VkPhysicalDeviceFeatures));
    out.resize(sizeof(uint32_t) + base);
    std::memcpy(out.data(), &base, sizeof(base));
    std::memcpy(out.data() + sizeof(uint32_t), &features.features, base);
    flatten_chain(chain, out);
    *out_len = static_cast<uint32_t>(out.size());
    return static_cast<uint64_t>(static_cast<int32_t>(VK_SUCCESS));
}

// Whether the real driver offers a device extension. Asked before adding
// one the engine did not request, so a driver without it is simply left
// alone rather than failing device creation.
bool device_supports_extension(const char* name) {
    Loader& l = loader();
    if (l.physical_device == VK_NULL_HANDLE || l.enumerate_device_extension_properties == nullptr) {
        return false;
    }
    uint32_t n = 0;
    l.enumerate_device_extension_properties(l.physical_device, nullptr, &n, nullptr);
    std::vector<VkExtensionProperties> props(n);
    if (n > 0) {
        l.enumerate_device_extension_properties(l.physical_device, nullptr, &n, props.data());
    }
    for (const auto& p : props) {
        if (std::strcmp(p.extensionName, name) == 0) return true;
    }
    return false;
}

uint64_t vk_create_device(uint64_t physical_device, const std::vector<uint8_t>& in,
                           std::vector<uint8_t>& out, uint32_t* out_len) {
    Loader& l = loader();
    if (l.create_device == nullptr) {
        return static_cast<uint64_t>(static_cast<int32_t>(VK_ERROR_INITIALIZATION_FAILED));
    }
    if (in.size() < sizeof(vk_wire::CreateDeviceHeader)) {
        return static_cast<uint64_t>(static_cast<int32_t>(VK_ERROR_INITIALIZATION_FAILED));
    }
    vk_wire::CreateDeviceHeader hdr{};
    std::memcpy(&hdr, in.data(), sizeof(hdr));
    size_t off = sizeof(hdr);

    std::vector<VkDeviceQueueCreateInfo> queues;
    std::vector<std::vector<float>> priorities;
    priorities.reserve(hdr.queue_create_count);
    for (uint32_t i = 0; i < hdr.queue_create_count; ++i) {
        if (off + sizeof(vk_wire::DeviceQueueCreateHeader) > in.size()) break;
        vk_wire::DeviceQueueCreateHeader q{};
        std::memcpy(&q, in.data() + off, sizeof(q));
        off += sizeof(q);
        const size_t bytes = sizeof(float) * q.queue_count;
        if (off + bytes > in.size()) break;
        std::vector<float> pr(q.queue_count);
        if (q.queue_count > 0) std::memcpy(pr.data(), in.data() + off, bytes);
        off += bytes;
        priorities.push_back(std::move(pr));

        VkDeviceQueueCreateInfo ci{};
        ci.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
        ci.flags = q.flags;
        ci.queueFamilyIndex = q.queue_family_index;
        ci.queueCount = q.queue_count;
        queues.push_back(ci);
    }
    for (size_t i = 0; i < queues.size(); ++i) {
        queues[i].pQueuePriorities = priorities[i].empty() ? nullptr : priorities[i].data();
    }

    auto take_cstr = [&in, &off]() -> std::string {
        if (off >= in.size()) return {};
        std::string s(reinterpret_cast<const char*>(in.data() + off));
        off += s.size() + 1;
        return s;
    };
    std::vector<std::string> layers;
    for (uint32_t i = 0; i < hdr.enabled_layer_count; ++i) layers.push_back(take_cstr());
    std::vector<std::string> extensions;
    for (uint32_t i = 0; i < hdr.enabled_extension_count; ++i) extensions.push_back(take_cstr());

    VkPhysicalDeviceFeatures enabled_features{};
    if (hdr.has_enabled_features != 0 && hdr.enabled_features_size == sizeof(enabled_features) &&
        off + hdr.enabled_features_size <= in.size()) {
        std::memcpy(&enabled_features, in.data() + off, sizeof(enabled_features));
        off += hdr.enabled_features_size;
    }

    std::vector<std::vector<uint8_t>> storage;
    void* chain = rebuild_chain(in.data() + off, in.size() - off, hdr.chain_node_count, storage);

    std::vector<const char*> layer_ptrs;
    for (const std::string& s : layers) layer_ptrs.push_back(s.c_str());
    // VK_EXT_external_memory_host, whether or not the engine asked for it.
    //
    // It is what lets host-visible allocations be shared outright with the
    // process that writes them: Stud maps a file in the runtime directory,
    // hands the engine that pointer, and imports the same pages here as
    // real device memory. Without it every byte the engine writes has to
    // be copied across the socket -- measured at 197 MB/s in a real game,
    // which is most of what the render thread was doing.
    if (device_supports_extension(VK_EXT_EXTERNAL_MEMORY_HOST_EXTENSION_NAME)) {
        bool already = false;
        for (const std::string& e : extensions) {
            if (e == VK_EXT_EXTERNAL_MEMORY_HOST_EXTENSION_NAME) already = true;
        }
        if (!already) extensions.push_back(VK_EXT_EXTERNAL_MEMORY_HOST_EXTENSION_NAME);
    }

    std::vector<const char*> ext_ptrs;
    for (const std::string& s : extensions) ext_ptrs.push_back(s.c_str());

    VkDeviceCreateInfo ci{};
    ci.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
    ci.pNext = chain;
    ci.flags = hdr.flags;
    ci.queueCreateInfoCount = static_cast<uint32_t>(queues.size());
    ci.pQueueCreateInfos = queues.empty() ? nullptr : queues.data();
    ci.enabledLayerCount = static_cast<uint32_t>(layer_ptrs.size());
    ci.ppEnabledLayerNames = layer_ptrs.empty() ? nullptr : layer_ptrs.data();
    ci.enabledExtensionCount = static_cast<uint32_t>(ext_ptrs.size());
    ci.ppEnabledExtensionNames = ext_ptrs.empty() ? nullptr : ext_ptrs.data();
    ci.pEnabledFeatures = hdr.has_enabled_features != 0 ? &enabled_features : nullptr;

    VkDevice device = VK_NULL_HANDLE;
    l.physical_device =
        reinterpret_cast<VkPhysicalDevice>(static_cast<uintptr_t>(physical_device));
    VkResult r = l.create_device(l.physical_device, &ci, nullptr, &device);
    std::printf("stud-render-host: vkCreateDevice -> %d (queues=%zu extensions=%zu chain=%u)\n",
                static_cast<int>(r), queues.size(), ext_ptrs.size(), hdr.chain_node_count);
    std::fflush(stdout);
    if (r != VK_SUCCESS) return static_cast<uint64_t>(static_cast<int32_t>(r));

    // Resolve every device-level command from the device itself, now that
    // one exists. The spec requires this -- vkGetDeviceProcAddr reaches
    // the driver's real implementations, where vkGetInstanceProcAddr
    // would only give loader trampolines.
    //
    // Live-caught when this block was missing: every device call returned
    // the null-pointer path (VK_ERROR_INITIALIZATION_FAILED), and the
    // engine logged a wall of "VULKAN ERROR: vkCreateSemaphore ...
    // returned -3" before giving up with "Failed to allocate image
    // memory".
    l.device = device;
    if (l.get_device_proc_addr != nullptr) {
        auto dev = [&l](const char* n) { return l.get_device_proc_addr(l.device, n); };
        l.get_device_queue = reinterpret_cast<PFN_vkGetDeviceQueue>(dev("vkGetDeviceQueue"));
        l.create_command_pool =
            reinterpret_cast<PFN_vkCreateCommandPool>(dev("vkCreateCommandPool"));
        l.create_semaphore = reinterpret_cast<PFN_vkCreateSemaphore>(dev("vkCreateSemaphore"));
        l.create_fence = reinterpret_cast<PFN_vkCreateFence>(dev("vkCreateFence"));
        l.create_query_pool = reinterpret_cast<PFN_vkCreateQueryPool>(dev("vkCreateQueryPool"));
        l.create_pipeline_cache =
            reinterpret_cast<PFN_vkCreatePipelineCache>(dev("vkCreatePipelineCache"));
        l.get_pipeline_cache_data =
            reinterpret_cast<PFN_vkGetPipelineCacheData>(dev("vkGetPipelineCacheData"));
        l.destroy_pipeline_cache =
            reinterpret_cast<PFN_vkDestroyPipelineCache>(dev("vkDestroyPipelineCache"));
        l.destroy_sampler = reinterpret_cast<PFN_vkDestroySampler>(dev("vkDestroySampler"));
        l.destroy_render_pass =
            reinterpret_cast<PFN_vkDestroyRenderPass>(dev("vkDestroyRenderPass"));
        l.destroy_pipeline = reinterpret_cast<PFN_vkDestroyPipeline>(dev("vkDestroyPipeline"));
        l.destroy_pipeline_layout =
            reinterpret_cast<PFN_vkDestroyPipelineLayout>(dev("vkDestroyPipelineLayout"));
        l.destroy_descriptor_set_layout = reinterpret_cast<PFN_vkDestroyDescriptorSetLayout>(
            dev("vkDestroyDescriptorSetLayout"));
        l.destroy_descriptor_pool =
            reinterpret_cast<PFN_vkDestroyDescriptorPool>(dev("vkDestroyDescriptorPool"));
        l.destroy_descriptor_update_template =
            reinterpret_cast<PFN_vkDestroyDescriptorUpdateTemplate>(
                dev("vkDestroyDescriptorUpdateTemplate"));
        l.create_image = reinterpret_cast<PFN_vkCreateImage>(dev("vkCreateImage"));
        l.get_image_memory_requirements =
            reinterpret_cast<PFN_vkGetImageMemoryRequirements>(dev("vkGetImageMemoryRequirements"));
        l.allocate_memory = reinterpret_cast<PFN_vkAllocateMemory>(dev("vkAllocateMemory"));
        l.bind_image_memory = reinterpret_cast<PFN_vkBindImageMemory>(dev("vkBindImageMemory"));
        l.free_memory = reinterpret_cast<PFN_vkFreeMemory>(dev("vkFreeMemory"));
        l.map_memory = reinterpret_cast<PFN_vkMapMemory>(dev("vkMapMemory"));
        l.unmap_memory = reinterpret_cast<PFN_vkUnmapMemory>(dev("vkUnmapMemory"));
        l.flush_mapped_memory_ranges =
            reinterpret_cast<PFN_vkFlushMappedMemoryRanges>(dev("vkFlushMappedMemoryRanges"));
        l.device_wait_idle = reinterpret_cast<PFN_vkDeviceWaitIdle>(dev("vkDeviceWaitIdle"));
        l.create_swapchain =
            reinterpret_cast<PFN_vkCreateSwapchainKHR>(dev("vkCreateSwapchainKHR"));
        l.get_swapchain_images =
            reinterpret_cast<PFN_vkGetSwapchainImagesKHR>(dev("vkGetSwapchainImagesKHR"));
        l.create_buffer = reinterpret_cast<PFN_vkCreateBuffer>(dev("vkCreateBuffer"));
        l.get_buffer_memory_requirements = reinterpret_cast<PFN_vkGetBufferMemoryRequirements>(
            dev("vkGetBufferMemoryRequirements"));
        l.bind_buffer_memory = reinterpret_cast<PFN_vkBindBufferMemory>(dev("vkBindBufferMemory"));
        l.create_image_view = reinterpret_cast<PFN_vkCreateImageView>(dev("vkCreateImageView"));
        l.create_shader_module =
            reinterpret_cast<PFN_vkCreateShaderModule>(dev("vkCreateShaderModule"));
        l.destroy_buffer = reinterpret_cast<PFN_vkDestroyBuffer>(dev("vkDestroyBuffer"));
        l.destroy_image = reinterpret_cast<PFN_vkDestroyImage>(dev("vkDestroyImage"));
        l.destroy_image_view = reinterpret_cast<PFN_vkDestroyImageView>(dev("vkDestroyImageView"));
        l.destroy_shader_module =
            reinterpret_cast<PFN_vkDestroyShaderModule>(dev("vkDestroyShaderModule"));
        l.destroy_semaphore = reinterpret_cast<PFN_vkDestroySemaphore>(dev("vkDestroySemaphore"));
        l.destroy_fence = reinterpret_cast<PFN_vkDestroyFence>(dev("vkDestroyFence"));
        l.destroy_command_pool =
            reinterpret_cast<PFN_vkDestroyCommandPool>(dev("vkDestroyCommandPool"));
        l.destroy_query_pool = reinterpret_cast<PFN_vkDestroyQueryPool>(dev("vkDestroyQueryPool"));
        l.destroy_swapchain =
            reinterpret_cast<PFN_vkDestroySwapchainKHR>(dev("vkDestroySwapchainKHR"));
        l.destroy_device = reinterpret_cast<PFN_vkDestroyDevice>(dev("vkDestroyDevice"));
        l.create_render_pass = reinterpret_cast<PFN_vkCreateRenderPass>(dev("vkCreateRenderPass"));
        l.create_framebuffer = reinterpret_cast<PFN_vkCreateFramebuffer>(dev("vkCreateFramebuffer"));
        l.destroy_framebuffer =
            reinterpret_cast<PFN_vkDestroyFramebuffer>(dev("vkDestroyFramebuffer"));
        l.create_sampler = reinterpret_cast<PFN_vkCreateSampler>(dev("vkCreateSampler"));
        l.create_pipeline_layout =
            reinterpret_cast<PFN_vkCreatePipelineLayout>(dev("vkCreatePipelineLayout"));
        l.create_descriptor_set_layout = reinterpret_cast<PFN_vkCreateDescriptorSetLayout>(
            dev("vkCreateDescriptorSetLayout"));
        l.create_descriptor_pool =
            reinterpret_cast<PFN_vkCreateDescriptorPool>(dev("vkCreateDescriptorPool"));
        l.allocate_descriptor_sets =
            reinterpret_cast<PFN_vkAllocateDescriptorSets>(dev("vkAllocateDescriptorSets"));
        l.get_memory_host_pointer_properties =
            reinterpret_cast<PFN_vkGetMemoryHostPointerPropertiesEXT>(
                dev("vkGetMemoryHostPointerPropertiesEXT"));
        l.reset_descriptor_pool =
            reinterpret_cast<PFN_vkResetDescriptorPool>(dev("vkResetDescriptorPool"));
        l.create_descriptor_update_template =
            reinterpret_cast<PFN_vkCreateDescriptorUpdateTemplate>(
                dev("vkCreateDescriptorUpdateTemplate"));
        if (l.create_descriptor_update_template == nullptr) {
            l.create_descriptor_update_template =
                reinterpret_cast<PFN_vkCreateDescriptorUpdateTemplate>(
                    dev("vkCreateDescriptorUpdateTemplateKHR"));
        }
        l.update_descriptor_set_with_template =
            reinterpret_cast<PFN_vkUpdateDescriptorSetWithTemplate>(
                dev("vkUpdateDescriptorSetWithTemplate"));
        if (l.update_descriptor_set_with_template == nullptr) {
            l.update_descriptor_set_with_template =
                reinterpret_cast<PFN_vkUpdateDescriptorSetWithTemplate>(
                    dev("vkUpdateDescriptorSetWithTemplateKHR"));
        }
        l.create_graphics_pipelines =
            reinterpret_cast<PFN_vkCreateGraphicsPipelines>(dev("vkCreateGraphicsPipelines"));
        l.create_compute_pipelines =
            reinterpret_cast<PFN_vkCreateComputePipelines>(dev("vkCreateComputePipelines"));
        l.allocate_command_buffers =
            reinterpret_cast<PFN_vkAllocateCommandBuffers>(dev("vkAllocateCommandBuffers"));
        l.begin_command_buffer =
            reinterpret_cast<PFN_vkBeginCommandBuffer>(dev("vkBeginCommandBuffer"));
        l.end_command_buffer = reinterpret_cast<PFN_vkEndCommandBuffer>(dev("vkEndCommandBuffer"));
        l.reset_command_pool = reinterpret_cast<PFN_vkResetCommandPool>(dev("vkResetCommandPool"));
        l.queue_submit = reinterpret_cast<PFN_vkQueueSubmit>(dev("vkQueueSubmit"));
        l.wait_for_fences = reinterpret_cast<PFN_vkWaitForFences>(dev("vkWaitForFences"));
        l.reset_fences = reinterpret_cast<PFN_vkResetFences>(dev("vkResetFences"));
        l.acquire_next_image =
            reinterpret_cast<PFN_vkAcquireNextImageKHR>(dev("vkAcquireNextImageKHR"));
        l.queue_present = reinterpret_cast<PFN_vkQueuePresentKHR>(dev("vkQueuePresentKHR"));
        l.get_query_pool_results =
            reinterpret_cast<PFN_vkGetQueryPoolResults>(dev("vkGetQueryPoolResults"));
        l.cmd_begin_render_pass =
            reinterpret_cast<PFN_vkCmdBeginRenderPass>(dev("vkCmdBeginRenderPass"));
        l.cmd_end_render_pass =
            reinterpret_cast<PFN_vkCmdEndRenderPass>(dev("vkCmdEndRenderPass"));
        l.cmd_bind_pipeline = reinterpret_cast<PFN_vkCmdBindPipeline>(dev("vkCmdBindPipeline"));
        l.cmd_bind_descriptor_sets =
            reinterpret_cast<PFN_vkCmdBindDescriptorSets>(dev("vkCmdBindDescriptorSets"));
        l.cmd_bind_vertex_buffers =
            reinterpret_cast<PFN_vkCmdBindVertexBuffers>(dev("vkCmdBindVertexBuffers"));
        l.cmd_bind_index_buffer =
            reinterpret_cast<PFN_vkCmdBindIndexBuffer>(dev("vkCmdBindIndexBuffer"));
        l.cmd_draw = reinterpret_cast<PFN_vkCmdDraw>(dev("vkCmdDraw"));
        l.cmd_draw_indexed = reinterpret_cast<PFN_vkCmdDrawIndexed>(dev("vkCmdDrawIndexed"));
        l.cmd_dispatch = reinterpret_cast<PFN_vkCmdDispatch>(dev("vkCmdDispatch"));
        l.cmd_set_viewport = reinterpret_cast<PFN_vkCmdSetViewport>(dev("vkCmdSetViewport"));
        l.cmd_set_scissor = reinterpret_cast<PFN_vkCmdSetScissor>(dev("vkCmdSetScissor"));
        l.cmd_pipeline_barrier =
            reinterpret_cast<PFN_vkCmdPipelineBarrier>(dev("vkCmdPipelineBarrier"));
        l.cmd_copy_buffer = reinterpret_cast<PFN_vkCmdCopyBuffer>(dev("vkCmdCopyBuffer"));
        l.cmd_copy_buffer_to_image =
            reinterpret_cast<PFN_vkCmdCopyBufferToImage>(dev("vkCmdCopyBufferToImage"));
        l.cmd_copy_image_to_buffer =
            reinterpret_cast<PFN_vkCmdCopyImageToBuffer>(dev("vkCmdCopyImageToBuffer"));
        l.cmd_copy_image = reinterpret_cast<PFN_vkCmdCopyImage>(dev("vkCmdCopyImage"));
        l.cmd_blit_image = reinterpret_cast<PFN_vkCmdBlitImage>(dev("vkCmdBlitImage"));
        l.cmd_resolve_image = reinterpret_cast<PFN_vkCmdResolveImage>(dev("vkCmdResolveImage"));
        l.cmd_reset_query_pool =
            reinterpret_cast<PFN_vkCmdResetQueryPool>(dev("vkCmdResetQueryPool"));
        l.cmd_write_timestamp =
            reinterpret_cast<PFN_vkCmdWriteTimestamp>(dev("vkCmdWriteTimestamp"));
        l.queue_wait_idle = reinterpret_cast<PFN_vkQueueWaitIdle>(dev("vkQueueWaitIdle"));
        std::printf("stud-render-host: device commands resolved (queue=%d image=%d swapchain=%d "
                    "buffer=%d renderpass=%d framebuffer=%d gfxpipe=%d cmdbuf=%d)\n",
                    l.get_device_queue != nullptr, l.create_image != nullptr,
                    l.create_swapchain != nullptr, l.create_buffer != nullptr,
                    l.create_render_pass != nullptr, l.create_framebuffer != nullptr,
                    l.create_graphics_pipelines != nullptr,
                    l.allocate_command_buffers != nullptr);
        std::fflush(stdout);
    }

    uint64_t handle = static_cast<uint64_t>(reinterpret_cast<uintptr_t>(device));
    out.resize(sizeof(handle));
    std::memcpy(out.data(), &handle, sizeof(handle));
    *out_len = sizeof(handle);
    return static_cast<uint64_t>(static_cast<int32_t>(VK_SUCCESS));
}

uint64_t vk_get_physical_device_format_properties(uint64_t device, uint32_t format,
                                                   std::vector<uint8_t>& out, uint32_t* out_len) {
    Loader& l = loader();
    if (l.get_physical_device_format_properties == nullptr) {
        return static_cast<uint64_t>(static_cast<int32_t>(VK_ERROR_INITIALIZATION_FAILED));
    }
    VkFormatProperties props{};
    l.get_physical_device_format_properties(
        reinterpret_cast<VkPhysicalDevice>(static_cast<uintptr_t>(device)),
        static_cast<VkFormat>(format), &props);
    return write_pod(props, out, out_len);
}

uint64_t vk_get_physical_device_image_format_properties(uint64_t device, uint32_t format,
                                                         uint32_t type, uint32_t tiling,
                                                         uint32_t usage, uint32_t flags,
                                                         std::vector<uint8_t>& out,
                                                         uint32_t* out_len) {
    Loader& l = loader();
    if (l.get_physical_device_image_format_properties == nullptr) {
        return static_cast<uint64_t>(static_cast<int32_t>(VK_ERROR_INITIALIZATION_FAILED));
    }
    VkImageFormatProperties props{};
    VkResult r = l.get_physical_device_image_format_properties(
        reinterpret_cast<VkPhysicalDevice>(static_cast<uintptr_t>(device)),
        static_cast<VkFormat>(format), static_cast<VkImageType>(type),
        static_cast<VkImageTiling>(tiling), usage, flags, &props);
    // A format the driver does not support is a normal answer, not a
    // transport failure: report it with the struct still written.
    write_pod(props, out, out_len);
    return static_cast<uint64_t>(static_cast<int32_t>(r));
}

namespace {

// Vulkan handles are pointer-typed on a 64-bit build but plain uint64 on
// others, so conversion has to work either way rather than assuming.
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

// Every simple create returns a 64-bit handle the same way.
uint64_t write_handle(uint64_t handle, std::vector<uint8_t>& out, uint32_t* out_len) {
    out.resize(sizeof(handle));
    std::memcpy(out.data(), &handle, sizeof(handle));
    *out_len = sizeof(handle);
    return static_cast<uint64_t>(static_cast<int32_t>(VK_SUCCESS));
}

}  // namespace

uint64_t vk_get_device_queue(uint32_t family, uint32_t index, std::vector<uint8_t>& out,
                              uint32_t* out_len) {
    Loader& l = loader();
    if (l.get_device_queue == nullptr) {
        return static_cast<uint64_t>(static_cast<int32_t>(VK_ERROR_INITIALIZATION_FAILED));
    }
    VkQueue queue = VK_NULL_HANDLE;
    l.get_device_queue(l.device, family, index, &queue);
    // The pixel probe needs a queue and a pool of its own; the first
    // queue the engine asks for is as good as any, and it is only used
    // when STUD_VK_PROBE_PIXELS is set.
    if (l.probe_queue == VK_NULL_HANDLE) {
        l.probe_queue = queue;
        if (l.create_command_pool != nullptr) {
            VkCommandPoolCreateInfo pci{};
            pci.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
            pci.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
            pci.queueFamilyIndex = family;
            l.create_command_pool(l.device, &pci, nullptr, &l.probe_pool);
        }
    }
    return write_handle(to_u64(queue), out, out_len);
}

uint64_t vk_create_command_pool(uint32_t flags, uint32_t family, std::vector<uint8_t>& out,
                                 uint32_t* out_len) {
    Loader& l = loader();
    if (l.create_command_pool == nullptr) {
        return static_cast<uint64_t>(static_cast<int32_t>(VK_ERROR_INITIALIZATION_FAILED));
    }
    VkCommandPoolCreateInfo ci{};
    ci.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
    ci.flags = flags;
    ci.queueFamilyIndex = family;
    VkCommandPool pool = VK_NULL_HANDLE;
    VkResult r = l.create_command_pool(l.device, &ci, nullptr, &pool);
    if (r != VK_SUCCESS) return static_cast<uint64_t>(static_cast<int32_t>(r));
    return write_handle(to_u64(pool), out, out_len);
}

uint64_t vk_create_semaphore(uint32_t flags, std::vector<uint8_t>& out, uint32_t* out_len) {
    Loader& l = loader();
    if (l.create_semaphore == nullptr) {
        return static_cast<uint64_t>(static_cast<int32_t>(VK_ERROR_INITIALIZATION_FAILED));
    }
    VkSemaphoreCreateInfo ci{};
    ci.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;
    ci.flags = flags;
    VkSemaphore sem = VK_NULL_HANDLE;
    VkResult r = l.create_semaphore(l.device, &ci, nullptr, &sem);
    if (r != VK_SUCCESS) return static_cast<uint64_t>(static_cast<int32_t>(r));
    return write_handle(to_u64(sem), out, out_len);
}

uint64_t vk_create_fence(uint32_t flags, std::vector<uint8_t>& out, uint32_t* out_len) {
    Loader& l = loader();
    if (l.create_fence == nullptr) {
        return static_cast<uint64_t>(static_cast<int32_t>(VK_ERROR_INITIALIZATION_FAILED));
    }
    VkFenceCreateInfo ci{};
    ci.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
    ci.flags = flags;
    VkFence fence = VK_NULL_HANDLE;
    VkResult r = l.create_fence(l.device, &ci, nullptr, &fence);
    if (r != VK_SUCCESS) return static_cast<uint64_t>(static_cast<int32_t>(r));
    return write_handle(to_u64(fence), out, out_len);
}

uint64_t vk_create_query_pool(uint32_t flags, uint32_t query_type, uint32_t query_count,
                               uint32_t pipeline_statistics, std::vector<uint8_t>& out,
                               uint32_t* out_len) {
    Loader& l = loader();
    if (l.create_query_pool == nullptr) {
        return static_cast<uint64_t>(static_cast<int32_t>(VK_ERROR_INITIALIZATION_FAILED));
    }
    VkQueryPoolCreateInfo ci{};
    ci.sType = VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO;
    ci.flags = flags;
    ci.queryType = static_cast<VkQueryType>(query_type);
    ci.queryCount = query_count;
    ci.pipelineStatistics = pipeline_statistics;
    VkQueryPool pool = VK_NULL_HANDLE;
    VkResult r = l.create_query_pool(l.device, &ci, nullptr, &pool);
    if (r != VK_SUCCESS) return static_cast<uint64_t>(static_cast<int32_t>(r));
    return write_handle(to_u64(pool), out, out_len);
}

uint64_t vk_create_pipeline_cache(uint32_t flags, const std::vector<uint8_t>& in,
                                   std::vector<uint8_t>& out, uint32_t* out_len) {
    Loader& l = loader();
    if (l.create_pipeline_cache == nullptr) {
        return static_cast<uint64_t>(static_cast<int32_t>(VK_ERROR_INITIALIZATION_FAILED));
    }
    VkPipelineCacheCreateInfo ci{};
    ci.sType = VK_STRUCTURE_TYPE_PIPELINE_CACHE_CREATE_INFO;
    ci.flags = flags;
    ci.initialDataSize = in.size();
    ci.pInitialData = in.empty() ? nullptr : in.data();
    VkPipelineCache cache = VK_NULL_HANDLE;
    VkResult r = l.create_pipeline_cache(l.device, &ci, nullptr, &cache);
    if (r != VK_SUCCESS) return static_cast<uint64_t>(static_cast<int32_t>(r));
    return write_handle(to_u64(cache), out, out_len);
}

uint64_t vk_get_pipeline_cache_data(uint64_t cache, uint32_t capacity, std::vector<uint8_t>& out,
                                     uint32_t* out_len) {
    Loader& l = loader();
    if (l.get_pipeline_cache_data == nullptr) {
        return static_cast<uint64_t>(static_cast<int32_t>(VK_ERROR_INITIALIZATION_FAILED));
    }
    // Real two-call idiom: capacity 0 means "how big is it".
    size_t size = 0;
    VkResult r =
        l.get_pipeline_cache_data(l.device, from_u64<VkPipelineCache>(cache), &size, nullptr);
    if (r != VK_SUCCESS && r != VK_INCOMPLETE) {
        return static_cast<uint64_t>(static_cast<int32_t>(r));
    }
    uint64_t reported = size;
    if (capacity == 0) {
        out.resize(sizeof(reported));
        std::memcpy(out.data(), &reported, sizeof(reported));
        *out_len = sizeof(reported);
        return static_cast<uint64_t>(static_cast<int32_t>(r));
    }
    size_t want = capacity < size ? capacity : size;
    std::vector<uint8_t> data(want);
    r = l.get_pipeline_cache_data(l.device, from_u64<VkPipelineCache>(cache), &want,
                                   data.data());
    reported = want;
    out.resize(sizeof(reported) + want);
    std::memcpy(out.data(), &reported, sizeof(reported));
    if (want > 0) std::memcpy(out.data() + sizeof(reported), data.data(), want);
    *out_len = static_cast<uint32_t>(out.size());
    return static_cast<uint64_t>(static_cast<int32_t>(r));
}

uint64_t vk_destroy_pipeline_cache(uint64_t cache) {
    Loader& l = loader();
    if (l.destroy_pipeline_cache == nullptr) return 0;
    l.destroy_pipeline_cache(l.device, from_u64<VkPipelineCache>(cache), nullptr);
    return 0;
}
// Per-object create/query chatter. Off by default: it is useful while
// chasing one specific allocation and pure volume otherwise.
bool vk_object_trace_enabled() {
    static const bool enabled = std::getenv("STUD_VK_TRACE_OBJECTS") != nullptr;
    return enabled;
}


uint64_t vk_create_image(const std::vector<uint8_t>& in, std::vector<uint8_t>& out,
                          uint32_t* out_len) {
    Loader& l = loader();
    if (l.create_image == nullptr) {
        return static_cast<uint64_t>(static_cast<int32_t>(VK_ERROR_INITIALIZATION_FAILED));
    }
    if (in.size() < sizeof(vk_wire::CreateImageHeader)) {
        return static_cast<uint64_t>(static_cast<int32_t>(VK_ERROR_INITIALIZATION_FAILED));
    }
    vk_wire::CreateImageHeader h{};
    std::memcpy(&h, in.data(), sizeof(h));
    size_t off = sizeof(h);

    std::vector<uint32_t> families(h.queue_family_count);
    const size_t fam_bytes = sizeof(uint32_t) * h.queue_family_count;
    if (h.queue_family_count > 0 && off + fam_bytes <= in.size()) {
        std::memcpy(families.data(), in.data() + off, fam_bytes);
        off += fam_bytes;
    }
    std::vector<std::vector<uint8_t>> storage;
    void* chain = rebuild_chain(in.data() + off, in.size() - off, h.chain_node_count, storage);

    VkImageCreateInfo ci{};
    ci.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    ci.pNext = chain;
    ci.flags = h.flags;
    ci.imageType = static_cast<VkImageType>(h.image_type);
    ci.format = static_cast<VkFormat>(h.format);
    ci.extent = {h.extent_width, h.extent_height, h.extent_depth};
    ci.mipLevels = h.mip_levels;
    ci.arrayLayers = h.array_layers;
    ci.samples = static_cast<VkSampleCountFlagBits>(h.samples);
    ci.tiling = static_cast<VkImageTiling>(h.tiling);
    ci.usage = h.usage;
    ci.sharingMode = static_cast<VkSharingMode>(h.sharing_mode);
    ci.queueFamilyIndexCount = h.queue_family_count;
    ci.pQueueFamilyIndices = families.empty() ? nullptr : families.data();
    ci.initialLayout = static_cast<VkImageLayout>(h.initial_layout);

    VkImage image = VK_NULL_HANDLE;
    VkResult r = l.create_image(l.device, &ci, nullptr, &image);
    // Only a failure, unless someone asked for the running commentary.
    // A real game creates images constantly -- measured at 14k of these
    // in one session, next to 18k memory-requirement lines, which was
    // 84% of the whole log -- and "it worked" for the 14000th time says
    // nothing. The failure line is what mattered, and it was the one
    // buried.
    if (r != VK_SUCCESS || vk_object_trace_enabled()) {
        std::printf("stud-render-host: vkCreateImage -> %d (format=%u %ux%u usage=0x%x tiling=%u)\n",
                    static_cast<int>(r), h.format, h.extent_width, h.extent_height, h.usage,
                    h.tiling);
        std::fflush(stdout);
    }
    if (r != VK_SUCCESS) return static_cast<uint64_t>(static_cast<int32_t>(r));
    return write_handle(to_u64(image), out, out_len);
}

uint64_t vk_get_image_memory_requirements(uint64_t image, std::vector<uint8_t>& out,
                                           uint32_t* out_len) {
    Loader& l = loader();
    if (l.get_image_memory_requirements == nullptr) {
        return static_cast<uint64_t>(static_cast<int32_t>(VK_ERROR_INITIALIZATION_FAILED));
    }
    VkMemoryRequirements req{};
    l.get_image_memory_requirements(l.device, from_u64<VkImage>(image), &req);
    if (vk_object_trace_enabled()) {
        std::printf("stud-render-host: image memreq: size=%llu align=%llu bits=0x%x\n",
                    static_cast<unsigned long long>(req.size),
                    static_cast<unsigned long long>(req.alignment), req.memoryTypeBits);
        std::fflush(stdout);
    }
    return write_pod(req, out, out_len);
}

uint64_t vk_get_physical_device_surface_capabilities(uint64_t physical_device, uint64_t surface,
                                                      std::vector<uint8_t>& out,
                                                      uint32_t* out_len) {
    Loader& l = loader();
    if (l.get_physical_device_surface_capabilities == nullptr) {
        return static_cast<uint64_t>(static_cast<int32_t>(VK_ERROR_INITIALIZATION_FAILED));
    }
    VkSurfaceCapabilitiesKHR caps{};
    VkResult r = l.get_physical_device_surface_capabilities(
        reinterpret_cast<VkPhysicalDevice>(static_cast<uintptr_t>(physical_device)),
        from_u64<VkSurfaceKHR>(surface), &caps);

    // Wayland has no inherent surface size, so the driver reports
    // currentExtent = 0xFFFFFFFF x 0xFFFFFFFF -- the spec's "the
    // swapchain chooses" sentinel. On Android currentExtent is ALWAYS the
    // real window size, so an Android client trusts it without handling
    // the sentinel: live-caught as the engine logging
    //   "Vulkan: skipping framebuffer creation, invalid currentExtent -1x-1"
    // and then "Failed to allocate image memory".
    //
    // Process C owns the window, so it can answer the question the
    // sentinel defers. Reporting the real size here is what a real
    // Android surface would have reported, not a fabrication.
    constexpr uint32_t kUndefinedExtent = 0xFFFFFFFFu;
    if (caps.currentExtent.width == kUndefinedExtent ||
        caps.currentExtent.height == kUndefinedExtent) {
        // NEVER call ANativeWindow_fromSurface(nullptr, nullptr) here.
        // With a null Surface it only returns the existing window while
        // the window cache is non-empty -- and a null Surface never
        // populates that cache, so each call creates a brand new Wayland
        // window. Doing that on a query the engine repeats in a retry
        // loop spawned over a hundred real windows (live-caught, by the
        // user, on their own desktop). The window this process already
        // owns is handed in once at startup instead.
        const uint32_t w = g_window_width.load(std::memory_order_relaxed);
        const uint32_t h = g_window_height.load(std::memory_order_relaxed);
        if (w > 0 && h > 0) {
            caps.currentExtent = {w, h};
            // Keep the reported range consistent with the size just
            // given, or a client that clamps against it gets a different
            // answer than the one it was told to use.
            if (caps.minImageExtent.width > w) caps.minImageExtent.width = w;
            if (caps.minImageExtent.height > h) caps.minImageExtent.height = h;
            if (caps.maxImageExtent.width < w) caps.maxImageExtent.width = w;
            if (caps.maxImageExtent.height < h) caps.maxImageExtent.height = h;
            // Once per size, not once per query. The engine asks for
            // surface capabilities constantly -- measured at 5100 lines
            // of one 9700-line session log, so half the log was this
            // sentence -- and the answer only carries information when
            // it changes.
            static uint32_t announced_w = 0;
            static uint32_t announced_h = 0;
            if (w != announced_w || h != announced_h) {
                announced_w = w;
                announced_h = h;
                std::printf("stud-render-host: surface currentExtent was undefined (Wayland), "
                            "reporting the real window size %ux%u\n", w, h);
                std::fflush(stdout);
            }
        }
    }
    write_pod(caps, out, out_len);
    return static_cast<uint64_t>(static_cast<int32_t>(r));
}

// Whether the real driver resolves this command name, instance- or
// device-level. Stud answers the engine's own proc-address lookups with
// this so an unsupported extension reads as unsupported rather than as a
// working entry point that returns nonsense.
uint64_t vk_host_has_proc(const std::vector<uint8_t>& in) {
    Loader& l = loader();
    std::string name(reinterpret_cast<const char*>(in.data()), in.size());
    if (name.empty()) return 0;
    if (l.get_instance_proc_addr != nullptr && l.instance != VK_NULL_HANDLE &&
        l.get_instance_proc_addr(l.instance, name.c_str()) != nullptr) {
        return 1;
    }
    if (l.get_device_proc_addr != nullptr && l.device != VK_NULL_HANDLE &&
        l.get_device_proc_addr(l.device, name.c_str()) != nullptr) {
        return 1;
    }
    return 0;
}

uint64_t vk_device_wait_idle() {
    Loader& l = loader();
    if (l.device_wait_idle == nullptr) {
        return static_cast<uint64_t>(static_cast<int32_t>(VK_ERROR_INITIALIZATION_FAILED));
    }
    return static_cast<uint64_t>(static_cast<int32_t>(l.device_wait_idle(l.device)));
}

// A host-visible allocation the client also has mapped. Both processes map
// the same file, so what the engine writes is what the device reads.
struct SharedMapping {
    void* address = nullptr;
    size_t length = 0;
};
std::mutex& shared_memory_mutex() {
    static std::mutex m;
    return m;
}
// Which memory backs which buffer. Only read by the image-to-buffer
// readback, to answer one question: can the engine see what the GPU
// just wrote there?
std::mutex& buffer_memory_mutex() {
    static std::mutex m;
    return m;
}
std::unordered_map<uint64_t, uint64_t>& buffer_memory() {
    static std::unordered_map<uint64_t, uint64_t> m;
    return m;
}

std::unordered_map<uint64_t, SharedMapping>& shared_memory() {
    static std::unordered_map<uint64_t, SharedMapping> m;
    return m;
}

// The host-visible memory type an imported host pointer can back. Prefers
// the plainest one: coherent, uncached, so the engine's writes reach the
// device without anything having to be flushed by hand.
uint32_t importable_host_visible_type(uint32_t allowed_bits) {
    Loader& l = loader();
    if (l.physical_device == VK_NULL_HANDLE || l.get_physical_device_memory_properties == nullptr) {
        return UINT32_MAX;
    }
    VkPhysicalDeviceMemoryProperties mp{};
    l.get_physical_device_memory_properties(l.physical_device, &mp);
    uint32_t best = UINT32_MAX;
    for (uint32_t i = 0; i < mp.memoryTypeCount; ++i) {
        if ((allowed_bits & (1u << i)) == 0) continue;
        const VkMemoryPropertyFlags f = mp.memoryTypes[i].propertyFlags;
        if ((f & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT) == 0) continue;
        if ((f & VK_MEMORY_PROPERTY_HOST_COHERENT_BIT) == 0) continue;
        if (best == UINT32_MAX) best = i;
        // Uncached is the better default for memory the CPU only writes.
        if ((f & VK_MEMORY_PROPERTY_HOST_CACHED_BIT) == 0) return i;
    }
    return best;
}

bool map_shared_memory(const std::string& path, uint64_t size, SharedMapping& out) {
    const int fd = ::open(path.c_str(), O_RDWR | O_CLOEXEC);
    if (fd < 0) return false;
    // The import wants the mapping page-aligned and whole pages long, which
    // is what the client sized the file to.
    const size_t length = static_cast<size_t>(size);
    void* p = ::mmap(nullptr, length, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    ::close(fd);
    if (p == MAP_FAILED) return false;
    out.address = p;
    out.length = length;
    return true;
}

void unmap_shared_memory(SharedMapping& m) {
    if (m.address != nullptr) ::munmap(m.address, m.length);
    m.address = nullptr;
    m.length = 0;
}

uint64_t vk_allocate_memory(uint64_t size, uint32_t type_index, const std::vector<uint8_t>& in,
                             uint32_t node_count, std::vector<uint8_t>& out, uint32_t* out_len,
                             const std::string& shared_path) {
    Loader& l = loader();
    if (l.allocate_memory == nullptr) {
        return static_cast<uint64_t>(static_cast<int32_t>(VK_ERROR_INITIALIZATION_FAILED));
    }
    std::vector<std::vector<uint8_t>> storage;
    void* chain = rebuild_chain(in.data(), in.size(), node_count, storage);
    VkMemoryAllocateInfo ai{};
    ai.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    ai.pNext = chain;
    ai.allocationSize = size;
    ai.memoryTypeIndex = type_index;

    // Shared host memory, when the client asked for it (`shared_path` is
    // the file it already mapped). Importing those same pages as device
    // memory means the engine's writes are the device's memory -- nothing
    // is copied, and the whole dirty-page tracking and flush path stops
    // being needed for this allocation.
    VkImportMemoryHostPointerInfoEXT import{};
    SharedMapping mapping;
    if (!shared_path.empty() && map_shared_memory(shared_path, size, mapping) &&
        l.get_memory_host_pointer_properties != nullptr) {
        VkMemoryHostPointerPropertiesEXT props{};
        props.sType = VK_STRUCTURE_TYPE_MEMORY_HOST_POINTER_PROPERTIES_EXT;
        const VkResult pr = l.get_memory_host_pointer_properties(
            l.device, VK_EXTERNAL_MEMORY_HANDLE_TYPE_HOST_ALLOCATION_BIT_EXT, mapping.address,
            &props);
        // The driver decides which memory types can back an imported host
        // pointer, and it need not include the one the engine picked. When
        // it does not, this falls through to an ordinary allocation and
        // the copying path, rather than guessing at a different type and
        // handing the engine memory with the wrong properties.
        // The driver decides which memory types can back an imported host
        // pointer, and on this hardware that excludes the DEVICE_LOCAL |
        // HOST_VISIBLE type (BAR memory) the engine picks for its
        // streaming buffers -- which is exactly the memory it rewrites
        // every frame, and so exactly what was still being copied.
        //
        // STUD_SHARE_ALL_HOST_MEMORY=1 allocates those from an importable
        // host-visible type instead. The trade is real and worth stating:
        // the GPU then reads that memory across PCIe rather than from
        // VRAM. On this workload the CPU is the constraint (17 ms against
        // 6 ms of GPU), so paying the GPU to stop the CPU copying is the
        // right way round -- but it is a trade, not a free win, which is
        // why it is a switch.
        uint32_t use_type = type_index;
        if (pr == VK_SUCCESS && (props.memoryTypeBits & (1u << type_index)) == 0 &&
            std::getenv("STUD_SHARE_ALL_HOST_MEMORY") != nullptr) {
            const uint32_t substitute = importable_host_visible_type(props.memoryTypeBits);
            if (substitute != UINT32_MAX) {
                static std::set<uint32_t> reported;
                if (reported.insert(type_index).second) {
                    std::printf("stud-render-host: memory type %u is not importable; sharing it as "
                                "type %u instead\n",
                                type_index, substitute);
                    std::fflush(stdout);
                }
                use_type = substitute;
                ai.memoryTypeIndex = substitute;
            }
        }
        if (pr == VK_SUCCESS && (props.memoryTypeBits & (1u << use_type)) != 0) {
            import.sType = VK_STRUCTURE_TYPE_IMPORT_MEMORY_HOST_POINTER_INFO_EXT;
            import.handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_HOST_ALLOCATION_BIT_EXT;
            import.pHostPointer = mapping.address;
            import.pNext = ai.pNext;
            ai.pNext = &import;
        } else {
            static std::set<uint32_t> reported;
            if (reported.insert(type_index).second) {
                std::printf("stud-render-host: memory type %u is not host-importable "
                            "(driver allows types 0x%x) -- this one is copied\n",
                            type_index, pr == VK_SUCCESS ? props.memoryTypeBits : 0u);
                std::fflush(stdout);
            }
            unmap_shared_memory(mapping);
        }
    }

    VkDeviceMemory memory = VK_NULL_HANDLE;
    VkResult r = l.allocate_memory(l.device, &ai, nullptr, &memory);
    if (r != VK_SUCCESS && import.pHostPointer != nullptr) {
        // The driver said this memory type can back an imported host
        // pointer and then refused the import anyway -- live-caught on
        // Intel, which answers VK_ERROR_INVALID_EXTERNAL_HANDLE
        // (-1000072003) for a type its own
        // vkGetMemoryHostPointerPropertiesEXT had just allowed. The
        // engine takes a failed allocation as fatal
        // (RBXCRASH: OutOfMemoryGraphics) and the process wedges, so
        // sharing is an optimisation that must never be the reason an
        // allocation fails: drop the import and allocate ordinarily,
        // which puts this one allocation back on the copying path.
        static std::set<uint32_t> reported;
        if (reported.insert(type_index).second) {
            std::printf("stud-render-host: memory type %u was allowed for import and then "
                        "refused it (%d) -- this one is copied\n",
                        type_index, static_cast<int>(r));
            std::fflush(stdout);
        }
        unmap_shared_memory(mapping);
        import.pHostPointer = nullptr;
        ai.pNext = chain;
        ai.memoryTypeIndex = type_index;
        r = l.allocate_memory(l.device, &ai, nullptr, &memory);
    }
    if (r != VK_SUCCESS) {
        std::printf("stud-render-host: vkAllocateMemory -> %d (%llu bytes, type %u)\n",
                    static_cast<int>(r), static_cast<unsigned long long>(size), type_index);
        std::fflush(stdout);
        return static_cast<uint64_t>(static_cast<int32_t>(r));
    }
    const bool imported = import.pHostPointer != nullptr;
    if (imported) {
        static std::set<uint32_t> shared_types;
        if (shared_types.insert(type_index).second) {
            std::printf("stud-render-host: memory type %u is shared with the engine\n", type_index);
            std::fflush(stdout);
        }
        std::lock_guard<std::mutex> lock(shared_memory_mutex());
        shared_memory()[to_u64(memory)] = mapping;
        static bool announced = false;
        if (!announced) {
            announced = true;
            std::printf("stud-render-host: host-visible memory is shared with the engine, not "
                        "copied\n");
            std::fflush(stdout);
        }
    }
    // The client must be told whether the import actually happened: it
    // only hands the engine the shared pointer if the device really is
    // reading those pages. Getting this wrong is silent and total -- the
    // engine writes into memory nothing renders from, and the screen is
    // black.
    out.resize(sizeof(uint64_t) * 2);
    const uint64_t handle_value = to_u64(memory);
    const uint64_t shared_flag = imported ? 1u : 0u;
    std::memcpy(out.data(), &handle_value, sizeof(handle_value));
    std::memcpy(out.data() + sizeof(handle_value), &shared_flag, sizeof(shared_flag));
    *out_len = static_cast<uint32_t>(out.size());
    return static_cast<uint64_t>(static_cast<int32_t>(VK_SUCCESS));
}

uint64_t vk_bind_image_memory(uint64_t image, uint64_t memory, uint64_t offset) {
    Loader& l = loader();
    if (l.bind_image_memory == nullptr) {
        return static_cast<uint64_t>(static_cast<int32_t>(VK_ERROR_INITIALIZATION_FAILED));
    }
    VkResult r = l.bind_image_memory(l.device, from_u64<VkImage>(image),
                                      from_u64<VkDeviceMemory>(memory), offset);
    return static_cast<uint64_t>(static_cast<int32_t>(r));
}

uint64_t vk_free_memory_shared_cleanup(uint64_t memory) {
    std::lock_guard<std::mutex> lock(shared_memory_mutex());
    auto it = shared_memory().find(memory);
    if (it == shared_memory().end()) return 0;
    unmap_shared_memory(it->second);
    shared_memory().erase(it);
    return 1;
}

uint64_t vk_free_memory(uint64_t memory) {
    Loader& l = loader();
    if (l.free_memory == nullptr) return 0;
    l.mapped.erase(memory);
    l.free_memory(l.device, from_u64<VkDeviceMemory>(memory), nullptr);
    // The import keeps the pages alive as long as the memory object does,
    // so the mapping is dropped only now.
    vk_free_memory_shared_cleanup(memory);
    return 0;
}

uint64_t vk_map_memory(uint64_t memory, uint64_t offset, uint64_t size, uint32_t flags) {
    Loader& l = loader();
    if (l.map_memory == nullptr) {
        return static_cast<uint64_t>(static_cast<int32_t>(VK_ERROR_INITIALIZATION_FAILED));
    }
    void* p = nullptr;
    VkResult r =
        l.map_memory(l.device, from_u64<VkDeviceMemory>(memory), offset, size, flags, &p);
    if (r != VK_SUCCESS) return static_cast<uint64_t>(static_cast<int32_t>(r));
    l.mapped[memory] = p;
    return static_cast<uint64_t>(static_cast<int32_t>(VK_SUCCESS));
}

// The engine's writes land here: the client ships whatever it changed in
// its staging copy, and this copies it into the real mapping.
uint64_t vk_write_mapped_memory(uint64_t memory, uint64_t offset, const std::vector<uint8_t>& in) {
    Loader& l = loader();
    auto it = l.mapped.find(memory);
    if (it == l.mapped.end() || it->second == nullptr) {
        return static_cast<uint64_t>(static_cast<int32_t>(VK_ERROR_MEMORY_MAP_FAILED));
    }
    std::memcpy(static_cast<uint8_t*>(it->second) + offset, in.data(), in.size());
    return static_cast<uint64_t>(static_cast<int32_t>(VK_SUCCESS));
}

uint64_t vk_unmap_memory(uint64_t memory) {
    Loader& l = loader();
    if (l.unmap_memory == nullptr) return 0;
    l.unmap_memory(l.device, from_u64<VkDeviceMemory>(memory));
    l.mapped.erase(memory);
    return 0;
}

uint64_t vk_flush_mapped_memory_ranges(uint64_t memory, uint64_t offset, uint64_t size) {
    Loader& l = loader();
    if (l.flush_mapped_memory_ranges == nullptr) {
        return static_cast<uint64_t>(static_cast<int32_t>(VK_SUCCESS));
    }
    VkMappedMemoryRange range{};
    range.sType = VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE;
    range.memory = from_u64<VkDeviceMemory>(memory);
    range.offset = offset;
    range.size = size;
    VkResult r = l.flush_mapped_memory_ranges(l.device, 1, &range);
    return static_cast<uint64_t>(static_cast<int32_t>(r));
}

uint64_t vk_get_surface_formats(uint64_t physical_device, uint64_t surface, uint32_t capacity,
                                 std::vector<uint8_t>& out, uint32_t* out_len) {
    Loader& l = loader();
    if (l.get_surface_formats == nullptr) {
        return static_cast<uint64_t>(static_cast<int32_t>(VK_ERROR_INITIALIZATION_FAILED));
    }
    VkPhysicalDevice pd =
        reinterpret_cast<VkPhysicalDevice>(static_cast<uintptr_t>(physical_device));
    VkSurfaceKHR surf = from_u64<VkSurfaceKHR>(surface);
    uint32_t count = 0;
    VkResult r = l.get_surface_formats(pd, surf, &count, nullptr);
    if (r != VK_SUCCESS && r != VK_INCOMPLETE) {
        return static_cast<uint64_t>(static_cast<int32_t>(r));
    }
    std::vector<VkSurfaceFormatKHR> formats;
    uint32_t returned = 0;
    if (capacity > 0 && count > 0) {
        returned = count < capacity ? count : capacity;
        formats.resize(returned);
        r = l.get_surface_formats(pd, surf, &returned, formats.data());
    }
    const uint32_t reported = capacity > 0 ? returned : count;
    const uint32_t stride = static_cast<uint32_t>(sizeof(VkSurfaceFormatKHR));
    out.resize(sizeof(uint32_t) * 2 + stride * returned);
    std::memcpy(out.data(), &reported, sizeof(reported));
    std::memcpy(out.data() + sizeof(uint32_t), &stride, sizeof(stride));
    for (uint32_t i = 0; i < returned; ++i) {
        std::memcpy(out.data() + sizeof(uint32_t) * 2 + i * stride, &formats[i], stride);
    }
    *out_len = static_cast<uint32_t>(out.size());
    return static_cast<uint64_t>(static_cast<int32_t>(r));
}

uint64_t vk_get_surface_present_modes(uint64_t physical_device, uint64_t surface, uint32_t capacity,
                                       std::vector<uint8_t>& out, uint32_t* out_len) {
    Loader& l = loader();
    if (l.get_surface_present_modes == nullptr) {
        return static_cast<uint64_t>(static_cast<int32_t>(VK_ERROR_INITIALIZATION_FAILED));
    }
    VkPhysicalDevice pd =
        reinterpret_cast<VkPhysicalDevice>(static_cast<uintptr_t>(physical_device));
    VkSurfaceKHR surf = from_u64<VkSurfaceKHR>(surface);
    uint32_t count = 0;
    VkResult r = l.get_surface_present_modes(pd, surf, &count, nullptr);
    if (r != VK_SUCCESS && r != VK_INCOMPLETE) {
        return static_cast<uint64_t>(static_cast<int32_t>(r));
    }
    std::vector<VkPresentModeKHR> modes;
    uint32_t returned = 0;
    if (capacity > 0 && count > 0) {
        returned = count < capacity ? count : capacity;
        modes.resize(returned);
        r = l.get_surface_present_modes(pd, surf, &returned, modes.data());
    }
    const uint32_t reported = capacity > 0 ? returned : count;
    out.resize(sizeof(uint32_t) + sizeof(uint32_t) * returned);
    std::memcpy(out.data(), &reported, sizeof(reported));
    for (uint32_t i = 0; i < returned; ++i) {
        uint32_t m = static_cast<uint32_t>(modes[i]);
        std::memcpy(out.data() + sizeof(uint32_t) * (1 + i), &m, sizeof(m));
    }
    *out_len = static_cast<uint32_t>(out.size());
    return static_cast<uint64_t>(static_cast<int32_t>(r));
}

uint64_t vk_get_surface_support(uint64_t physical_device, uint32_t queue_family, uint64_t surface,
                                 std::vector<uint8_t>& out, uint32_t* out_len) {
    Loader& l = loader();
    if (l.get_surface_support == nullptr) {
        return static_cast<uint64_t>(static_cast<int32_t>(VK_ERROR_INITIALIZATION_FAILED));
    }
    VkBool32 supported = VK_FALSE;
    VkResult r = l.get_surface_support(
        reinterpret_cast<VkPhysicalDevice>(static_cast<uintptr_t>(physical_device)), queue_family,
        from_u64<VkSurfaceKHR>(surface), &supported);
    write_pod(supported, out, out_len);
    return static_cast<uint64_t>(static_cast<int32_t>(r));
}

// Which present mode the swapchain actually gets.
//
// The engine asks for FIFO, which is v-sync: one frame per refresh, and a
// hard ceiling at the display's rate however much headroom the machine
// has. MAILBOX renders as fast as it can and shows the newest finished
// frame at each refresh -- no tearing, no ceiling -- and IMMEDIATE is the
// uncapped fallback that every driver has, at the cost of tearing.
//
// Only ever a substitution among modes the driver advertises FOR THIS
// SURFACE, asked for each time rather than assumed: presenting with a
// mode the surface does not support is undefined behaviour, not a slow
// path.
//
// STUD_PRESENT_MODE=engine|mailbox|immediate|fifo|fifo-relaxed picks one.
// `engine` forwards whatever the engine asked for, which is the control
// for measuring whether any of this helps.
VkPresentModeKHR choose_present_mode(VkPresentModeKHR requested, VkSurfaceKHR surface) {
    static const std::string choice = [] {
        const char* v = std::getenv("STUD_PRESENT_MODE");
        return std::string(v != nullptr ? v : "mailbox");
    }();
    if (choice == "engine" || surface == VK_NULL_HANDLE) return requested;

    Loader& l = loader();
    if (l.get_surface_present_modes == nullptr || l.physical_device == VK_NULL_HANDLE) {
        return requested;
    }
    uint32_t count = 0;
    l.get_surface_present_modes(l.physical_device, surface, &count, nullptr);
    if (count == 0) return requested;
    std::vector<VkPresentModeKHR> modes(count);
    l.get_surface_present_modes(l.physical_device, surface, &count, modes.data());
    auto advertised = [&](VkPresentModeKHR m) {
        return std::find(modes.begin(), modes.end(), m) != modes.end();
    };

    std::vector<VkPresentModeKHR> wanted;
    if (choice == "immediate") {
        wanted = {VK_PRESENT_MODE_IMMEDIATE_KHR};
    } else if (choice == "fifo") {
        wanted = {VK_PRESENT_MODE_FIFO_KHR};
    } else if (choice == "fifo-relaxed") {
        wanted = {VK_PRESENT_MODE_FIFO_RELAXED_KHR};
    } else {
        // The default: uncap without tearing where the driver can, and
        // leave the engine's own choice alone where it cannot.
        wanted = {VK_PRESENT_MODE_MAILBOX_KHR};
    }
    for (VkPresentModeKHR m : wanted) {
        if (!advertised(m) || m == requested) continue;
        static std::set<int> announced;
        if (announced.insert(static_cast<int>(m)).second) {
            std::printf("stud-render-host: present mode %d -> %d (%s)\n",
                         static_cast<int>(requested), static_cast<int>(m), choice.c_str());
            std::fflush(stdout);
        }
        return m;
    }
    return requested;
}

uint64_t vk_create_swapchain(const std::vector<uint8_t>& in, std::vector<uint8_t>& out,
                              uint32_t* out_len) {
    Loader& l = loader();
    if (l.create_swapchain == nullptr) {
        return static_cast<uint64_t>(static_cast<int32_t>(VK_ERROR_INITIALIZATION_FAILED));
    }
    if (in.size() < sizeof(vk_wire::CreateSwapchainHeader)) {
        return static_cast<uint64_t>(static_cast<int32_t>(VK_ERROR_INITIALIZATION_FAILED));
    }
    vk_wire::CreateSwapchainHeader h{};
    std::memcpy(&h, in.data(), sizeof(h));
    std::vector<uint32_t> families(h.queue_family_count);
    if (h.queue_family_count > 0 &&
        in.size() >= sizeof(h) + sizeof(uint32_t) * h.queue_family_count) {
        std::memcpy(families.data(), in.data() + sizeof(h),
                    sizeof(uint32_t) * h.queue_family_count);
    }

    VkSwapchainCreateInfoKHR ci{};
    ci.sType = VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR;
    ci.flags = h.flags;
    ci.surface = from_u64<VkSurfaceKHR>(h.surface);
    ci.minImageCount = h.min_image_count;
    // STUD_VK_FORCE_OPAQUE_FORMAT=1 swaps a B8G8R8A8 swapchain for
    // B8G8R8X8, whose DRM format is XR24 rather than AR24.
    //
    // The engine asks for an alpha format, and the driver duly allocates
    // buffers with alpha bits the compositor is entitled to honour. The
    // reference test (stud_try_vulkan_window), which presents correctly on
    // this same GPU and window, gets XR24. Off by default because it is a
    // real behaviour change to what the engine requested, not a fix in
    // itself.
    ci.imageFormat = static_cast<VkFormat>(h.image_format);
    static const bool force_opaque_format = std::getenv("STUD_VK_FORCE_OPAQUE_FORMAT") != nullptr;
    if (force_opaque_format) {
        if (ci.imageFormat == VK_FORMAT_B8G8R8A8_UNORM) {
            ci.imageFormat = VK_FORMAT_B8G8R8A8_SRGB;
        }
        std::printf("stud-render-host: forcing swapchain format %u -> %u\n", h.image_format,
                    static_cast<uint32_t>(ci.imageFormat));
        std::fflush(stdout);
    }
    ci.imageColorSpace = static_cast<VkColorSpaceKHR>(h.image_color_space);
    ci.imageExtent = {h.width, h.height};
    ci.imageArrayLayers = h.image_array_layers;
    ci.imageUsage = h.image_usage;
    ci.imageSharingMode = static_cast<VkSharingMode>(h.sharing_mode);
    ci.queueFamilyIndexCount = h.queue_family_count;
    ci.pQueueFamilyIndices = families.empty() ? nullptr : families.data();
    ci.preTransform = static_cast<VkSurfaceTransformFlagBitsKHR>(h.pre_transform);
    ci.compositeAlpha = static_cast<VkCompositeAlphaFlagBitsKHR>(h.composite_alpha);
    std::printf("stud-render-host: swapchain compositeAlpha=0x%x format=%u usage=0x%x\n",
                h.composite_alpha, h.image_format, h.image_usage);
    std::fflush(stdout);
    ci.presentMode = choose_present_mode(static_cast<VkPresentModeKHR>(h.present_mode),
                                          from_u64<VkSurfaceKHR>(h.surface));
    ci.clipped = h.clipped;
    ci.oldSwapchain = from_u64<VkSwapchainKHR>(h.old_swapchain);

    VkSwapchainKHR swapchain = VK_NULL_HANDLE;
    VkResult r = l.create_swapchain(l.device, &ci, nullptr, &swapchain);
    std::printf("stud-render-host: vkCreateSwapchainKHR -> %d (%ux%u format=%u images>=%u)\n",
                static_cast<int>(r), h.width, h.height, h.image_format, h.min_image_count);
    std::fflush(stdout);
    if (r != VK_SUCCESS) return static_cast<uint64_t>(static_cast<int32_t>(r));
    g_swapchain_extents[to_u64(swapchain)] = ci.imageExtent;
    return write_handle(to_u64(swapchain), out, out_len);
}

// True when this swapchain no longer matches the window it presents to.
//
// Deliberately NOT wired into vkAcquireNextImageKHR or vkQueuePresentKHR.
// A Wayland surface never goes out of date on its own (the buffer defines
// the size), so returning VK_ERROR_OUT_OF_DATE_KHR on a resize looks like
// the textbook fix -- and it was live-tested and is wrong for this engine:
// it logs `VULKAN ERROR: vkAcquireNextImageKHR ... returned -1000001004`
// and stops presenting entirely (frozen at the same frame, confirmed by a
// present counter that stopped advancing). The engine rebuilds its
// swapchain from its own resize path instead, which only needs
// vkGetPhysicalDeviceSurfaceCapabilitiesKHR to report the window's real
// current size -- that is what sync_vk_window_size() in render-host's main
// loop keeps true. Kept because "did this swapchain outlive its window
// size" is the question a future resize bug will ask first.
bool swapchain_is_out_of_date(uint64_t swapchain) {
    const uint32_t win_w = g_window_width.load(std::memory_order_relaxed);
    const uint32_t win_h = g_window_height.load(std::memory_order_relaxed);
    if (win_w == 0 || win_h == 0) return false;
    auto it = g_swapchain_extents.find(swapchain);
    if (it == g_swapchain_extents.end()) return false;
    return it->second.width != win_w || it->second.height != win_h;
}

uint64_t vk_get_swapchain_images(uint64_t swapchain, uint32_t capacity, std::vector<uint8_t>& out,
                                  uint32_t* out_len) {
    Loader& l = loader();
    if (l.get_swapchain_images == nullptr) {
        return static_cast<uint64_t>(static_cast<int32_t>(VK_ERROR_INITIALIZATION_FAILED));
    }
    VkSwapchainKHR sc = from_u64<VkSwapchainKHR>(swapchain);
    uint32_t count = 0;
    VkResult r = l.get_swapchain_images(l.device, sc, &count, nullptr);
    if (r != VK_SUCCESS && r != VK_INCOMPLETE) {
        return static_cast<uint64_t>(static_cast<int32_t>(r));
    }
    std::vector<VkImage> images;
    uint32_t returned = 0;
    if (capacity > 0 && count > 0) {
        returned = count < capacity ? count : capacity;
        images.resize(returned);
        r = l.get_swapchain_images(l.device, sc, &returned, images.data());
    }
    const uint32_t reported = capacity > 0 ? returned : count;
    out.resize(sizeof(uint32_t) + sizeof(uint64_t) * returned);
    std::memcpy(out.data(), &reported, sizeof(reported));
    for (uint32_t i = 0; i < returned; ++i) {
        uint64_t h = to_u64(images[i]);
        l.swapchain_images.insert(h);
        l.swapchain_image_list[swapchain].push_back(h);
        // A handle can be reused by a later swapchain, so anything that
        // comes back live is no longer retired.
        l.retired_images.erase(h);
        std::memcpy(out.data() + sizeof(uint32_t) + i * sizeof(h), &h, sizeof(h));
    }
    *out_len = static_cast<uint32_t>(out.size());
    return static_cast<uint64_t>(static_cast<int32_t>(r));
}

uint64_t vk_get_image_format_properties2(uint64_t physical_device, const std::vector<uint8_t>& in,
                                          uint32_t node_count, std::vector<uint8_t>& out,
                                          uint32_t* out_len) {
    Loader& l = loader();
    if (l.get_image_format_properties2 == nullptr || in.size() < sizeof(uint32_t) * 6) {
        return static_cast<uint64_t>(static_cast<int32_t>(VK_ERROR_INITIALIZATION_FAILED));
    }
    uint32_t q[6] = {};
    std::memcpy(q, in.data(), sizeof(q));

    VkPhysicalDeviceImageFormatInfo2 info{};
    info.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_IMAGE_FORMAT_INFO_2;
    info.format = static_cast<VkFormat>(q[0]);
    info.type = static_cast<VkImageType>(q[1]);
    info.tiling = static_cast<VkImageTiling>(q[2]);
    info.usage = q[3];
    info.flags = q[4];

    std::vector<std::vector<uint8_t>> storage;
    void* chain = rebuild_chain(in.data() + sizeof(q), in.size() - sizeof(q), node_count, storage);

    VkImageFormatProperties2 props{};
    props.sType = VK_STRUCTURE_TYPE_IMAGE_FORMAT_PROPERTIES_2;
    props.pNext = chain;
    VkResult r = l.get_image_format_properties2(
        reinterpret_cast<VkPhysicalDevice>(static_cast<uintptr_t>(physical_device)), &info,
        &props);

    const uint32_t base = static_cast<uint32_t>(sizeof(VkImageFormatProperties));
    out.resize(sizeof(uint32_t) + base);
    std::memcpy(out.data(), &base, sizeof(base));
    std::memcpy(out.data() + sizeof(uint32_t), &props.imageFormatProperties, base);
    flatten_chain(chain, out);
    *out_len = static_cast<uint32_t>(out.size());
    return static_cast<uint64_t>(static_cast<int32_t>(r));
}

uint64_t vk_create_buffer(uint32_t flags, uint64_t size, uint32_t usage, uint32_t sharing_mode,
                           std::vector<uint8_t>& out, uint32_t* out_len) {
    Loader& l = loader();
    if (l.create_buffer == nullptr) {
        return static_cast<uint64_t>(static_cast<int32_t>(VK_ERROR_INITIALIZATION_FAILED));
    }
    VkBufferCreateInfo ci{};
    ci.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    ci.flags = flags;
    ci.size = size;
    ci.usage = usage;
    ci.sharingMode = static_cast<VkSharingMode>(sharing_mode);
    VkBuffer buffer = VK_NULL_HANDLE;
    VkResult r = l.create_buffer(l.device, &ci, nullptr, &buffer);
    // Deliberately no per-call print here. The engine creates buffers
    // sixteen times a frame, so a line each -- with a flush -- is a write
    // syscall per creation and a log that grows without bound. Failures
    // still say so, below. (Fourth time in this project that a diagnostic
    // was found still armed in a normal run; the others are recorded in
    // the engineering notes.)
    if (r != VK_SUCCESS) {
        std::printf("stud-render-host: vkCreateBuffer -> %d (%llu bytes usage=0x%x)\n",
                    static_cast<int>(r), static_cast<unsigned long long>(size), usage);
        std::fflush(stdout);
        return static_cast<uint64_t>(static_cast<int32_t>(r));
    }
    return write_handle(to_u64(buffer), out, out_len);
}

uint64_t vk_get_buffer_memory_requirements(uint64_t buffer, std::vector<uint8_t>& out,
                                            uint32_t* out_len) {
    Loader& l = loader();
    if (l.get_buffer_memory_requirements == nullptr) {
        return static_cast<uint64_t>(static_cast<int32_t>(VK_ERROR_INITIALIZATION_FAILED));
    }
    VkMemoryRequirements req{};
    l.get_buffer_memory_requirements(l.device, from_u64<VkBuffer>(buffer), &req);
    return write_pod(req, out, out_len);
}

uint64_t vk_bind_buffer_memory(uint64_t buffer, uint64_t memory, uint64_t offset) {
    Loader& l = loader();
    if (l.bind_buffer_memory == nullptr) {
        return static_cast<uint64_t>(static_cast<int32_t>(VK_ERROR_INITIALIZATION_FAILED));
    }
    VkResult r = l.bind_buffer_memory(l.device, from_u64<VkBuffer>(buffer),
                                       from_u64<VkDeviceMemory>(memory), offset);
    if (r == VK_SUCCESS) {
        std::lock_guard<std::mutex> lock(buffer_memory_mutex());
        buffer_memory()[buffer] = memory;
    }
    return static_cast<uint64_t>(static_cast<int32_t>(r));
}

uint64_t vk_create_image_view(const std::vector<uint8_t>& in, std::vector<uint8_t>& out,
                               uint32_t* out_len) {
    Loader& l = loader();
    if (l.create_image_view == nullptr || in.size() < sizeof(uint32_t) * 12 + sizeof(uint64_t)) {
        return static_cast<uint64_t>(static_cast<int32_t>(VK_ERROR_INITIALIZATION_FAILED));
    }
    uint64_t image = 0;
    std::memcpy(&image, in.data(), sizeof(image));
    uint32_t q[12] = {};
    std::memcpy(q, in.data() + sizeof(image), sizeof(q));

    VkImageViewCreateInfo ci{};
    ci.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    ci.flags = q[0];
    ci.image = from_u64<VkImage>(image);
    ci.viewType = static_cast<VkImageViewType>(q[1]);
    ci.format = static_cast<VkFormat>(q[2]);
    ci.components = {static_cast<VkComponentSwizzle>(q[3]), static_cast<VkComponentSwizzle>(q[4]),
                     static_cast<VkComponentSwizzle>(q[5]), static_cast<VkComponentSwizzle>(q[6])};
    ci.subresourceRange = {q[7], q[8], q[9], q[10], q[11]};

    VkImageView view = VK_NULL_HANDLE;
    VkResult r = l.create_image_view(l.device, &ci, nullptr, &view);
    if (r != VK_SUCCESS) return static_cast<uint64_t>(static_cast<int32_t>(r));
    if (l.swapchain_images.count(image) != 0) l.swapchain_views.insert(to_u64(view));
    return write_handle(to_u64(view), out, out_len);
}

uint64_t vk_create_shader_module(const std::vector<uint8_t>& in, std::vector<uint8_t>& out,
                                  uint32_t* out_len) {
    Loader& l = loader();
    if (l.create_shader_module == nullptr) {
        return static_cast<uint64_t>(static_cast<int32_t>(VK_ERROR_INITIALIZATION_FAILED));
    }
    VkShaderModuleCreateInfo ci{};
    ci.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
    ci.codeSize = in.size();
    ci.pCode = reinterpret_cast<const uint32_t*>(in.data());
    // SPIR-V is a stream of 32-bit words starting with magic 0x07230203.
    // A module that arrives truncated or misaligned can still create
    // successfully on some drivers and then draw nothing, which is
    // indistinguishable from every other cause of a black frame -- so
    // check the shape here rather than trusting the byte count.
    if (in.size() < 20 || (in.size() % 4) != 0) {
        std::printf("stud-render-host: SHADER suspicious size %zu\n", in.size());
        std::fflush(stdout);
    } else {
        uint32_t magic = 0;
        std::memcpy(&magic, in.data(), sizeof(magic));
        if (magic != 0x07230203u) {
            std::printf("stud-render-host: SHADER bad magic 0x%08x (size %zu)\n", magic,
                        in.size());
            std::fflush(stdout);
        }
    }

    VkShaderModule module = VK_NULL_HANDLE;
    VkResult r = l.create_shader_module(l.device, &ci, nullptr, &module);
    if (r != VK_SUCCESS) {
        std::printf("stud-render-host: vkCreateShaderModule -> %d (%zu bytes)\n",
                    static_cast<int>(r), in.size());
        std::fflush(stdout);
    }
    if (r != VK_SUCCESS) return static_cast<uint64_t>(static_cast<int32_t>(r));
    return write_handle(to_u64(module), out, out_len);
}

// Defined here rather than beside the rest of the descriptor-set id
// machinery below, because destroying a pool invalidates every id
// allocated from it and that happens here.
std::mutex& descriptor_set_mutex() {
    static std::mutex m;
    return m;
}
struct OwnedDescriptorSet {
    VkDescriptorSet set = VK_NULL_HANDLE;
    uint64_t pool = 0;
};

std::unordered_map<uint64_t, OwnedDescriptorSet>& descriptor_sets() {
    static std::unordered_map<uint64_t, OwnedDescriptorSet> m;
    return m;
}

uint64_t vk_destroy_handle(uint32_t kind, uint64_t handle) {
    Loader& l = loader();
    using K = vk_wire::DestroyKind;
    switch (static_cast<K>(kind)) {
        case K::Buffer: {
            {
                // Handle values are reused the moment an object dies, so
                // a stale entry would answer for a completely different
                // buffer.
                std::lock_guard<std::mutex> lock(buffer_memory_mutex());
                buffer_memory().erase(handle);
            }
            if (l.destroy_buffer) l.destroy_buffer(l.device, from_u64<VkBuffer>(handle), nullptr);
            break;
        }
        case K::Image:
            // Forget it as well as destroying it. A Vulkan handle is only
            // unique while the object lives, and drivers reuse values
            // aggressively -- so an offscreen image created after a
            // swapchain was torn down (which is exactly what a resize
            // does) can land on a dead swapchain image's handle and be
            // mistaken for one. These sets drive "does anything render to
            // the screen" reporting and STUD_VK_FORCE_CLEAR, so a stale
            // entry does not just log noise: it aims a diagnostic at the
            // wrong render pass, which is worse than no diagnostic.
            l.swapchain_images.erase(handle);
            if (l.destroy_image) l.destroy_image(l.device, from_u64<VkImage>(handle), nullptr);
            break;
        case K::ImageView:
            l.swapchain_views.erase(handle);
            if (l.destroy_image_view) {
                l.destroy_image_view(l.device, from_u64<VkImageView>(handle), nullptr);
            }
            break;
        case K::ShaderModule:
            if (l.destroy_shader_module) {
                l.destroy_shader_module(l.device, from_u64<VkShaderModule>(handle), nullptr);
            }
            break;
        case K::Semaphore:
            if (l.destroy_semaphore) {
                l.destroy_semaphore(l.device, from_u64<VkSemaphore>(handle), nullptr);
            }
            break;
        case K::Fence:
            if (l.destroy_fence) l.destroy_fence(l.device, from_u64<VkFence>(handle), nullptr);
            break;
        case K::CommandPool:
            if (l.destroy_command_pool) {
                l.destroy_command_pool(l.device, from_u64<VkCommandPool>(handle), nullptr);
            }
            break;
        case K::QueryPool:
            if (l.destroy_query_pool) {
                l.destroy_query_pool(l.device, from_u64<VkQueryPool>(handle), nullptr);
            }
            break;
        case K::Swapchain: {
            g_swapchain_extents.erase(handle);
            auto images = l.swapchain_image_list.find(handle);
            if (images != l.swapchain_image_list.end()) {
                for (uint64_t image : images->second) {
                    l.swapchain_images.erase(image);
                    l.swapchain_views.erase(image);
                    l.retired_images.insert(image);
                }
                l.swapchain_image_list.erase(images);
            }
            if (l.destroy_swapchain) {
                l.destroy_swapchain(l.device, from_u64<VkSwapchainKHR>(handle), nullptr);
            }
            break;
        }
        case K::Surface:
            if (l.destroy_surface) {
                l.destroy_surface(l.instance, from_u64<VkSurfaceKHR>(handle), nullptr);
            }
            break;
        case K::Framebuffer:
            l.swapchain_framebuffers.erase(handle);
            if (l.destroy_framebuffer) {
                l.destroy_framebuffer(l.device, from_u64<VkFramebuffer>(handle), nullptr);
            }
            break;
        case K::PipelineCache:
            if (l.destroy_pipeline_cache) {
                l.destroy_pipeline_cache(l.device, from_u64<VkPipelineCache>(handle), nullptr);
            }
            break;
        case K::Device:
            // Deliberately not destroyed: this process owns exactly one
            // device for its whole life, and the engine tears its
            // renderer down and rebuilds it repeatedly (once per mode
            // probe). Destroying it here would invalidate every resolved
            // device command while the engine still holds them.
            break;
        case K::Instance:
            // Same reasoning as Device.
            break;
        case K::Sampler:
            if (l.destroy_sampler) {
                l.destroy_sampler(l.device, from_u64<VkSampler>(handle), nullptr);
            }
            break;
        case K::RenderPass:
            if (l.destroy_render_pass) {
                l.destroy_render_pass(l.device, from_u64<VkRenderPass>(handle), nullptr);
            }
            break;
        case K::Pipeline:
            if (l.destroy_pipeline) {
                l.destroy_pipeline(l.device, from_u64<VkPipeline>(handle), nullptr);
            }
            break;
        case K::PipelineLayout:
            if (l.destroy_pipeline_layout) {
                l.destroy_pipeline_layout(l.device, from_u64<VkPipelineLayout>(handle), nullptr);
            }
            break;
        case K::DescriptorSetLayout:
            if (l.destroy_descriptor_set_layout) {
                l.destroy_descriptor_set_layout(l.device,
                                                 from_u64<VkDescriptorSetLayout>(handle), nullptr);
            }
            break;
        case K::DescriptorPool: {
            // Destroying a pool destroys every set allocated from it, so
            // the ids naming those sets must stop resolving -- the same
            // bookkeeping vkResetDescriptorPool already does, for the
            // same reason: a later use would hand the driver a freed
            // handle.
            {
                std::lock_guard<std::mutex> lock(descriptor_set_mutex());
                auto& sets = descriptor_sets();
                for (auto it = sets.begin(); it != sets.end();) {
                    it = it->second.pool == handle ? sets.erase(it) : std::next(it);
                }
            }
            if (l.destroy_descriptor_pool) {
                l.destroy_descriptor_pool(l.device, from_u64<VkDescriptorPool>(handle), nullptr);
            }
            break;
        }
        case K::DescriptorUpdateTemplate:
            if (l.destroy_descriptor_update_template) {
                l.destroy_descriptor_update_template(
                    l.device, from_u64<VkDescriptorUpdateTemplate>(handle), nullptr);
            }
            break;
        default:
            break;
    }
    return 0;
}

// ---- draw pipeline ---------------------------------------------------

uint64_t vk_create_render_pass(const std::vector<uint8_t>& in, std::vector<uint8_t>& out,
                                uint32_t* out_len) {
    Loader& l = loader();
    if (l.create_render_pass == nullptr) {
        return static_cast<uint64_t>(static_cast<int32_t>(VK_ERROR_INITIALIZATION_FAILED));
    }
    vk_wire::Reader r(in.data(), in.size());
    const uint32_t flags = r.u32();

    const uint32_t attachment_count = r.u32();
    std::vector<VkAttachmentDescription> attachments(attachment_count);
    for (auto& a : attachments) {
        a.flags = r.u32();
        a.format = static_cast<VkFormat>(r.u32());
        a.samples = static_cast<VkSampleCountFlagBits>(r.u32());
        a.loadOp = static_cast<VkAttachmentLoadOp>(r.u32());
        a.storeOp = static_cast<VkAttachmentStoreOp>(r.u32());
        a.stencilLoadOp = static_cast<VkAttachmentLoadOp>(r.u32());
        a.stencilStoreOp = static_cast<VkAttachmentStoreOp>(r.u32());
        a.initialLayout = static_cast<VkImageLayout>(r.u32());
        a.finalLayout = static_cast<VkImageLayout>(r.u32());
    }

    // Subpass attachment references are pointer arrays inside each
    // subpass, so they are read into per-subpass storage that outlives
    // the create call.
    const uint32_t subpass_count = r.u32();
    std::vector<VkSubpassDescription> subpasses(subpass_count);
    std::vector<std::vector<VkAttachmentReference>> inputs(subpass_count);
    std::vector<std::vector<VkAttachmentReference>> colors(subpass_count);
    std::vector<std::vector<VkAttachmentReference>> resolves(subpass_count);
    std::vector<VkAttachmentReference> depths(subpass_count);
    std::vector<bool> has_depth(subpass_count, false);
    std::vector<std::vector<uint32_t>> preserves(subpass_count);
    auto read_refs = [&r](std::vector<VkAttachmentReference>& v) {
        const uint32_t n = r.u32();
        v.resize(n);
        for (auto& ref : v) {
            ref.attachment = r.u32();
            ref.layout = static_cast<VkImageLayout>(r.u32());
        }
    };
    for (uint32_t i = 0; i < subpass_count; ++i) {
        subpasses[i].flags = r.u32();
        subpasses[i].pipelineBindPoint = static_cast<VkPipelineBindPoint>(r.u32());
        read_refs(inputs[i]);
        read_refs(colors[i]);
        read_refs(resolves[i]);
        has_depth[i] = r.u32() != 0;
        if (has_depth[i]) {
            depths[i].attachment = r.u32();
            depths[i].layout = static_cast<VkImageLayout>(r.u32());
        }
        const uint32_t np = r.u32();
        preserves[i].resize(np);
        for (auto& v : preserves[i]) v = r.u32();
    }
    for (uint32_t i = 0; i < subpass_count; ++i) {
        subpasses[i].inputAttachmentCount = static_cast<uint32_t>(inputs[i].size());
        subpasses[i].pInputAttachments = inputs[i].empty() ? nullptr : inputs[i].data();
        subpasses[i].colorAttachmentCount = static_cast<uint32_t>(colors[i].size());
        subpasses[i].pColorAttachments = colors[i].empty() ? nullptr : colors[i].data();
        subpasses[i].pResolveAttachments = resolves[i].empty() ? nullptr : resolves[i].data();
        subpasses[i].pDepthStencilAttachment = has_depth[i] ? &depths[i] : nullptr;
        subpasses[i].preserveAttachmentCount = static_cast<uint32_t>(preserves[i].size());
        subpasses[i].pPreserveAttachments = preserves[i].empty() ? nullptr : preserves[i].data();
    }

    const uint32_t dep_count = r.u32();
    std::vector<VkSubpassDependency> deps(dep_count);
    for (auto& d : deps) {
        d.srcSubpass = r.u32();
        d.dstSubpass = r.u32();
        d.srcStageMask = r.u32();
        d.dstStageMask = r.u32();
        d.srcAccessMask = r.u32();
        d.dstAccessMask = r.u32();
        d.dependencyFlags = r.u32();
    }

    VkRenderPassCreateInfo ci{};
    ci.sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO;
    ci.flags = flags;
    ci.attachmentCount = attachment_count;
    ci.pAttachments = attachments.empty() ? nullptr : attachments.data();
    ci.subpassCount = subpass_count;
    ci.pSubpasses = subpasses.empty() ? nullptr : subpasses.data();
    ci.dependencyCount = dep_count;
    ci.pDependencies = deps.empty() ? nullptr : deps.data();

    // STUD_VK_FORCE_LOAD_CLEAR=1 turns every attachment's loadOp into
    // CLEAR. Combined with STUD_VK_FORCE_CLEAR's magenta clear value,
    // this paints through the render pass itself rather than relying on
    // any draw succeeding -- the one way to colour the screen that does
    // not depend on pipelines, descriptors or vertex data being right.
    static const bool force_load_clear = std::getenv("STUD_VK_FORCE_LOAD_CLEAR") != nullptr;
    if (force_load_clear) {
        for (auto& a : attachments) {
            a.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
            a.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        }
    }

    VkRenderPass pass = VK_NULL_HANDLE;
    VkResult res = l.create_render_pass(l.device, &ci, nullptr, &pass);
    if (vk_object_trace_enabled()) {
        std::printf("stud-render-host: DIAG vkCreateRenderPass att=%u sub=%u dep=%u -> %d\n",
                    attachment_count, subpass_count, dep_count, static_cast<int>(res));
    }
    // The per-attachment dump belongs with the line above it: useful
    // while chasing one render pass, ~100 passes' worth of volume in an
    // ordinary session otherwise. A failure prints either way.
    if (vk_object_trace_enabled() || res != VK_SUCCESS) {
        for (uint32_t i = 0; i < attachment_count; ++i) {
            std::printf("    attachment %u: format=%u load=%u store=%u initial=%u final=%u\n", i,
                        static_cast<uint32_t>(attachments[i].format),
                        static_cast<uint32_t>(attachments[i].loadOp),
                        static_cast<uint32_t>(attachments[i].storeOp),
                        static_cast<uint32_t>(attachments[i].initialLayout),
                        static_cast<uint32_t>(attachments[i].finalLayout));
        }
        std::fflush(stdout);
    }
    if (res != VK_SUCCESS) {
        std::printf("stud-render-host: vkCreateRenderPass -> %d\n", static_cast<int>(res));
        std::fflush(stdout);
        return static_cast<uint64_t>(static_cast<int32_t>(res));
    }
    return write_handle(to_u64(pass), out, out_len);
}

uint64_t vk_create_framebuffer(const std::vector<uint8_t>& in, std::vector<uint8_t>& out,
                                uint32_t* out_len) {
    Loader& l = loader();
    if (l.create_framebuffer == nullptr) {
        return static_cast<uint64_t>(static_cast<int32_t>(VK_ERROR_INITIALIZATION_FAILED));
    }
    vk_wire::Reader r(in.data(), in.size());
    VkFramebufferCreateInfo ci{};
    ci.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO;
    ci.flags = r.u32();
    ci.renderPass = from_u64<VkRenderPass>(r.u64());
    const uint32_t n = r.u32();
    std::vector<VkImageView> views(n);
    for (auto& v : views) v = from_u64<VkImageView>(r.u64());
    ci.attachmentCount = n;
    ci.pAttachments = views.empty() ? nullptr : views.data();
    ci.width = r.u32();
    ci.height = r.u32();
    ci.layers = r.u32();

    VkFramebuffer fb = VK_NULL_HANDLE;
    VkResult res = l.create_framebuffer(l.device, &ci, nullptr, &fb);
    if (res != VK_SUCCESS) return static_cast<uint64_t>(static_cast<int32_t>(res));
    bool targets_screen = false;
    for (uint32_t i = 0; i < n; ++i) {
        if (l.swapchain_views.count(to_u64(views[i])) != 0) targets_screen = true;
    }
    if (targets_screen) {
        l.swapchain_framebuffers.insert(to_u64(fb));
        std::printf("stud-render-host: framebuffer %llu targets the swapchain (%ux%u)\n",
                    static_cast<unsigned long long>(to_u64(fb)), ci.width, ci.height);
        std::fflush(stdout);
    }
    return write_handle(to_u64(fb), out, out_len);
}

uint64_t vk_create_sampler(const std::vector<uint8_t>& in, std::vector<uint8_t>& out,
                            uint32_t* out_len) {
    Loader& l = loader();
    if (l.create_sampler == nullptr) {
        return static_cast<uint64_t>(static_cast<int32_t>(VK_ERROR_INITIALIZATION_FAILED));
    }
    vk_wire::Reader r(in.data(), in.size());
    VkSamplerCreateInfo ci{};
    ci.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
    ci.flags = r.u32();
    ci.magFilter = static_cast<VkFilter>(r.u32());
    ci.minFilter = static_cast<VkFilter>(r.u32());
    ci.mipmapMode = static_cast<VkSamplerMipmapMode>(r.u32());
    ci.addressModeU = static_cast<VkSamplerAddressMode>(r.u32());
    ci.addressModeV = static_cast<VkSamplerAddressMode>(r.u32());
    ci.addressModeW = static_cast<VkSamplerAddressMode>(r.u32());
    ci.mipLodBias = r.f32();
    ci.anisotropyEnable = r.u32();
    ci.maxAnisotropy = r.f32();
    ci.compareEnable = r.u32();
    ci.compareOp = static_cast<VkCompareOp>(r.u32());
    ci.minLod = r.f32();
    ci.maxLod = r.f32();
    ci.borderColor = static_cast<VkBorderColor>(r.u32());
    ci.unnormalizedCoordinates = r.u32();

    VkSampler sampler = VK_NULL_HANDLE;
    VkResult res = l.create_sampler(l.device, &ci, nullptr, &sampler);
    if (res != VK_SUCCESS) return static_cast<uint64_t>(static_cast<int32_t>(res));
    return write_handle(to_u64(sampler), out, out_len);
}

uint64_t vk_create_descriptor_set_layout(const std::vector<uint8_t>& in,
                                          std::vector<uint8_t>& out, uint32_t* out_len) {
    Loader& l = loader();
    if (l.create_descriptor_set_layout == nullptr) {
        return static_cast<uint64_t>(static_cast<int32_t>(VK_ERROR_INITIALIZATION_FAILED));
    }
    vk_wire::Reader r(in.data(), in.size());
    VkDescriptorSetLayoutCreateInfo ci{};
    ci.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    ci.flags = r.u32();
    const uint32_t n = r.u32();
    std::vector<VkDescriptorSetLayoutBinding> bindings(n);
    std::vector<std::vector<VkSampler>> immutable(n);
    for (uint32_t i = 0; i < n; ++i) {
        bindings[i].binding = r.u32();
        bindings[i].descriptorType = static_cast<VkDescriptorType>(r.u32());
        bindings[i].descriptorCount = r.u32();
        bindings[i].stageFlags = r.u32();
        const uint32_t ns = r.u32();
        immutable[i].resize(ns);
        for (auto& sm : immutable[i]) sm = from_u64<VkSampler>(r.u64());
    }
    for (uint32_t i = 0; i < n; ++i) {
        bindings[i].pImmutableSamplers = immutable[i].empty() ? nullptr : immutable[i].data();
    }
    ci.bindingCount = n;
    ci.pBindings = bindings.empty() ? nullptr : bindings.data();

    VkDescriptorSetLayout layout = VK_NULL_HANDLE;
    VkResult res = l.create_descriptor_set_layout(l.device, &ci, nullptr, &layout);
    if (res != VK_SUCCESS) return static_cast<uint64_t>(static_cast<int32_t>(res));
    return write_handle(to_u64(layout), out, out_len);
}

uint64_t vk_create_pipeline_layout(const std::vector<uint8_t>& in, std::vector<uint8_t>& out,
                                    uint32_t* out_len) {
    Loader& l = loader();
    if (l.create_pipeline_layout == nullptr) {
        return static_cast<uint64_t>(static_cast<int32_t>(VK_ERROR_INITIALIZATION_FAILED));
    }
    vk_wire::Reader r(in.data(), in.size());
    VkPipelineLayoutCreateInfo ci{};
    ci.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    ci.flags = r.u32();
    const uint32_t nl = r.u32();
    std::vector<VkDescriptorSetLayout> layouts(nl);
    for (auto& v : layouts) v = from_u64<VkDescriptorSetLayout>(r.u64());
    const uint32_t nr = r.u32();
    std::vector<VkPushConstantRange> ranges(nr);
    for (auto& pr : ranges) {
        pr.stageFlags = r.u32();
        pr.offset = r.u32();
        pr.size = r.u32();
    }
    ci.setLayoutCount = nl;
    ci.pSetLayouts = layouts.empty() ? nullptr : layouts.data();
    ci.pushConstantRangeCount = nr;
    ci.pPushConstantRanges = ranges.empty() ? nullptr : ranges.data();

    VkPipelineLayout layout = VK_NULL_HANDLE;
    VkResult res = l.create_pipeline_layout(l.device, &ci, nullptr, &layout);
    if (res != VK_SUCCESS) return static_cast<uint64_t>(static_cast<int32_t>(res));
    return write_handle(to_u64(layout), out, out_len);
}

uint64_t vk_create_descriptor_pool(const std::vector<uint8_t>& in, std::vector<uint8_t>& out,
                                    uint32_t* out_len) {
    Loader& l = loader();
    if (l.create_descriptor_pool == nullptr) {
        return static_cast<uint64_t>(static_cast<int32_t>(VK_ERROR_INITIALIZATION_FAILED));
    }
    vk_wire::Reader r(in.data(), in.size());
    VkDescriptorPoolCreateInfo ci{};
    ci.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    ci.flags = r.u32();
    ci.maxSets = r.u32();
    const uint32_t n = r.u32();
    std::vector<VkDescriptorPoolSize> sizes(n);
    for (auto& sz : sizes) {
        sz.type = static_cast<VkDescriptorType>(r.u32());
        sz.descriptorCount = r.u32();
    }
    ci.poolSizeCount = n;
    ci.pPoolSizes = sizes.empty() ? nullptr : sizes.data();

    VkDescriptorPool pool = VK_NULL_HANDLE;
    VkResult res = l.create_descriptor_pool(l.device, &ci, nullptr, &pool);
    if (res != VK_SUCCESS) return static_cast<uint64_t>(static_cast<int32_t>(res));
    return write_handle(to_u64(pool), out, out_len);
}

// Descriptor sets are named by ids the CLIENT invents, not by the real
// handles this process gets back.
//
// The engine allocates descriptor sets constantly -- measured at 738 calls
// a frame in a real game -- and a handle that has to be returned makes
// every one of those a synchronous round trip. Together with the template
// update that follows each, they were 90% of all IPC round trips and 20ms
// of a 33ms frame, which is most of the gap against a client that talks to
// the driver in-process. With the client naming them, the call carries no
// answer and never blocks.
//
// The ids are tagged so they can never be confused with a real handle:
// nothing else in this process hands out pointers in that range, so a
// lookup miss is a bug rather than an ambiguity.
VkDescriptorSet resolve_descriptor_set(uint64_t id) {
    std::lock_guard<std::mutex> lock(descriptor_set_mutex());
    auto it = descriptor_sets().find(id);
    if (it != descriptor_sets().end()) return it->second.set;
    static bool warned = false;
    if (!warned) {
        warned = true;
        std::fprintf(stderr,
                     "stud-render-host: a descriptor set id was used before it was allocated "
                     "(0x%llx)\n",
                     static_cast<unsigned long long>(id));
    }
    return VK_NULL_HANDLE;
}

uint64_t vk_allocate_descriptor_sets(const std::vector<uint8_t>& in, std::vector<uint8_t>& out,
                                      uint32_t* out_len) {
    Loader& l = loader();
    if (l.allocate_descriptor_sets == nullptr) {
        return static_cast<uint64_t>(static_cast<int32_t>(VK_ERROR_INITIALIZATION_FAILED));
    }
    vk_wire::Reader r(in.data(), in.size());
    VkDescriptorSetAllocateInfo ai{};
    ai.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
    ai.descriptorPool = from_u64<VkDescriptorPool>(r.u64());
    const uint32_t n = r.u32();
    std::vector<VkDescriptorSetLayout> layouts(n);
    for (auto& v : layouts) v = from_u64<VkDescriptorSetLayout>(r.u64());
    ai.descriptorSetCount = n;
    ai.pSetLayouts = layouts.empty() ? nullptr : layouts.data();

    // The ids the client has already handed to the engine for these sets.
    std::vector<uint64_t> ids(n);
    for (auto& id : ids) id = r.u64();

    std::vector<VkDescriptorSet> sets(n);
    VkResult res = l.allocate_descriptor_sets(l.device, &ai, sets.data());
    if (res != VK_SUCCESS) return static_cast<uint64_t>(static_cast<int32_t>(res));
    {
        std::lock_guard<std::mutex> lock(descriptor_set_mutex());
        for (uint32_t i = 0; i < n; ++i) {
            descriptor_sets()[ids[i]] = OwnedDescriptorSet{sets[i], to_u64(ai.descriptorPool)};
        }
    }
    (void)out;
    *out_len = 0;
    return static_cast<uint64_t>(static_cast<int32_t>(VK_SUCCESS));
}

uint64_t vk_reset_descriptor_pool(uint64_t pool, uint32_t flags) {
    Loader& l = loader();
    if (l.reset_descriptor_pool == nullptr) {
        return static_cast<uint64_t>(static_cast<int32_t>(VK_ERROR_INITIALIZATION_FAILED));
    }
    {
        // Every set allocated from this pool is destroyed by the reset, so
        // its id must stop resolving -- otherwise a later use would hand
        // the driver a freed handle.
        std::lock_guard<std::mutex> lock(descriptor_set_mutex());
        auto& sets = descriptor_sets();
        for (auto it = sets.begin(); it != sets.end();) {
            it = it->second.pool == pool ? sets.erase(it) : std::next(it);
        }
    }
    return static_cast<uint64_t>(static_cast<int32_t>(
        l.reset_descriptor_pool(l.device, from_u64<VkDescriptorPool>(pool), flags)));
}

uint64_t vk_create_descriptor_update_template(const std::vector<uint8_t>& in,
                                               std::vector<uint8_t>& out, uint32_t* out_len) {
    Loader& l = loader();
    if (l.create_descriptor_update_template == nullptr) {
        return static_cast<uint64_t>(static_cast<int32_t>(VK_ERROR_INITIALIZATION_FAILED));
    }
    vk_wire::Reader r(in.data(), in.size());
    VkDescriptorUpdateTemplateCreateInfo ci{};
    ci.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_UPDATE_TEMPLATE_CREATE_INFO;
    ci.flags = r.u32();
    const uint32_t n = r.u32();
    std::vector<VkDescriptorUpdateTemplateEntry> entries(n);
    for (auto& e : entries) {
        e.dstBinding = r.u32();
        e.dstArrayElement = r.u32();
        e.descriptorCount = r.u32();
        e.descriptorType = static_cast<VkDescriptorType>(r.u32());
        e.offset = r.u64();
        e.stride = r.u64();
    }
    ci.descriptorUpdateEntryCount = n;
    ci.pDescriptorUpdateEntries = entries.empty() ? nullptr : entries.data();
    ci.templateType = static_cast<VkDescriptorUpdateTemplateType>(r.u32());
    ci.descriptorSetLayout = from_u64<VkDescriptorSetLayout>(r.u64());
    ci.pipelineBindPoint = static_cast<VkPipelineBindPoint>(r.u32());
    ci.pipelineLayout = from_u64<VkPipelineLayout>(r.u64());
    ci.set = r.u32();

    VkDescriptorUpdateTemplate tmpl = VK_NULL_HANDLE;
    VkResult res = l.create_descriptor_update_template(l.device, &ci, nullptr, &tmpl);
    if (res != VK_SUCCESS) return static_cast<uint64_t>(static_cast<int32_t>(res));

    // Remember the layout: a template update's payload is an opaque blob
    // whose meaning is defined entirely by these entries, and the handles
    // inside it are Process B's copies of host handles. They have to be
    // translated on the way through, which needs the entry list.
    l.template_entries[to_u64(tmpl)] = std::move(entries);
    return write_handle(to_u64(tmpl), out, out_len);
}

uint64_t vk_update_descriptor_set_with_template(uint64_t set, uint64_t tmpl,
                                                 const std::vector<uint8_t>& in) {
    Loader& l = loader();
    if (l.update_descriptor_set_with_template == nullptr) {
        return static_cast<uint64_t>(static_cast<int32_t>(VK_ERROR_INITIALIZATION_FAILED));
    }
    // The blob is laid out by the template's own entries, and every
    // descriptor in it carries handles. Those are already host handles --
    // Process B never invents one, it only ever echoes back what this
    // process gave it -- so the blob passes through unchanged.
    // What the template says it needs versus what arrived. A blob that is
    // short leaves the driver reading whatever follows it -- descriptors
    // that point nowhere, and draws that sample nothing.
    static const bool trace = std::getenv("STUD_VK_TRACE_DESC") != nullptr;
    if (trace) {
        static int reported = 0;
        auto it = l.template_entries.find(tmpl);
        if (reported < 12 && it != l.template_entries.end()) {
            uint64_t need = 0;
            for (const auto& e : it->second) {
                if (e.descriptorCount == 0) continue;
                const uint64_t end = e.offset + static_cast<uint64_t>(e.stride) *
                                                     (e.descriptorCount - 1) + 24;
                if (end > need) need = end;
            }
            std::printf("stud-render-host: DESC tmpl=%llu entries=%zu need>=%llu got=%zu%s\n",
                        static_cast<unsigned long long>(tmpl), it->second.size(),
                        static_cast<unsigned long long>(need), in.size(),
                        in.size() < need ? "   <-- SHORT" : "");
            std::fflush(stdout);
            ++reported;
        }
    }
    std::vector<uint8_t> data(in.begin(), in.end());
    l.update_descriptor_set_with_template(l.device, resolve_descriptor_set(set),
                                           from_u64<VkDescriptorUpdateTemplate>(tmpl),
                                           data.data());
    return static_cast<uint64_t>(static_cast<int32_t>(VK_SUCCESS));
}

uint64_t vk_create_graphics_pipelines(const std::vector<uint8_t>& in, std::vector<uint8_t>& out,
                                       uint32_t* out_len) {
    Loader& l = loader();
    if (l.create_graphics_pipelines == nullptr) {
        return static_cast<uint64_t>(static_cast<int32_t>(VK_ERROR_INITIALIZATION_FAILED));
    }
    vk_wire::Reader r(in.data(), in.size());
    const uint64_t cache = r.u64();

    // One pipeline per call. The engine batches rarely and a batch would
    // multiply the storage juggling below for no measured benefit; if a
    // real capture ever shows batching, this is where it goes.
    VkGraphicsPipelineCreateInfo ci{};
    ci.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
    ci.flags = r.u32();

    const uint32_t stage_count = r.u32();
    std::vector<VkPipelineShaderStageCreateInfo> stages(stage_count);
    std::vector<std::string> stage_names(stage_count);
    for (uint32_t i = 0; i < stage_count; ++i) {
        stages[i].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
        stages[i].flags = r.u32();
        stages[i].stage = static_cast<VkShaderStageFlagBits>(r.u32());
        stages[i].module = from_u64<VkShaderModule>(r.u64());
        uint32_t nlen = 0;
        const uint8_t* nb = r.bytes(&nlen);
        stage_names[i].assign(reinterpret_cast<const char*>(nb), nlen);
    }
    for (uint32_t i = 0; i < stage_count; ++i) stages[i].pName = stage_names[i].c_str();

    VkPipelineVertexInputStateCreateInfo vi{};
    vi.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;
    const uint32_t nbind = r.u32();
    std::vector<VkVertexInputBindingDescription> binds(nbind);
    for (auto& b : binds) {
        b.binding = r.u32();
        b.stride = r.u32();
        b.inputRate = static_cast<VkVertexInputRate>(r.u32());
    }
    const uint32_t nattr = r.u32();
    std::vector<VkVertexInputAttributeDescription> attrs(nattr);
    for (auto& a : attrs) {
        a.location = r.u32();
        a.binding = r.u32();
        a.format = static_cast<VkFormat>(r.u32());
        a.offset = r.u32();
    }
    vi.vertexBindingDescriptionCount = nbind;
    vi.pVertexBindingDescriptions = binds.empty() ? nullptr : binds.data();
    vi.vertexAttributeDescriptionCount = nattr;
    vi.pVertexAttributeDescriptions = attrs.empty() ? nullptr : attrs.data();

    VkPipelineInputAssemblyStateCreateInfo ia{};
    ia.sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO;
    ia.topology = static_cast<VkPrimitiveTopology>(r.u32());
    ia.primitiveRestartEnable = r.u32();

    VkPipelineViewportStateCreateInfo vp{};
    vp.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO;
    vp.viewportCount = r.u32();
    const uint32_t sent_viewports = r.u32();
    std::vector<VkViewport> viewports(sent_viewports);
    for (auto& v : viewports) {
        v.x = r.f32();
        v.y = r.f32();
        v.width = r.f32();
        v.height = r.f32();
        v.minDepth = r.f32();
        v.maxDepth = r.f32();
    }
    vp.scissorCount = r.u32();
    const uint32_t sent_scissors = r.u32();
    std::vector<VkRect2D> scissors(sent_scissors);
    for (auto& sc : scissors) {
        sc.offset.x = static_cast<int32_t>(r.u32());
        sc.offset.y = static_cast<int32_t>(r.u32());
        sc.extent.width = r.u32();
        sc.extent.height = r.u32();
    }
    // Null when the pipeline uses dynamic viewport/scissor, which is what
    // the client signals by sending zero of them.
    vp.pViewports = viewports.empty() ? nullptr : viewports.data();
    vp.pScissors = scissors.empty() ? nullptr : scissors.data();

    VkPipelineRasterizationStateCreateInfo rs{};
    rs.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;
    rs.depthClampEnable = r.u32();
    rs.rasterizerDiscardEnable = r.u32();
    rs.polygonMode = static_cast<VkPolygonMode>(r.u32());
    rs.cullMode = r.u32();
    rs.frontFace = static_cast<VkFrontFace>(r.u32());
    rs.depthBiasEnable = r.u32();
    rs.depthBiasConstantFactor = r.f32();
    rs.depthBiasClamp = r.f32();
    rs.depthBiasSlopeFactor = r.f32();
    rs.lineWidth = r.f32();

    VkPipelineMultisampleStateCreateInfo ms{};
    ms.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
    ms.rasterizationSamples = static_cast<VkSampleCountFlagBits>(r.u32());
    ms.sampleShadingEnable = r.u32();
    ms.minSampleShading = r.f32();
    const uint32_t has_mask = r.u32();
    VkSampleMask sample_mask = r.u32();
    ms.pSampleMask = has_mask != 0 ? &sample_mask : nullptr;
    ms.alphaToCoverageEnable = r.u32();
    ms.alphaToOneEnable = r.u32();

    VkPipelineDepthStencilStateCreateInfo ds{};
    ds.sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO;
    const uint32_t has_ds = r.u32();
    if (has_ds != 0) {
        ds.depthTestEnable = r.u32();
        ds.depthWriteEnable = r.u32();
        ds.depthCompareOp = static_cast<VkCompareOp>(r.u32());
        ds.depthBoundsTestEnable = r.u32();
        ds.stencilTestEnable = r.u32();
        auto read_stencil = [&r](VkStencilOpState& s) {
            s.failOp = static_cast<VkStencilOp>(r.u32());
            s.passOp = static_cast<VkStencilOp>(r.u32());
            s.depthFailOp = static_cast<VkStencilOp>(r.u32());
            s.compareOp = static_cast<VkCompareOp>(r.u32());
            s.compareMask = r.u32();
            s.writeMask = r.u32();
            s.reference = r.u32();
        };
        read_stencil(ds.front);
        read_stencil(ds.back);
        ds.minDepthBounds = r.f32();
        ds.maxDepthBounds = r.f32();
    }

    VkPipelineColorBlendStateCreateInfo cb{};
    cb.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
    const uint32_t has_cb = r.u32();
    std::vector<VkPipelineColorBlendAttachmentState> blends;
    if (has_cb != 0) {
        cb.logicOpEnable = r.u32();
        cb.logicOp = static_cast<VkLogicOp>(r.u32());
        const uint32_t nb = r.u32();
        blends.resize(nb);
        for (auto& b : blends) {
            b.blendEnable = r.u32();
            b.srcColorBlendFactor = static_cast<VkBlendFactor>(r.u32());
            b.dstColorBlendFactor = static_cast<VkBlendFactor>(r.u32());
            b.colorBlendOp = static_cast<VkBlendOp>(r.u32());
            b.srcAlphaBlendFactor = static_cast<VkBlendFactor>(r.u32());
            b.dstAlphaBlendFactor = static_cast<VkBlendFactor>(r.u32());
            b.alphaBlendOp = static_cast<VkBlendOp>(r.u32());
            b.colorWriteMask = r.u32();
        }
        cb.attachmentCount = nb;
        cb.pAttachments = blends.empty() ? nullptr : blends.data();
        for (int i = 0; i < 4; ++i) cb.blendConstants[i] = r.f32();
    }

    VkPipelineDynamicStateCreateInfo dyn{};
    dyn.sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO;
    const uint32_t ndyn = r.u32();
    std::vector<VkDynamicState> dyns(ndyn);
    for (auto& d : dyns) d = static_cast<VkDynamicState>(r.u32());
    dyn.dynamicStateCount = ndyn;
    dyn.pDynamicStates = dyns.empty() ? nullptr : dyns.data();

    ci.stageCount = stage_count;
    ci.pStages = stages.empty() ? nullptr : stages.data();
    ci.pVertexInputState = &vi;
    ci.pInputAssemblyState = &ia;
    ci.pViewportState = &vp;
    ci.pRasterizationState = &rs;
    ci.pMultisampleState = &ms;
    ci.pDepthStencilState = has_ds != 0 ? &ds : nullptr;
    ci.pColorBlendState = has_cb != 0 ? &cb : nullptr;
    ci.pDynamicState = ndyn > 0 ? &dyn : nullptr;
    ci.layout = from_u64<VkPipelineLayout>(r.u64());
    ci.renderPass = from_u64<VkRenderPass>(r.u64());
    ci.subpass = r.u32();

    VkPipeline pipeline = VK_NULL_HANDLE;
    VkResult res = l.create_graphics_pipelines(l.device, from_u64<VkPipelineCache>(cache), 1, &ci,
                                                nullptr, &pipeline);
    if (res != VK_SUCCESS) {
        std::printf("stud-render-host: vkCreateGraphicsPipelines -> %d\n", static_cast<int>(res));
        std::fflush(stdout);
        return static_cast<uint64_t>(static_cast<int32_t>(res));
    }
    return write_handle(to_u64(pipeline), out, out_len);
}

uint64_t vk_create_compute_pipelines(const std::vector<uint8_t>& in, std::vector<uint8_t>& out,
                                      uint32_t* out_len) {
    Loader& l = loader();
    if (l.create_compute_pipelines == nullptr) {
        return static_cast<uint64_t>(static_cast<int32_t>(VK_ERROR_INITIALIZATION_FAILED));
    }
    vk_wire::Reader r(in.data(), in.size());
    const uint64_t cache = r.u64();
    VkComputePipelineCreateInfo ci{};
    ci.sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO;
    ci.flags = r.u32();
    ci.stage.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    ci.stage.flags = r.u32();
    ci.stage.stage = static_cast<VkShaderStageFlagBits>(r.u32());
    ci.stage.module = from_u64<VkShaderModule>(r.u64());
    uint32_t nlen = 0;
    const uint8_t* nb = r.bytes(&nlen);
    std::string name(reinterpret_cast<const char*>(nb), nlen);
    ci.stage.pName = name.c_str();
    ci.layout = from_u64<VkPipelineLayout>(r.u64());

    VkPipeline pipeline = VK_NULL_HANDLE;
    VkResult res = l.create_compute_pipelines(l.device, from_u64<VkPipelineCache>(cache), 1, &ci,
                                               nullptr, &pipeline);
    if (res != VK_SUCCESS) return static_cast<uint64_t>(static_cast<int32_t>(res));
    return write_handle(to_u64(pipeline), out, out_len);
}

// ---- command buffers and submission ----------------------------------

uint64_t vk_allocate_command_buffers(uint64_t pool, uint32_t level, uint32_t count,
                                      std::vector<uint8_t>& out, uint32_t* out_len) {
    Loader& l = loader();
    if (l.allocate_command_buffers == nullptr) {
        return static_cast<uint64_t>(static_cast<int32_t>(VK_ERROR_INITIALIZATION_FAILED));
    }
    VkCommandBufferAllocateInfo ai{};
    ai.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    ai.commandPool = from_u64<VkCommandPool>(pool);
    ai.level = static_cast<VkCommandBufferLevel>(level);
    ai.commandBufferCount = count;
    std::vector<VkCommandBuffer> buffers(count);
    VkResult r = l.allocate_command_buffers(l.device, &ai, buffers.data());
    if (r != VK_SUCCESS) return static_cast<uint64_t>(static_cast<int32_t>(r));
    out.resize(sizeof(uint64_t) * count);
    for (uint32_t i = 0; i < count; ++i) {
        uint64_t h = to_u64(buffers[i]);
        std::memcpy(out.data() + i * sizeof(h), &h, sizeof(h));
    }
    *out_len = static_cast<uint32_t>(out.size());
    return static_cast<uint64_t>(static_cast<int32_t>(VK_SUCCESS));
}

uint64_t vk_begin_command_buffer(uint64_t cb, uint32_t flags) {
    Loader& l = loader();
    if (l.begin_command_buffer == nullptr) {
        return static_cast<uint64_t>(static_cast<int32_t>(VK_ERROR_INITIALIZATION_FAILED));
    }
    VkCommandBufferBeginInfo bi{};
    bi.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    bi.flags = flags;
    return static_cast<uint64_t>(
        static_cast<int32_t>(l.begin_command_buffer(from_u64<VkCommandBuffer>(cb), &bi)));
}

uint64_t vk_end_command_buffer(uint64_t cb) {
    Loader& l = loader();
    if (l.end_command_buffer == nullptr) {
        return static_cast<uint64_t>(static_cast<int32_t>(VK_ERROR_INITIALIZATION_FAILED));
    }
    return static_cast<uint64_t>(
        static_cast<int32_t>(l.end_command_buffer(from_u64<VkCommandBuffer>(cb))));
}

uint64_t vk_reset_command_pool(uint64_t pool, uint32_t flags) {
    Loader& l = loader();
    if (l.reset_command_pool == nullptr) {
        return static_cast<uint64_t>(static_cast<int32_t>(VK_ERROR_INITIALIZATION_FAILED));
    }
    return static_cast<uint64_t>(static_cast<int32_t>(
        l.reset_command_pool(l.device, from_u64<VkCommandPool>(pool), flags)));
}

uint64_t vk_queue_submit(uint64_t queue, uint64_t fence, const std::vector<uint8_t>& in) {
    Loader& l = loader();
    if (l.queue_submit == nullptr) {
        return static_cast<uint64_t>(static_cast<int32_t>(VK_ERROR_INITIALIZATION_FAILED));
    }
    vk_wire::Reader r(in.data(), in.size());
    const uint32_t n = r.u32();
    std::vector<VkSubmitInfo> submits(n);
    std::vector<std::vector<VkSemaphore>> waits(n), signals(n);
    std::vector<std::vector<VkPipelineStageFlags>> stages(n);
    std::vector<std::vector<VkCommandBuffer>> buffers(n);
    for (uint32_t i = 0; i < n; ++i) {
        const uint32_t nw = r.u32();
        waits[i].resize(nw);
        stages[i].resize(nw);
        for (uint32_t j = 0; j < nw; ++j) waits[i][j] = from_u64<VkSemaphore>(r.u64());
        for (uint32_t j = 0; j < nw; ++j) stages[i][j] = r.u32();
        const uint32_t nc = r.u32();
        buffers[i].resize(nc);
        for (uint32_t j = 0; j < nc; ++j) buffers[i][j] = from_u64<VkCommandBuffer>(r.u64());
        const uint32_t ns = r.u32();
        signals[i].resize(ns);
        for (uint32_t j = 0; j < ns; ++j) signals[i][j] = from_u64<VkSemaphore>(r.u64());
    }
    for (uint32_t i = 0; i < n; ++i) {
        submits[i].sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
        submits[i].waitSemaphoreCount = static_cast<uint32_t>(waits[i].size());
        submits[i].pWaitSemaphores = waits[i].empty() ? nullptr : waits[i].data();
        submits[i].pWaitDstStageMask = stages[i].empty() ? nullptr : stages[i].data();
        submits[i].commandBufferCount = static_cast<uint32_t>(buffers[i].size());
        submits[i].pCommandBuffers = buffers[i].empty() ? nullptr : buffers[i].data();
        submits[i].signalSemaphoreCount = static_cast<uint32_t>(signals[i].size());
        submits[i].pSignalSemaphores = signals[i].empty() ? nullptr : signals[i].data();
    }
    VkResult res = l.queue_submit(from_u64<VkQueue>(queue), n, submits.empty() ? nullptr : submits.data(),
                                   from_u64<VkFence>(fence));
    return static_cast<uint64_t>(static_cast<int32_t>(res));
}

uint64_t vk_wait_for_fences(const std::vector<uint8_t>& in, uint32_t wait_all, uint64_t timeout) {
    Loader& l = loader();
    if (l.wait_for_fences == nullptr) {
        return static_cast<uint64_t>(static_cast<int32_t>(VK_ERROR_INITIALIZATION_FAILED));
    }
    vk_wire::Reader r(in.data(), in.size());
    const uint32_t n = r.u32();
    std::vector<VkFence> fences(n);
    for (auto& f : fences) f = from_u64<VkFence>(r.u64());
    const auto t0 = std::chrono::steady_clock::now();
    VkResult res = l.wait_for_fences(l.device, n, fences.empty() ? nullptr : fences.data(),
                                      wait_all, timeout);
    const double ms = std::chrono::duration<double, std::milli>(
                          std::chrono::steady_clock::now() - t0).count();
    if (ms > 50.0) {
        std::printf("stud-render-host: SLOW vkWaitForFences %.1fms (n=%u timeout=%llu) -> %d\n", ms,
                    n, static_cast<unsigned long long>(timeout), static_cast<int>(res));
        std::fflush(stdout);
    }
    return static_cast<uint64_t>(static_cast<int32_t>(res));
}

uint64_t vk_reset_fences(const std::vector<uint8_t>& in) {
    Loader& l = loader();
    if (l.reset_fences == nullptr) {
        return static_cast<uint64_t>(static_cast<int32_t>(VK_ERROR_INITIALIZATION_FAILED));
    }
    vk_wire::Reader r(in.data(), in.size());
    const uint32_t n = r.u32();
    std::vector<VkFence> fences(n);
    for (auto& f : fences) f = from_u64<VkFence>(r.u64());
    VkResult res = l.reset_fences(l.device, n, fences.empty() ? nullptr : fences.data());
    return static_cast<uint64_t>(static_cast<int32_t>(res));
}

uint64_t vk_acquire_next_image(uint64_t swapchain, uint64_t timeout, uint64_t semaphore,
                                uint64_t fence, std::vector<uint8_t>& out, uint32_t* out_len) {
    Loader& l = loader();
    if (l.acquire_next_image == nullptr) {
        return static_cast<uint64_t>(static_cast<int32_t>(VK_ERROR_INITIALIZATION_FAILED));
    }
    uint32_t index = 0;
    static int acquires = 0;
    if (acquires < 5) {
        std::printf("stud-render-host: vkAcquireNextImageKHR #%d\n", acquires);
        std::fflush(stdout);
    }
    ++acquires;
    const auto t0 = std::chrono::steady_clock::now();
    VkResult r = l.acquire_next_image(l.device, from_u64<VkSwapchainKHR>(swapchain), timeout,
                                       from_u64<VkSemaphore>(semaphore), from_u64<VkFence>(fence),
                                       &index);
    const double ms = std::chrono::duration<double, std::milli>(
                          std::chrono::steady_clock::now() - t0).count();
    if (ms > 50.0) {
        std::printf("stud-render-host: SLOW vkAcquireNextImageKHR %.1fms -> %d\n", ms,
                    static_cast<int>(r));
        std::fflush(stdout);
    }
    out.resize(sizeof(index));
    std::memcpy(out.data(), &index, sizeof(index));
    *out_len = sizeof(index);
    return static_cast<uint64_t>(static_cast<int32_t>(r));
}


// Reads one swapchain image back and reports whether it is all black.
// Uses its own command buffer and a full queue wait -- slow and only ever
// run a few times behind STUD_VK_PROBE_PIXELS, since the question it
// answers is worth a stall.
void probe_swapchain_pixels(VkSwapchainKHR swapchain, uint32_t index) {
    Loader& l = loader();
    if (l.get_swapchain_images == nullptr || l.create_buffer == nullptr ||
        l.allocate_memory == nullptr || l.map_memory == nullptr ||
        l.allocate_command_buffers == nullptr || l.queue_submit == nullptr) {
        return;
    }
    uint32_t count = 0;
    l.get_swapchain_images(l.device, swapchain, &count, nullptr);
    std::vector<VkImage> images(count);
    l.get_swapchain_images(l.device, swapchain, &count, images.data());
    if (index >= count) return;

    const uint32_t w = g_window_width.load(std::memory_order_relaxed);
    const uint32_t h = g_window_height.load(std::memory_order_relaxed);
    const VkDeviceSize size = static_cast<VkDeviceSize>(w) * h * 4;

    VkBufferCreateInfo bci{};
    bci.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    bci.size = size;
    bci.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT;
    VkBuffer buffer = VK_NULL_HANDLE;
    if (l.create_buffer(l.device, &bci, nullptr, &buffer) != VK_SUCCESS) return;

    VkMemoryRequirements req{};
    l.get_buffer_memory_requirements(l.device, buffer, &req);
    VkPhysicalDeviceMemoryProperties mem{};
    l.get_physical_device_memory_properties(l.physical_device, &mem);
    uint32_t type = UINT32_MAX;
    for (uint32_t i = 0; i < mem.memoryTypeCount; ++i) {
        const bool usable = (req.memoryTypeBits & (1u << i)) != 0;
        const bool visible = (mem.memoryTypes[i].propertyFlags &
                              VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT) != 0 &&
                             (mem.memoryTypes[i].propertyFlags &
                              VK_MEMORY_PROPERTY_HOST_COHERENT_BIT) != 0;
        if (usable && visible) { type = i; break; }
    }
    if (type == UINT32_MAX) return;

    VkMemoryAllocateInfo mai{};
    mai.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    mai.allocationSize = req.size;
    mai.memoryTypeIndex = type;
    VkDeviceMemory memory = VK_NULL_HANDLE;
    if (l.allocate_memory(l.device, &mai, nullptr, &memory) != VK_SUCCESS) return;
    l.bind_buffer_memory(l.device, buffer, memory, 0);

    VkCommandBufferAllocateInfo cbai{};
    cbai.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    cbai.commandPool = l.probe_pool;
    cbai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    cbai.commandBufferCount = 1;
    VkCommandBuffer cb = VK_NULL_HANDLE;
    if (l.probe_pool == VK_NULL_HANDLE ||
        l.allocate_command_buffers(l.device, &cbai, &cb) != VK_SUCCESS) {
        return;
    }

    VkCommandBufferBeginInfo bi{};
    bi.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    l.begin_command_buffer(cb, &bi);
    VkImageMemoryBarrier to_src{};
    to_src.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    to_src.oldLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
    to_src.newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    to_src.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    to_src.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    to_src.image = images[index];
    to_src.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    to_src.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
    l.cmd_pipeline_barrier(cb, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                            VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0, nullptr, 1, &to_src);
    VkBufferImageCopy region{};
    region.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    region.imageExtent = {w, h, 1};
    l.cmd_copy_image_to_buffer(cb, images[index], VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, buffer, 1,
                                &region);
    VkImageMemoryBarrier back = to_src;
    back.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    back.newLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
    back.srcAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
    back.dstAccessMask = 0;
    l.cmd_pipeline_barrier(cb, VK_PIPELINE_STAGE_TRANSFER_BIT,
                            VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, 0, 0, nullptr, 0, nullptr, 1,
                            &back);
    l.end_command_buffer(cb);

    VkSubmitInfo si{};
    si.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    si.commandBufferCount = 1;
    si.pCommandBuffers = &cb;
    l.queue_submit(l.probe_queue, 1, &si, VK_NULL_HANDLE);
    if (l.queue_wait_idle != nullptr) l.queue_wait_idle(l.probe_queue);

    void* data = nullptr;
    if (l.map_memory(l.device, memory, 0, size, 0, &data) == VK_SUCCESS && data != nullptr) {
        const auto* px = static_cast<const uint8_t*>(data);
        size_t nonzero = 0;
        for (VkDeviceSize i = 0; i + 3 < size; i += 4) {
            if (px[i] != 0 || px[i + 1] != 0 || px[i + 2] != 0) ++nonzero;
        }
        std::printf("stud-render-host: PIXELPROBE image %u: %zu non-black of %llu\n", index,
                    nonzero, static_cast<unsigned long long>(size / 4));
        std::fflush(stdout);
        l.unmap_memory(l.device, memory);
    }
    l.free_memory(l.device, memory, nullptr);
    l.destroy_buffer(l.device, buffer, nullptr);
}

uint64_t vk_queue_present(uint64_t queue, const std::vector<uint8_t>& in) {
    Loader& l = loader();
    if (l.queue_present == nullptr) {
        return static_cast<uint64_t>(static_cast<int32_t>(VK_ERROR_INITIALIZATION_FAILED));
    }
    vk_wire::Reader r(in.data(), in.size());
    const uint32_t nw = r.u32();
    std::vector<VkSemaphore> waits(nw);
    for (auto& w : waits) w = from_u64<VkSemaphore>(r.u64());
    const uint32_t ns = r.u32();
    std::vector<VkSwapchainKHR> chains(ns);
    std::vector<uint32_t> indices(ns);
    for (auto& c : chains) c = from_u64<VkSwapchainKHR>(r.u64());
    for (auto& i : indices) i = r.u32();

    VkPresentInfoKHR pi{};
    pi.sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR;
    pi.waitSemaphoreCount = nw;
    pi.pWaitSemaphores = waits.empty() ? nullptr : waits.data();
    pi.swapchainCount = ns;
    pi.pSwapchains = chains.empty() ? nullptr : chains.data();
    pi.pImageIndices = indices.empty() ? nullptr : indices.data();
    // STUD_VK_PROBE_PIXELS=1: before presenting, copy the swapchain image
    // to a host-visible buffer and report whether it holds anything but
    // black. This is the difference between "the engine renders nothing"
    // and "it renders and the compositor never shows it" -- the two
    // halves of a black window, which nothing upstream of here can tell
    // apart.
    static const bool probe = std::getenv("STUD_VK_PROBE_PIXELS") != nullptr;
    static int probed = 0;
    if (probe && probed < 3 && ns > 0 && !indices.empty()) {
        probe_swapchain_pixels(chains[0], indices[0]);
        ++probed;
    }

    VkResult res = l.queue_present(from_u64<VkQueue>(queue), &pi);
    static int presents = 0;
    // Which thread presents matters: only the main loop pumps Wayland,
    // and a driver completing a present may need the display dispatched.
    // So it is worth saying when it CHANGES, not once every two seconds
    // for the life of the session -- these two lines were 615 of one
    // in-game log.
    // The window is shown on the first frame rather than at creation,
    // so it never sits empty through the engine's bring-up. No-op on
    // Wayland and after the first call.
    stud::android_glue::x11_ensure_mapped();
    const int present_tid = static_cast<int>(::syscall(SYS_gettid));
    static int announced_tid = -1;
    if (present_tid != announced_tid) {
        announced_tid = present_tid;
        std::printf("stud-render-host: present on tid=%d (main=%d)\n", present_tid,
                    static_cast<int>(::getpid()));
        std::fflush(stdout);
    }
    // The first few frames say the pipeline started; after that only a
    // failure, or a heartbeat rare enough to be worth reading (~a minute
    // at 60fps).
    if (presents < 5 || res != VK_SUCCESS || (presents % 3600) == 0) {
        std::printf("stud-render-host: vkQueuePresentKHR #%d -> %d (%u swapchain(s))\n", presents,
                    static_cast<int>(res), ns);
        std::fflush(stdout);
    }
    ++presents;
    return static_cast<uint64_t>(static_cast<int32_t>(res));
}

uint64_t vk_get_query_pool_results(uint64_t pool, uint32_t first, uint32_t count, uint32_t stride,
                                    uint32_t flags, std::vector<uint8_t>& out, uint32_t* out_len) {
    Loader& l = loader();
    if (l.get_query_pool_results == nullptr) {
        return static_cast<uint64_t>(static_cast<int32_t>(VK_ERROR_INITIALIZATION_FAILED));
    }
    const size_t size = static_cast<size_t>(stride) * count;
    out.assign(size, 0);
    VkResult r = l.get_query_pool_results(l.device, from_u64<VkQueryPool>(pool), first, count, size,
                                           out.data(), stride, flags);
    *out_len = static_cast<uint32_t>(out.size());
    return static_cast<uint64_t>(static_cast<int32_t>(r));
}

// One entry point for the whole vkCmd* family. Every member takes a
// command buffer, returns nothing, and differs only in payload -- so
// they share a call id and, on the client side, the reply-free path.
uint64_t vk_cmd_record(uint64_t cb_handle, uint32_t kind, const std::vector<uint8_t>& in) {
    Loader& l = loader();
    VkCommandBuffer cb = from_u64<VkCommandBuffer>(cb_handle);
    vk_wire::Reader r(in.data(), in.size());
    using K = vk_wire::CmdKind;

    // Which command was being recorded, for the crash handler. A
    // backtrace that stops at this function names the function and not
    // the command, which is the one thing needed to act on it.
    {
        char note[96];
        std::snprintf(note, sizeof(note), "vk_cmd_record kind=%u cb=%llx in=%zu", kind,
                      static_cast<unsigned long long>(cb_handle), in.size());
        stud::logging::set_crash_note(note);
    }

    // STUD_VK_CMD_STATS=1: how many of each command actually get
    // recorded. Distinguishes "the engine never drew" from "it drew and
    // nothing reached the screen", which is the whole question when the
    // window is black.
    static const bool cmd_stats = std::getenv("STUD_VK_CMD_STATS") != nullptr;
    if (cmd_stats) {
        static std::map<uint32_t, uint64_t> counts;
        static int since_report = 0;
        ++counts[kind];
        if (++since_report >= 2000) {
            since_report = 0;
            std::printf("stud-render-host: VKCMDSTATS");
            for (const auto& kv : counts) {
                std::printf(" %u=%llu", kv.first, static_cast<unsigned long long>(kv.second));
            }
            std::printf("\n");
            std::fflush(stdout);
        }
    }

    switch (static_cast<K>(kind)) {
        case K::BeginRenderPass: {
            if (l.cmd_begin_render_pass == nullptr) break;
            // STUD_VK_TRACE_PASSES=1: what each render pass actually
            // renders into, and how big. A frame's worth of draws going
            // into a 1x1 or zero-sized area looks exactly like a black
            // screen from the outside.
            static const bool trace_passes = std::getenv("STUD_VK_TRACE_PASSES") != nullptr;
            static int pass_no = 0;
            // Re-arm on every resize: the interesting passes are the ones
            // recorded just after the window changed, not the first 40 of
            // the process's life.
            static uint32_t traced_generation = 0;
            const uint32_t gen = g_resize_generation.load(std::memory_order_relaxed);
            if (gen != traced_generation) {
                traced_generation = gen;
                pass_no = 0;
            }
            VkRenderPassBeginInfo bi{};
            bi.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
            bi.renderPass = from_u64<VkRenderPass>(r.u64());
            bi.framebuffer = from_u64<VkFramebuffer>(r.u64());
            bi.renderArea.offset.x = static_cast<int32_t>(r.u32());
            bi.renderArea.offset.y = static_cast<int32_t>(r.u32());
            bi.renderArea.extent.width = r.u32();
            bi.renderArea.extent.height = r.u32();
            const uint32_t nc = r.u32();
            std::vector<VkClearValue> clears(nc);
            for (auto& c : clears) {
                for (int i = 0; i < 4; ++i) c.color.float32[i] = r.f32();
            }
            bi.clearValueCount = nc;
            bi.pClearValues = clears.empty() ? nullptr : clears.data();
            const uint32_t contents = r.u32();
            // STUD_VK_FORCE_CLEAR=1 paints every screen-targeting render
            // pass bright magenta. If the window turns magenta, buffers
            // and presentation are fine and the black is the engine's own
            // content; if it stays black, the problem is downstream of
            // rendering. Same decisive test the GL path used years ago.
            static const bool force_clear = std::getenv("STUD_VK_FORCE_CLEAR") != nullptr;
            if (force_clear && l.swapchain_framebuffers.count(to_u64(bi.framebuffer)) != 0) {
                // A pass with loadOp CLEAR needs a clear value per
                // attachment; the engine's own pass may supply none, so
                // add them rather than only overwriting what is there.
                if (clears.empty()) clears.resize(2);
                for (auto& c : clears) {
                    c.color.float32[0] = 1.0f;
                    c.color.float32[1] = 0.0f;
                    c.color.float32[2] = 1.0f;
                    c.color.float32[3] = 1.0f;
                }
                bi.clearValueCount = static_cast<uint32_t>(clears.size());
                bi.pClearValues = clears.data();
            }
            if (l.swapchain_framebuffers.count(to_u64(bi.framebuffer)) != 0) {
                static int to_screen = 0;
                // Three at the start prove the engine is drawing to the
                // screen at all. After that it is one line per 200
                // render passes forever, which in a real game is 183 of
                // them in one session saying the same thing.
                if (to_screen < 3) {
                    std::printf("stud-render-host: render pass #%d TARGETS THE SCREEN\n",
                                to_screen);
                    std::fflush(stdout);
                }
                ++to_screen;
            }
            if (trace_passes && pass_no < 40) {
                std::printf("stud-render-host: PASS#%d fb=%llu area=%dx%d+%d+%d clears=%u%s\n",
                            pass_no, static_cast<unsigned long long>(to_u64(bi.framebuffer)),
                            bi.renderArea.extent.width, bi.renderArea.extent.height,
                            bi.renderArea.offset.x, bi.renderArea.offset.y, nc,
                            l.swapchain_framebuffers.count(to_u64(bi.framebuffer)) != 0
                                ? "  <-- SCREEN"
                                : "");
                std::fflush(stdout);
            }
            ++pass_no;
            l.pass_draws = 0;
            l.traced_pass = trace_passes && pass_no <= 40 ? pass_no : -1;
            l.in_screen_pass = l.swapchain_framebuffers.count(to_u64(bi.framebuffer)) != 0;
            l.cmd_begin_render_pass(cb, &bi, static_cast<VkSubpassContents>(contents));
            break;
        }
        case K::EndRenderPass:
            if (l.traced_pass >= 0) {
                std::printf("stud-render-host: PASS#%d ended: %llu draw(s)\n", l.traced_pass - 1,
                            static_cast<unsigned long long>(l.pass_draws));
                std::fflush(stdout);
                l.traced_pass = -1;
            }
            if (l.in_screen_pass) {
                static const bool trace = std::getenv("STUD_VK_TRACE_PASSES") != nullptr;
                static int reported = 0;
                if (trace && reported < 12) {
                    std::printf("stud-render-host: SCREEN PASS ended: %llu draw(s), %llu bind(s)\n",
                                static_cast<unsigned long long>(l.screen_draws),
                                static_cast<unsigned long long>(l.screen_binds));
                    std::fflush(stdout);
                    ++reported;
                }
                l.in_screen_pass = false;
                l.screen_draws = 0;
                l.screen_binds = 0;
            }
            if (l.cmd_end_render_pass) l.cmd_end_render_pass(cb);
            break;
        case K::BindPipeline: {
            if (l.cmd_bind_pipeline == nullptr) break;
            const uint32_t bp = r.u32();
            l.cmd_bind_pipeline(cb, static_cast<VkPipelineBindPoint>(bp),
                                 from_u64<VkPipeline>(r.u64()));
            break;
        }
        case K::BindDescriptorSets: {
            if (l.in_screen_pass) ++l.screen_binds;
            if (l.cmd_bind_descriptor_sets == nullptr) break;
            const uint32_t bp = r.u32();
            VkPipelineLayout layout = from_u64<VkPipelineLayout>(r.u64());
            const uint32_t first = r.u32();
            const uint32_t ns = r.u32();
            std::vector<VkDescriptorSet> sets(ns);
            for (auto& s : sets) s = resolve_descriptor_set(r.u64());
            const uint32_t nd = r.u32();
            std::vector<uint32_t> offsets(nd);
            for (auto& o : offsets) o = r.u32();
            l.cmd_bind_descriptor_sets(cb, static_cast<VkPipelineBindPoint>(bp), layout, first, ns,
                                        sets.empty() ? nullptr : sets.data(), nd,
                                        offsets.empty() ? nullptr : offsets.data());
            break;
        }
        case K::BindVertexBuffers: {
            if (l.cmd_bind_vertex_buffers == nullptr) break;
            const uint32_t first = r.u32();
            const uint32_t n = r.u32();
            std::vector<VkBuffer> buffers(n);
            std::vector<VkDeviceSize> offsets(n);
            for (auto& b : buffers) b = from_u64<VkBuffer>(r.u64());
            for (auto& o : offsets) o = r.u64();
            l.cmd_bind_vertex_buffers(cb, first, n, buffers.empty() ? nullptr : buffers.data(),
                                       offsets.empty() ? nullptr : offsets.data());
            break;
        }
        case K::BindIndexBuffer: {
            if (l.cmd_bind_index_buffer == nullptr) break;
            VkBuffer buffer = from_u64<VkBuffer>(r.u64());
            const uint64_t offset = r.u64();
            l.cmd_bind_index_buffer(cb, buffer, offset, static_cast<VkIndexType>(r.u32()));
            break;
        }
        case K::Draw: {
            ++l.pass_draws;
            if (l.in_screen_pass) ++l.screen_draws;
            if (l.cmd_draw == nullptr) break;
            const uint32_t vc = r.u32(), ic = r.u32(), fv = r.u32(), fi = r.u32();
            l.cmd_draw(cb, vc, ic, fv, fi);
            break;
        }
        case K::DrawIndexed: {
            ++l.pass_draws;
            if (l.in_screen_pass) ++l.screen_draws;
            if (l.cmd_draw_indexed == nullptr) break;
            const uint32_t xc = r.u32(), ic = r.u32(), fx = r.u32();
            const int32_t vo = static_cast<int32_t>(r.u32());
            const uint32_t fi = r.u32();
            l.cmd_draw_indexed(cb, xc, ic, fx, vo, fi);
            break;
        }
        case K::Dispatch: {
            if (l.cmd_dispatch == nullptr) break;
            const uint32_t x = r.u32(), y = r.u32(), z = r.u32();
            l.cmd_dispatch(cb, x, y, z);
            break;
        }
        case K::SetViewport: {
            if (l.cmd_set_viewport == nullptr) break;
            const uint32_t first = r.u32();
            const uint32_t n = r.u32();
            std::vector<VkViewport> vps(n);
            for (auto& v : vps) {
                v.x = r.f32();
                v.y = r.f32();
                v.width = r.f32();
                v.height = r.f32();
                v.minDepth = r.f32();
                v.maxDepth = r.f32();
            }
            l.cmd_set_viewport(cb, first, n, vps.empty() ? nullptr : vps.data());
            break;
        }
        case K::SetScissor: {
            if (l.cmd_set_scissor == nullptr) break;
            const uint32_t first = r.u32();
            const uint32_t n = r.u32();
            std::vector<VkRect2D> rects(n);
            for (auto& rc : rects) {
                rc.offset.x = static_cast<int32_t>(r.u32());
                rc.offset.y = static_cast<int32_t>(r.u32());
                rc.extent.width = r.u32();
                rc.extent.height = r.u32();
            }
            l.cmd_set_scissor(cb, first, n, rects.empty() ? nullptr : rects.data());
            break;
        }
        case K::PipelineBarrier: {
            if (l.cmd_pipeline_barrier == nullptr) break;
            const uint32_t src_stage = r.u32(), dst_stage = r.u32(), dep_flags = r.u32();
            const uint32_t nm = r.u32();
            std::vector<VkMemoryBarrier> mem(nm);
            for (auto& m : mem) {
                m.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER;
                m.srcAccessMask = r.u32();
                m.dstAccessMask = r.u32();
            }
            const uint32_t nb = r.u32();
            std::vector<VkBufferMemoryBarrier> buf(nb);
            for (auto& b : buf) {
                b.sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER;
                b.srcAccessMask = r.u32();
                b.dstAccessMask = r.u32();
                b.srcQueueFamilyIndex = r.u32();
                b.dstQueueFamilyIndex = r.u32();
                b.buffer = from_u64<VkBuffer>(r.u64());
                b.offset = r.u64();
                b.size = r.u64();
            }
            // Same treatment for buffers, for the same reason -- a
            // barrier on no buffer is meaningless and the driver has no
            // reason to tolerate one.
            buf.erase(std::remove_if(buf.begin(), buf.end(),
                                     [](const VkBufferMemoryBarrier& b) {
                                         return b.buffer == VK_NULL_HANDLE;
                                     }),
                      buf.end());
            const uint32_t ni = r.u32();
            std::vector<VkImageMemoryBarrier> img(ni);
            for (auto& i : img) {
                i.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
                i.srcAccessMask = r.u32();
                i.dstAccessMask = r.u32();
                i.oldLayout = static_cast<VkImageLayout>(r.u32());
                i.newLayout = static_cast<VkImageLayout>(r.u32());
                i.srcQueueFamilyIndex = r.u32();
                i.dstQueueFamilyIndex = r.u32();
                i.image = from_u64<VkImage>(r.u64());
                i.subresourceRange.aspectMask = r.u32();
                i.subresourceRange.baseMipLevel = r.u32();
                i.subresourceRange.levelCount = r.u32();
                i.subresourceRange.baseArrayLayer = r.u32();
                i.subresourceRange.layerCount = r.u32();
            }
            // Drop barriers against images that died with their
            // swapchain. See Loader::retired_images for why the engine
            // records these at all -- in short, a resize makes the
            // acquire fail and the frame already in flight carries on
            // regardless. Passing the freed handle through crashes the
            // driver inside render-host, taking the whole session with
            // it; dropping the barrier loses a layout transition for an
            // image that is never going to be presented anyway.
            {
                const size_t before = img.size();
                img.erase(std::remove_if(img.begin(), img.end(),
                                         [&l](const VkImageMemoryBarrier& b) {
                                             // A NULL image is the one that actually crashes:
                                             // when vkAcquireNextImageKHR fails with
                                             // OUT_OF_DATE during a resize the engine has no
                                             // image to transition, and records the barrier
                                             // with VK_NULL_HANDLE anyway. The driver
                                             // dereferences it (live-caught: fault address
                                             // 0x34a, a fixed offset off null). A barrier on
                                             // no image means nothing, so dropping it costs
                                             // nothing.
                                             return b.image == VK_NULL_HANDLE ||
                                                    l.retired_images.count(to_u64(b.image)) != 0;
                                         }),
                          img.end());
                if (img.size() != before) {
                    static uint64_t dropped = 0;
                    dropped += before - img.size();
                    static uint64_t announced = 0;
                    if (dropped >= announced + 64 || announced == 0) {
                        announced = dropped;
                        std::printf("stud-render-host: dropped %llu image barrier(s) with no "
                                    "live image (resize)\n",
                                    static_cast<unsigned long long>(dropped));
                        std::fflush(stdout);
                    }
                }
            }
            {
                char note[160];
                int at = std::snprintf(note, sizeof(note),
                                       "PipelineBarrier cb=%llx mem=%u buf=%u img=%u:",
                                       static_cast<unsigned long long>(cb_handle), nm, nb,
                                       static_cast<unsigned>(img.size()));
                for (const auto& b : img) {
                    if (at < 0 || at >= static_cast<int>(sizeof(note))) break;
                    at += std::snprintf(note + at, sizeof(note) - static_cast<size_t>(at), " %llx",
                                        static_cast<unsigned long long>(to_u64(b.image)));
                }
                stud::logging::set_crash_note(note);
            }
            // STUD_VK_TRACE_PASSES also reports layout transitions on the
            // swapchain images. A swapchain image presented in the wrong
            // layout shows as black, and the transition is the engine's
            // job to record -- so it is worth knowing whether it does.
            static const bool trace_barriers = std::getenv("STUD_VK_TRACE_PASSES") != nullptr;
            if (trace_barriers) {
                static int reported = 0;
                for (const auto& b : img) {
                    if (l.swapchain_images.count(to_u64(b.image)) != 0 && reported < 10) {
                        std::printf("stud-render-host: SWAPCHAIN IMAGE barrier %u -> %u\n",
                                    static_cast<uint32_t>(b.oldLayout),
                                    static_cast<uint32_t>(b.newLayout));
                        std::fflush(stdout);
                        ++reported;
                    }
                }
            }
            // The COUNTS come from the vectors, not from the wire. They
            // are the same number until something above drops an entry,
            // and then handing the driver the original count with a
            // shorter array is a read past the end of it.
            l.cmd_pipeline_barrier(cb, src_stage, dst_stage, dep_flags,
                                    static_cast<uint32_t>(mem.size()),
                                    mem.empty() ? nullptr : mem.data(),
                                    static_cast<uint32_t>(buf.size()),
                                    buf.empty() ? nullptr : buf.data(),
                                    static_cast<uint32_t>(img.size()),
                                    img.empty() ? nullptr : img.data());
            break;
        }
        case K::CopyBuffer: {
            if (l.cmd_copy_buffer == nullptr) break;
            VkBuffer src = from_u64<VkBuffer>(r.u64());
            VkBuffer dst = from_u64<VkBuffer>(r.u64());
            const uint32_t n = r.u32();
            std::vector<VkBufferCopy> regions(n);
            for (auto& c : regions) {
                c.srcOffset = r.u64();
                c.dstOffset = r.u64();
                c.size = r.u64();
            }
            l.cmd_copy_buffer(cb, src, dst, n, regions.empty() ? nullptr : regions.data());
            break;
        }
        case K::CopyBufferToImage: {
            if (l.cmd_copy_buffer_to_image == nullptr) break;
            VkBuffer src = from_u64<VkBuffer>(r.u64());
            VkImage dst = from_u64<VkImage>(r.u64());
            const uint32_t layout = r.u32();
            const uint32_t n = r.u32();
            std::vector<VkBufferImageCopy> regions(n);
            for (auto& c : regions) {
                c.bufferOffset = r.u64();
                c.bufferRowLength = r.u32();
                c.bufferImageHeight = r.u32();
                c.imageSubresource.aspectMask = r.u32();
                c.imageSubresource.mipLevel = r.u32();
                c.imageSubresource.baseArrayLayer = r.u32();
                c.imageSubresource.layerCount = r.u32();
                c.imageOffset.x = static_cast<int32_t>(r.u32());
                c.imageOffset.y = static_cast<int32_t>(r.u32());
                c.imageOffset.z = static_cast<int32_t>(r.u32());
                c.imageExtent.width = r.u32();
                c.imageExtent.height = r.u32();
                c.imageExtent.depth = r.u32();
            }
            l.cmd_copy_buffer_to_image(cb, src, dst, static_cast<VkImageLayout>(layout), n,
                                        regions.empty() ? nullptr : regions.data());
            break;
        }
        case K::CopyImageToBuffer: {
            if (l.cmd_copy_image_to_buffer == nullptr) break;
            VkImage src = from_u64<VkImage>(r.u64());
            const uint64_t dst_handle = r.u64();
            VkBuffer dst = from_u64<VkBuffer>(dst_handle);
            const uint32_t layout = r.u32();
            const uint32_t n = r.u32();
            std::vector<VkBufferImageCopy> regions(n);
            for (auto& c : regions) {
                c.bufferOffset = r.u64();
                c.bufferRowLength = r.u32();
                c.bufferImageHeight = r.u32();
                c.imageSubresource.aspectMask = r.u32();
                c.imageSubresource.mipLevel = r.u32();
                c.imageSubresource.baseArrayLayer = r.u32();
                c.imageSubresource.layerCount = r.u32();
                c.imageOffset.x = static_cast<int32_t>(r.u32());
                c.imageOffset.y = static_cast<int32_t>(r.u32());
                c.imageOffset.z = static_cast<int32_t>(r.u32());
                c.imageExtent.width = r.u32();
                c.imageExtent.height = r.u32();
                c.imageExtent.depth = r.u32();
            }
            // The result is only visible to the engine if the buffer's
            // memory is one of the allocations this process imported from
            // it -- then the GPU writes straight into the pages the
            // engine has mapped. A copied allocation has no path back, so
            // say so instead of leaving the engine to read stale bytes.
            {
                uint64_t memory = 0;
                {
                    std::lock_guard<std::mutex> lock(buffer_memory_mutex());
                    auto it = buffer_memory().find(dst_handle);
                    if (it != buffer_memory().end()) memory = it->second;
                }
                bool visible = false;
                {
                    std::lock_guard<std::mutex> lock(shared_memory_mutex());
                    visible = memory != 0 && shared_memory().count(memory) != 0;
                }
                if (!visible) {
                    static bool warned = false;
                    if (!warned) {
                        warned = true;
                        std::printf("stud-render-host: vkCmdCopyImageToBuffer reads back into "
                                    "memory this process does not share with the engine -- the "
                                    "copy runs, but the engine cannot see the result\n");
                        std::fflush(stdout);
                    }
                }
            }
            l.cmd_copy_image_to_buffer(cb, src, static_cast<VkImageLayout>(layout), dst, n,
                                        regions.empty() ? nullptr : regions.data());
            break;
        }
        case K::CopyImage: {
            if (l.cmd_copy_image == nullptr) break;
            VkImage src = from_u64<VkImage>(r.u64());
            const uint32_t sl = r.u32();
            VkImage dst = from_u64<VkImage>(r.u64());
            const uint32_t dl = r.u32();
            const uint32_t n = r.u32();
            std::vector<VkImageCopy> regions(n);
            auto read_subres = [&r](VkImageSubresourceLayers& s) {
                s.aspectMask = r.u32();
                s.mipLevel = r.u32();
                s.baseArrayLayer = r.u32();
                s.layerCount = r.u32();
            };
            for (auto& c : regions) {
                read_subres(c.srcSubresource);
                c.srcOffset.x = static_cast<int32_t>(r.u32());
                c.srcOffset.y = static_cast<int32_t>(r.u32());
                c.srcOffset.z = static_cast<int32_t>(r.u32());
                read_subres(c.dstSubresource);
                c.dstOffset.x = static_cast<int32_t>(r.u32());
                c.dstOffset.y = static_cast<int32_t>(r.u32());
                c.dstOffset.z = static_cast<int32_t>(r.u32());
                c.extent.width = r.u32();
                c.extent.height = r.u32();
                c.extent.depth = r.u32();
            }
            l.cmd_copy_image(cb, src, static_cast<VkImageLayout>(sl), dst,
                              static_cast<VkImageLayout>(dl), n,
                              regions.empty() ? nullptr : regions.data());
            break;
        }
        case K::BlitImage: {
            if (l.cmd_blit_image == nullptr) break;
            VkImage src = from_u64<VkImage>(r.u64());
            const uint32_t sl = r.u32();
            VkImage dst = from_u64<VkImage>(r.u64());
            const uint32_t dl = r.u32();
            const uint32_t n = r.u32();
            std::vector<VkImageBlit> regions(n);
            auto read_subres = [&r](VkImageSubresourceLayers& s) {
                s.aspectMask = r.u32();
                s.mipLevel = r.u32();
                s.baseArrayLayer = r.u32();
                s.layerCount = r.u32();
            };
            for (auto& b : regions) {
                read_subres(b.srcSubresource);
                for (int i = 0; i < 2; ++i) {
                    b.srcOffsets[i].x = static_cast<int32_t>(r.u32());
                    b.srcOffsets[i].y = static_cast<int32_t>(r.u32());
                    b.srcOffsets[i].z = static_cast<int32_t>(r.u32());
                }
                read_subres(b.dstSubresource);
                for (int i = 0; i < 2; ++i) {
                    b.dstOffsets[i].x = static_cast<int32_t>(r.u32());
                    b.dstOffsets[i].y = static_cast<int32_t>(r.u32());
                    b.dstOffsets[i].z = static_cast<int32_t>(r.u32());
                }
            }
            const uint32_t filter = r.u32();
            if (l.swapchain_images.count(to_u64(dst)) != 0) {
                static int blits = 0;
                if (blits < 3) {
                    std::printf("stud-render-host: blit #%d TARGETS THE SCREEN\n", blits);
                    std::fflush(stdout);
                }
                ++blits;
            }
            l.cmd_blit_image(cb, src, static_cast<VkImageLayout>(sl), dst,
                              static_cast<VkImageLayout>(dl), n,
                              regions.empty() ? nullptr : regions.data(),
                              static_cast<VkFilter>(filter));
            break;
        }
        case K::ResolveImage: {
            // Same wire shape as CopyImage: VkImageResolve and VkImageCopy
            // are laid out identically.
            if (l.cmd_resolve_image == nullptr) break;
            VkImage src = from_u64<VkImage>(r.u64());
            const uint32_t sl = r.u32();
            VkImage dst = from_u64<VkImage>(r.u64());
            const uint32_t dl = r.u32();
            const uint32_t n = r.u32();
            std::vector<VkImageResolve> regions(n);
            auto read_subres = [&r](VkImageSubresourceLayers& s) {
                s.aspectMask = r.u32();
                s.mipLevel = r.u32();
                s.baseArrayLayer = r.u32();
                s.layerCount = r.u32();
            };
            for (auto& c : regions) {
                read_subres(c.srcSubresource);
                c.srcOffset.x = static_cast<int32_t>(r.u32());
                c.srcOffset.y = static_cast<int32_t>(r.u32());
                c.srcOffset.z = static_cast<int32_t>(r.u32());
                read_subres(c.dstSubresource);
                c.dstOffset.x = static_cast<int32_t>(r.u32());
                c.dstOffset.y = static_cast<int32_t>(r.u32());
                c.dstOffset.z = static_cast<int32_t>(r.u32());
                c.extent.width = r.u32();
                c.extent.height = r.u32();
                c.extent.depth = r.u32();
            }
            l.cmd_resolve_image(cb, src, static_cast<VkImageLayout>(sl), dst,
                                 static_cast<VkImageLayout>(dl), n,
                                 regions.empty() ? nullptr : regions.data());
            break;
        }
        case K::ResetQueryPool: {
            if (l.cmd_reset_query_pool == nullptr) break;
            VkQueryPool pool = from_u64<VkQueryPool>(r.u64());
            const uint32_t first = r.u32(), count = r.u32();
            l.cmd_reset_query_pool(cb, pool, first, count);
            break;
        }
        case K::WriteTimestamp: {
            if (l.cmd_write_timestamp == nullptr) break;
            const uint32_t stage = r.u32();
            VkQueryPool pool = from_u64<VkQueryPool>(r.u64());
            const uint32_t query = r.u32();
            l.cmd_write_timestamp(cb, static_cast<VkPipelineStageFlagBits>(stage), pool, query);
            break;
        }
        default:
            break;
    }
    return 0;
}

}  // namespace stud::render_host
