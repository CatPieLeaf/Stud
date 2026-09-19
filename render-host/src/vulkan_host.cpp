// Real Vulkan, host side. Process C is ordinary glibc, so the vendor
// driver is at home here, which is exactly why the driver must never
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

#include <chrono>
#include <cstdio>
#include <cstring>
#include <fcntl.h>
#include <sys/mman.h>
#include <unistd.h>
#include <mutex>
#include <shared_mutex>
#include <condition_variable>
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
#include "stud/render_host_protocol.h"

namespace stud::render_host {

// The one real window this process owns, supplied once at startup by
// main(). Kept as plain numbers so nothing here can accidentally create
// a window as a side effect of a query.
namespace {
// The extent each live swapchain was actually created with. Recorded so
// a resize can be seen; NOT used to force VK_ERROR_OUT_OF_DATE_KHR;
// see swapchain_is_out_of_date's own comment for the live result of
// trying that.
// Once the driver has lost the device, every further call on it is
// meaningless and some are lethal: a vkQueueSubmit issued after the loss
// is where NVIDIA's driver segfaulted, live-caught, inside the very next
// acquire. So Stud stops issuing repair work of its own at that point.
//
// Cleared when the engine builds a NEW device: the loss belonged to the
// old one, and leaving it set turned the first loss of a session into a
// permanent, one-way disabling of three repair paths.
std::atomic<bool> g_device_lost{false};

// A device the engine has finished with, kept until the engine proves it
// has moved on.
//
// vkDestroyDevice arriving on the wire means the engine has destroyed
// every object it made -- that is the spec's own precondition, not a
// guess. What it does NOT prove is that Stud is finished: this process
// resolved its device commands from that device and may still be inside
// one on another thread. So the device is not destroyed at that moment.
// It is parked here with everything of Stud's that belongs to it, and
// released once the engine has presented enough frames on the REPLACEMENT
// device that the old one cannot still be in use.
//
// The mappings come with it. Unmapping them while the device still lives
// is what segfaulted inside libnvidia-glcore in an earlier attempt: the
// driver was still importing those pages. After the device is destroyed
// it is not, and the same munmap is safe.
struct RetiredDevice {
    VkDevice device = VK_NULL_HANDLE;
    VkCommandPool probe_pool = VK_NULL_HANDLE;
    VkFence repair_fence = VK_NULL_HANDLE;
    std::map<uint64_t, std::pair<void*, size_t>> writes;
};
// The repair submissions' one reusable fence. Declared here rather than
// as a function-local static so a device being retired can take it along:
// it belongs to whichever device created it.
VkFence g_repair_fence = VK_NULL_HANDLE;

std::vector<RetiredDevice> g_retired_devices;
// Presents on the new device since the old one was parked. The engine
// cannot have presented this many frames on a device it never finished
// building, so it is proof of a working replacement rather than a timer.
std::atomic<uint64_t> g_presents_since_new_device{0};

std::map<uint64_t, VkExtent2D> g_swapchain_extents;

// Atomic because they are written by whichever thread pumps Wayland and
// read by whichever thread is answering a surface-capabilities query,
// and after the secondary-connection change those are routinely
// different threads. Relaxed is enough: each is read on its own, and a
// reader that catches a resize one query late simply asks again.
std::atomic<uint32_t> g_window_width{0};
std::atomic<uint32_t> g_window_height{0};

// Whether this process offers Vulkan at all; see the header. Set once,
// from main(), before any client can connect.
bool g_vulkan_enabled = true;

// The GPU the user picked in Settings, as an index into
// vkEnumeratePhysicalDevices' own order, the same identifier the
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

// How hard the sharpening pass pulls, as a percentage: 100 is RCAS at its
// ordinary full strength and 125 is as far as the filter can be pushed
// before its renormaliser reaches zero (see sharpen.comp).
std::atomic<int32_t> g_upscale_sharpness_percent{100};

float upscale_sharpness() {
    return static_cast<float>(g_upscale_sharpness_percent.load(std::memory_order_relaxed)) / 100.0f;
}

void vk_set_upscale_sharpness_percent(int32_t percent) {
    const int32_t clamped = percent < 0 ? 0 : (percent > 125 ? 125 : percent);
    g_upscale_sharpness_percent.store(clamped, std::memory_order_relaxed);
}

std::atomic<uint32_t> g_upscale_output_w{0};
std::atomic<uint32_t> g_upscale_output_h{0};
// Whether the pass may use its shaders. See the header.
std::atomic<bool> g_upscale_use_compute{true};

void vk_set_upscale_use_compute(bool use_compute) {
    g_upscale_use_compute.store(use_compute, std::memory_order_relaxed);
}

void vk_set_upscale_output_size(uint32_t width, uint32_t height) {
    g_upscale_output_w.store(width, std::memory_order_relaxed);
    g_upscale_output_h.store(height, std::memory_order_relaxed);
}

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
    // Needed for the imported-host-pointer alignment, which is a
    // properties2 chain and has no 1.0 equivalent.
    PFN_vkGetPhysicalDeviceProperties2 get_physical_device_properties2 = nullptr;
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
    // exists, the spec's own requirement, and what reaches the
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
    // engine creates for every pipeline it builds were never freed,
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
    PFN_vkUpdateDescriptorSets update_descriptor_sets = nullptr;
    PFN_vkCmdPushConstants cmd_push_constants = nullptr;
    PFN_vkCmdSetViewport cmd_set_viewport = nullptr;
    PFN_vkCmdSetScissor cmd_set_scissor = nullptr;
    PFN_vkCmdPipelineBarrier cmd_pipeline_barrier = nullptr;
    PFN_vkCmdCopyBuffer cmd_copy_buffer = nullptr;
    PFN_vkCmdCopyBufferToImage cmd_copy_buffer_to_image = nullptr;
    PFN_vkCmdCopyImage cmd_copy_image = nullptr;
    PFN_vkCmdBlitImage cmd_blit_image = nullptr;
    // VK_NV_device_diagnostic_checkpoints. Null unless the driver has it.
    PFN_vkCmdSetCheckpointNV cmd_set_checkpoint = nullptr;
    PFN_vkGetQueueCheckpointDataNV get_queue_checkpoint_data = nullptr;
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
    // How many bytes of each of those a vkMapMemory asked for, so a read
    // back out of one can be refused rather than run off the end. Zero
    // where the size is not known (VK_WHOLE_SIZE).
    std::map<uint64_t, uint64_t> mapped_size;

    // Which objects lead to the screen. A frame can look perfectly busy
    // thousands of draws, successful presents, and still be black if
    // nothing the engine renders ever targets a swapchain image, so this
    // follows image -> view -> framebuffer and reports whether any render
    // pass or blit actually writes to one.
    std::set<uint64_t> swapchain_images;
    // The queue family the engine asked for first, which is the family
    // Stud's own upscale pass records its command buffers against.
    uint32_t first_queue_family = 0;
    bool have_first_queue_family = false;
    // Which images belong to which swapchain, and which images belonged
    // to one that has since been destroyed.
    //
    // vkDestroySwapchainKHR frees its images with it. An application is
    // not supposed to use them afterwards, but a resize makes the
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
        // Captures the function pointer, not the static itself: capturing a
        // variable with static storage is deprecated in C++20.
        auto global = [get = l.get_instance_proc_addr](const char* n) {
            return get(VK_NULL_HANDLE, n);
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
    // report it even when the caller's array is too small; that is what
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
        // engine fall back to its own GLES path; no flag, no knowledge
        // of the engine's internals.
        std::printf("stud-render-host: vkCreateInstance refused, graphics mode is OpenGL\n");
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
    // The counts are known before the loops, so the vectors are sized once
    // instead of growing.
    std::vector<std::string> layers;
    layers.reserve(hdr.enabled_layer_count);
    for (uint32_t i = 0; i < hdr.enabled_layer_count; ++i) layers.push_back(take_cstr());
    std::vector<std::string> extensions;
    extensions.reserve(hdr.enabled_extension_count);
    for (uint32_t i = 0; i < hdr.enabled_extension_count; ++i) extensions.push_back(take_cstr());

    // The engine is an Android client, so it asks for
    // VK_KHR_android_surface. This process presents to Wayland, where
    // that extension does not exist and its absence would fail instance
    // creation outright. Substituting the Wayland surface extension is
    // the same interposition the surface-creation path already does,
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
    // the spec's own requirement, and the only way to reach a driver's
    // real implementations rather than the loader's trampolines.
    l.instance = instance;
    auto inst = [&l](const char* n) { return l.get_instance_proc_addr(l.instance, n); };
    l.enumerate_physical_devices =
        reinterpret_cast<PFN_vkEnumeratePhysicalDevices>(inst("vkEnumeratePhysicalDevices"));
    l.get_physical_device_properties = reinterpret_cast<PFN_vkGetPhysicalDeviceProperties>(
        inst("vkGetPhysicalDeviceProperties"));
    l.get_physical_device_properties2 = reinterpret_cast<PFN_vkGetPhysicalDeviceProperties2>(
        inst("vkGetPhysicalDeviceProperties2"));
    if (l.get_physical_device_properties2 == nullptr) {
        l.get_physical_device_properties2 = reinterpret_cast<PFN_vkGetPhysicalDeviceProperties2>(
            inst("vkGetPhysicalDeviceProperties2KHR"));
    }
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
    // so ordering is the honest way to express a preference; every
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
    // the real loader what is at the position the user chose, the same
    // enumeration order the settings window listed, and translate.
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
    // heap is 24933841920 bytes and the engine reports 3459005440,
    // exactly the low 32 bits. Its device heap is 4294967296
    // (0x100000000), whose low 32 bits are 0, and the engine duly logs
    // `heapIndex = 0, heapFlags = 1 (device), heapSize = 0` and then
    // falls back to `caps.videoMemory = 67108864`: it believes a 4GiB
    // GPU has 64MB. That is the worst possible answer; it drives
    // texture and shadow resolution down and makes the engine stream and
    // evict constantly, which costs CPU every frame.
    //
    // Reporting 0xFFFFFFFF instead of the true size is the honest
    // choice here: it is the largest value the engine can actually
    // represent, so it is far closer to the truth than the zero it
    // derives on its own, and heap size is informational, the driver,
    // not this number, enforces what can really be allocated. Only
    // sizes that would truncate are touched; anything already under
    // 4GiB is passed through exactly.
    constexpr VkDeviceSize kMax32 = 0xFFFFFFFFull;
    for (uint32_t i = 0; i < mem.memoryHeapCount; ++i) {
        if (mem.memoryHeaps[i].size > kMax32) {
            static std::set<uint32_t> announced_heaps;
            if (announced_heaps.insert(i).second) {
                std::printf("stud-render-host: heap %u is %llu bytes, reporting %llu, the engine "
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
    // for it on every renderer rebuild, 86 times in one session, three
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

// Retired upscale passes cannot outlive the surface they present to or
// the device they were built on; see flush_retired_chains().
void flush_retired_chains(const char* why);

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
    // The counts are known before the loops, so the vectors are sized once
    // instead of growing.
    std::vector<std::string> layers;
    layers.reserve(hdr.enabled_layer_count);
    for (uint32_t i = 0; i < hdr.enabled_layer_count; ++i) layers.push_back(take_cstr());
    std::vector<std::string> extensions;
    extensions.reserve(hdr.enabled_extension_count);
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
    // be copied across the socket, measured at 197 MB/s in a real game,
    // which is most of what the render thread was doing.
    if (device_supports_extension(VK_EXT_EXTERNAL_MEMORY_HOST_EXTENSION_NAME)) {
        bool already = false;
        for (const std::string& e : extensions) {
            if (e == VK_EXT_EXTERNAL_MEMORY_HOST_EXTENSION_NAME) already = true;
        }
        if (!already) extensions.push_back(VK_EXT_EXTERNAL_MEMORY_HOST_EXTENSION_NAME);
    }

    // Before anything below asks what this device supports.
    //
    // device_supports_extension() reads l.physical_device, and this used
    // to be assigned further down, at the vkCreateDevice call itself --
    // so every check up here ran against VK_NULL_HANDLE, returned false,
    // and silently added nothing. The external-memory-host block below
    // has been dead the whole time for that reason, not because the
    // driver lacks it. Caught because a checkpoint extension that
    // vulkaninfo lists as present kept resolving to null entry points.
    l.physical_device =
        reinterpret_cast<VkPhysicalDevice>(static_cast<uintptr_t>(physical_device));

    // Checkpoints, so a hung GPU can say where it stopped.
    //
    // A device lost to a context-switch timeout tells you nothing about
    // WHICH work hung: the engine's own submissions and Stud's upscale
    // pass are on the same queue, and afterwards both look equally
    // guilty. These markers are written as the GPU passes them, and the
    // driver keeps the last ones it reached across the loss, so one
    // freeze names its own cause instead of needing a run with the
    // suspect turned off and another with it on -- which is no use at all
    // for a fault that appears at random, minutes apart.
    //
    // Recording a marker is a write, not a barrier or a stall, so this is
    // left on rather than hidden behind a switch: a diagnostic that is off
    // when the rare thing happens has no value.
    if (device_supports_extension(VK_NV_DEVICE_DIAGNOSTIC_CHECKPOINTS_EXTENSION_NAME)) {
        bool already = false;
        for (const std::string& e : extensions) {
            if (e == VK_NV_DEVICE_DIAGNOSTIC_CHECKPOINTS_EXTENSION_NAME) already = true;
        }
        if (!already) extensions.push_back(VK_NV_DEVICE_DIAGNOSTIC_CHECKPOINTS_EXTENSION_NAME);
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

    // The engine builds a whole new device when it rebuilds its renderer,
    // and l.device is replaced here. Anything still deferred against the
    // outgoing one has to go now, while its device, its queue and its
    // resolved commands are still the current ones -- afterwards it could
    // only be destroyed with a device it was never created on.
    flush_retired_chains("the engine builds a new device");

    VkDevice device = VK_NULL_HANDLE;
    // l.physical_device was set above, before the support checks.
    VkResult r = l.create_device(l.physical_device, &ci, nullptr, &device);
    std::printf("stud-render-host: vkCreateDevice -> %d (queues=%zu extensions=%zu chain=%u)\n",
                static_cast<int>(r), queues.size(), ext_ptrs.size(), hdr.chain_node_count);
    std::fflush(stdout);
    if (r != VK_SUCCESS) return static_cast<uint64_t>(static_cast<int32_t>(r));

    // Resolve every device-level command from the device itself, now that
    // one exists. The spec requires this, vkGetDeviceProcAddr reaches
    // the driver's real implementations, where vkGetInstanceProcAddr
    // would only give loader trampolines.
    //
    // Live-caught when this block was missing: every device call returned
    // the null-pointer path (VK_ERROR_INITIALIZATION_FAILED), and the
    // engine logged a wall of "VULKAN ERROR: vkCreateSemaphore ...
    // returned -3" before giving up with "Failed to allocate image
    // memory".
    // A NEW device means every per-device thing Stud remembered about the
    // old one is now a lie.
    //
    // The engine tears its renderer down and builds it again -- on a mode
    // probe, and after every device loss it recovers from: 4 to 6
    // vkCreateDevice calls in a typical session. None of this state was
    // being reset, which had two consequences.
    //
    // The memory one: entries keyed by handles from a device nobody can
    // reach any more are never erased, because the only erase sites are
    // engine-issued destroys naming handles the engine has already thrown
    // away. An upscale chain stranded that way takes its offscreen,
    // staging and sharpened images, their memory, views, pools,
    // pipelines, semaphores and fences with it -- the largest
    // allocations here. Measured from the engine's own side: 1941 MB
    // climbing to 2230 MB across four recoveries, while the scene being
    // drawn got SIMPLER.
    //
    // The correctness one: drivers reuse handle values, so a fresh
    // VkBuffer can collide with a dead one still in the table and be
    // answered for with the wrong allocation.
    //
    // Deliberately NOT destroying the old device here. Doing that safely
    // needs to know the engine has stopped using every object on it, and
    // Stud does not know that; see case K::Device in vk_destroy_handle().
    // Dropping Stud's own references is separate from, and safer than,
    // destroying the device, and it is what stops the tables growing.
    // NOT clearing Stud's per-device tables here, and the attempt to do
    // so is why this comment exists.
    //
    // It looked safe: a new device means the old handles are dead, so drop
    // them. They are not. The engine builds a second device while still
    // using the first -- the mode probe does exactly that -- and the tables
    // describe memory the OLD device still has imported. Dropping the
    // shared-write entries unmapped those pages out from under it, and the
    // driver segfaulted inside libnvidia-glcore in
    // vk_update_descriptor_set_with_template about a minute later. Live
    // caught, and it turned a recoverable freeze into a dead renderer --
    // the same trade the device-fault query made, for the same reason:
    // acting on an assumption about the engine's object lifetimes that
    // Stud is not in a position to make.
    //
    // The growth these tables show across a session is real and is
    // documented in Stud-Analysis/gpu/stud-freeze.md. Fixing it needs to
    // know when the engine has actually finished with a device, which
    // nothing here currently knows.

    // The device loss that prompted the rebuild belongs to the OLD device.
    //
    // This flag is what stops Stud issuing repair work on a device the
    // driver has given up on, and it was set with exchange(true) and never
    // once set back. So the first loss in a session permanently disabled
    // three repairs -- the failed-acquire signal, the stale-signal drain
    // and the orphaned-fence signal -- for the whole remaining life of the
    // process, on a device that no longer exists. A session that took one
    // loss became a structurally different program from one that had not,
    // with an identical startup log. The new device is healthy until it
    // says otherwise.
    g_device_lost.store(false, std::memory_order_relaxed);

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
        l.update_descriptor_sets =
            reinterpret_cast<PFN_vkUpdateDescriptorSets>(dev("vkUpdateDescriptorSets"));
        l.cmd_push_constants = reinterpret_cast<PFN_vkCmdPushConstants>(dev("vkCmdPushConstants"));
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
        l.cmd_set_checkpoint =
            reinterpret_cast<PFN_vkCmdSetCheckpointNV>(dev("vkCmdSetCheckpointNV"));
        l.get_queue_checkpoint_data =
            reinterpret_cast<PFN_vkGetQueueCheckpointDataNV>(dev("vkGetQueueCheckpointDataNV"));
        // Said at startup, because the answer decides what a later "no
        // checkpoints" line MEANS: with the entry points resolved it says
        // the GPU never reached Stud's own work, and without them it says
        // only that nothing was ever recorded.
        std::printf("stud-render-host: GPU checkpoints %s\n",
                    (l.cmd_set_checkpoint != nullptr && l.get_queue_checkpoint_data != nullptr)
                        ? "are available; Stud's own passes are marked, so a device loss can say "
                          "where the GPU stopped"
                        : "are NOT available on this driver; a device loss will not be able to say "
                          "where the GPU stopped");
        std::fflush(stdout);
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
    if (!l.have_first_queue_family) {
        l.have_first_queue_family = true;
        l.first_queue_family = family;
    }
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
    // A real game creates images constantly, measured at 14k of these
    // in one session, next to 18k memory-requirement lines, which was
    // 84% of the whole log, and "it worked" for the 14000th time says
    // nothing. The failure line is what mattered, and it was the one
    // buried.
    if (r != VK_SUCCESS || vk_object_trace_enabled()) {
        std::printf("stud-render-host: vkCreateImage -> %d (format=%u %ux%u usage=0x%x tiling=%u)\n",
                    static_cast<int>(r), h.format, h.extent_width, h.extent_height, h.usage,
                    h.tiling);
        std::fflush(stdout);
    }
    if (r != VK_SUCCESS) return static_cast<uint64_t>(static_cast<int32_t>(r));
    // The driver recycles handles, and a retired swapchain image's handle
    // can come back here as an ordinary image. Without this, that image
    // stays "retired" for the rest of the process and EVERY barrier
    // against it is dropped, a real layout transition, thrown away
    // silently. Live-measured before this line existed: 8450 dropped
    // barriers in 30 seconds, every one of them against a retired handle
    // and not one against a null one, i.e. all of them wrong.
    //
    // vkGetSwapchainImagesKHR already does this, for the same reason and
    // with the same comment. Ordinary images were the case it missed.
    l.retired_images.erase(to_u64(image));
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

// STUD_VK_TRACE_PASSES: what each render pass targets, and the layout
// transitions on swapchain images. Read once, from the three places that
// report them -- the swapchain-image barriers included, which is why the
// barrier site below reads this and not STUD_VK_TRACE_BARRIERS (that one
// is for barriers Stud DROPS, a different question).
bool vk_trace_passes_enabled() {
    static const bool on = std::getenv("STUD_VK_TRACE_PASSES") != nullptr;
    return on;
}

// One thread at a time on the surface.
//
// vkCreateSwapchainKHR is deliberately outside the dispatch lock -- it
// blocks in the driver for as long as the driver feels like, and holding
// the lock across that froze render-host whole. But that also let a
// swapchain build run at the same time as the engine's own queries about
// the same surface, from the thread its other connection is served on.
//
// Live-caught on Wayland, inside a rebuild: two
// vkGetPhysicalDeviceSurfaceFormatsKHR calls came back
// VK_ERROR_INITIALIZATION_FAILED for a surface that had answered all
// session, and the engine -- which does not check that result -- walked
// the empty format list into a null dereference. Stud's own recovery then
// killed the thread, which is why the window vanished while the process,
// its audio and its tray carried on.
//
// So the surface gets a lock of its own: narrow enough that the create
// still does not block everything else in render-host, and enough that
// nobody asks the driver about a surface while it is building a swapchain
// for it.
// Readers share it; only a swapchain build excludes them.
//
// The engine asks about this surface constantly -- 5100 capability
// queries in one measured session -- and those have never needed to be
// serialised against EACH OTHER, only against a build. A plain mutex made
// every one of them wait for the last, on the frame path, which is a cost
// this never meant to add.
std::shared_mutex& surface_mutex() {
    static std::shared_mutex m;
    return m;
}

uint64_t vk_get_physical_device_surface_capabilities(uint64_t physical_device, uint64_t surface,
                                                      std::vector<uint8_t>& out,
                                                      uint32_t* out_len) {
    Loader& l = loader();
    if (l.get_physical_device_surface_capabilities == nullptr) {
        return static_cast<uint64_t>(static_cast<int32_t>(VK_ERROR_INITIALIZATION_FAILED));
    }
    VkSurfaceCapabilitiesKHR caps{};
    std::shared_lock<std::shared_mutex> surface_lock(surface_mutex());
    VkResult r = l.get_physical_device_surface_capabilities(
        reinterpret_cast<VkPhysicalDevice>(static_cast<uintptr_t>(physical_device)),
        from_u64<VkSurfaceKHR>(surface), &caps);

    // Wayland has no inherent surface size, so the driver reports
    // currentExtent = 0xFFFFFFFF x 0xFFFFFFFF, the spec's "the
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
    // X11 answers this question itself, and its answer is the wrong one.
    //
    // A Wayland surface has no size of its own, so the driver returns the
    // sentinel and Stud says how big the engine's buffer is. An X surface
    // IS a window, so the driver returns the window's real device size --
    // and the engine then renders at that size, which is the whole
    // resolution, leaving the upscaler nothing to scale and HiDPI nothing
    // to do. Substituting here puts both backends back on the same
    // footing: the engine renders into the buffer Stud sized for it, and
    // the pass covers the difference.
    const bool substitute_extent =
        stud::android_glue::display_backend() == stud::android_glue::DisplayBackend::X11 ||
        caps.currentExtent.width == kUndefinedExtent ||
        caps.currentExtent.height == kUndefinedExtent;
    if (substitute_extent) {
        // NEVER call ANativeWindow_fromSurface(nullptr, nullptr) here.
        // With a null Surface it only returns the existing window while
        // the window cache is non-empty, and a null Surface never
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
            // surface capabilities constantly, measured at 5100 lines
            // of one 9700-line session log, so half the log was this
            // sentence, and the answer only carries information when
            // it changes.
            static uint32_t announced_w = 0;
            static uint32_t announced_h = 0;
            if (w != announced_w || h != announced_h) {
                announced_w = w;
                announced_h = h;
                std::printf("stud-render-host: reporting the engine's own surface size "
                            "%ux%u\n", w, h);
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

// A VkQueue may only be touched by one thread at a time.
//
// The spec says so plainly: vkQueueSubmit, vkQueuePresentKHR and
// vkQueueWaitIdle all list the queue as "externally synchronized", which
// means the application is the one that has to serialise them. Stud had
// nothing. Its dispatch mutex looks like it would cover this and does
// not: calls that block in the driver deliberately run WITHOUT it (or a
// blocking present would hold every other connection still), and
// vkQueuePresentKHR is exactly such a call, while vkQueueSubmit is not.
// So the engine's submit ran under the dispatch mutex on one thread
// while Stud's present -- and, with upscaling on, Stud's own upscale
// submit inside it -- ran on another, on the same queue, at the same
// time.
//
// That is undefined behaviour, and the way it shows up on a real driver
// is the GPU wedging: live-caught as
// `vkWaitForFences returned VK_ERROR_DEVICE_LOST` with the engine's own
// `DeviceRecovery reason=hung`, about ten seconds of frozen window while
// the driver waited out its timeout, then a clean recovery. Rare,
// timing-dependent, and far more likely with upscaling on, because that
// is what puts a second submit on the queue from the present thread.
std::mutex& queue_mutex() {
    static std::mutex m;
    return m;
}




uint64_t vk_device_wait_idle() {
    Loader& l = loader();
    if (l.device_wait_idle == nullptr) {
        return static_cast<uint64_t>(static_cast<int32_t>(VK_ERROR_INITIALIZATION_FAILED));
    }
    // NOT under the queue lock, deliberately, and this was a deadlock.
    //
    // Taking it here looks right -- vkDeviceWaitIdle is externally
    // synchronised against every queue on the device -- but it waits for
    // the GPU to go idle, and the thread it waits behind is the one
    // inside vkQueuePresentKHR, which holds the lock while the driver
    // blocks. During a resize the engine calls this on every swapchain
    // rebuild, so a drag put the two on top of each other: the wait could
    // not start until the present finished and the present could not
    // finish. Live-caught as the render connection stopping dead --
    // `dispatch call=154` for five seconds and a black window that never
    // came back.
    //
    // What the lock is actually for is two threads issuing queue work at
    // the same instant, and Stud's own submits and presents still take
    // it. A wait for idle issues nothing.
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

// The driver's own rule for importing host pointers. Queried once: it is a
// physical-device property, and the import path hits it on every shared
// allocation.
VkDeviceSize imported_host_pointer_alignment() {
    Loader& l = loader();
    static VkDeviceSize cached = [&]() -> VkDeviceSize {
        if (l.get_physical_device_properties2 == nullptr) return 0;
        VkPhysicalDeviceExternalMemoryHostPropertiesEXT host{};
        host.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTERNAL_MEMORY_HOST_PROPERTIES_EXT;
        VkPhysicalDeviceProperties2 props{};
        props.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2;
        props.pNext = &host;
        l.get_physical_device_properties2(l.physical_device, &props);
        return host.minImportedHostPointerAlignment;
    }();
    return cached;
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
    // memory means the engine's writes are the device's memory; nothing
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
        // streaming buffers, which is exactly the memory it rewrites
        // every frame, and so exactly what was still being copied.
        //
        // STUD_SHARE_ALL_HOST_MEMORY=1 allocates those from an importable
        // host-visible type instead. The trade is real and worth stating:
        // the GPU then reads that memory across PCIe rather than from
        // VRAM. On this workload the CPU is the constraint (17 ms against
        // 6 ms of GPU), so paying the GPU to stop the CPU copying is the
        // right way round: but it is a trade, not a free win, which is
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
            // An imported host pointer carries its own size rule: the
            // allocation must be a whole number of the driver's import
            // alignment, and the engine's size is whatever it asked for.
            // Live-caught by validation: 2310400 bytes against a 4096
            // alignment. Rounding up is safe because mmap already gave
            // whole pages, so the extra bytes are mapped memory; leaving
            // it unrounded is undefined behaviour in the one subsystem
            // that hands the device its data.
            const VkDeviceSize align = imported_host_pointer_alignment();
            if (align > 1) {
                const VkDeviceSize rounded = ((size + align - 1) / align) * align;
                if (rounded != size) ai.allocationSize = rounded;
            }
            import.sType = VK_STRUCTURE_TYPE_IMPORT_MEMORY_HOST_POINTER_INFO_EXT;
            import.handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_HOST_ALLOCATION_BIT_EXT;
            import.pHostPointer = mapping.address;
            import.pNext = ai.pNext;
            ai.pNext = &import;
        } else {
            static std::set<uint32_t> reported;
            if (reported.insert(type_index).second) {
                std::printf("stud-render-host: memory type %u is not host-importable "
                            "(driver allows types 0x%x); this one is copied\n",
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
        // pointer and then refused the import anyway, live-caught on
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
                        "refused it (%d); this one is copied\n",
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
    // reading those pages. Getting this wrong is silent and total, the
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

// Defined below; vk_free_memory() drops this mapping too.
std::map<uint64_t, std::pair<void*, size_t>>& shared_writes();

uint64_t vk_free_memory(uint64_t memory) {
    Loader& l = loader();
    if (l.free_memory == nullptr) return 0;
    l.mapped.erase(memory);
    l.mapped_size.erase(memory);
    l.free_memory(l.device, from_u64<VkDeviceMemory>(memory), nullptr);
    // The import keeps the pages alive as long as the memory object does,
    // so the mapping is dropped only now.
    vk_free_memory_shared_cleanup(memory);
    // ...and so is the write-sharing mmap for the same memory. This was
    // the one table with no erase site at all: every mapping made by
    // vk_share_mapped_memory() stayed for the life of the process, and
    // was only ever unmapped if the very same VkDeviceMemory handle value
    // happened to be shared a second time. Address space and map nodes
    // both leaked, on every session, with or without a device loss.
    {
        auto w = shared_writes().find(memory);
        if (w != shared_writes().end()) {
            if (w->second.first != nullptr) ::munmap(w->second.first, w->second.second);
            shared_writes().erase(w);
        }
    }
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
    l.mapped_size[memory] = size == VK_WHOLE_SIZE ? 0 : size;
    return static_cast<uint64_t>(static_cast<int32_t>(VK_SUCCESS));
}

// A mapped allocation whose pages the client and this process both map.
//
// The engine writes through its own mapping of a file; this process maps
// the same file, so the bytes are already here by the time a flush names
// the run that changed. What used to be 12.5 MB a frame down the socket
// becomes an offset and a length, and one memcpy on this side.
std::map<uint64_t, std::pair<void*, size_t>>& shared_writes() {
    static std::map<uint64_t, std::pair<void*, size_t>> m;
    return m;
}

uint64_t vk_share_mapped_memory(uint64_t memory, uint64_t shared_id, uint64_t size) {
    const std::string path = stud::render_host::shared_memory_path(shared_id);
    const int fd = ::open(path.c_str(), O_RDWR | O_CLOEXEC);
    if (fd < 0) return static_cast<uint64_t>(static_cast<int32_t>(VK_ERROR_MEMORY_MAP_FAILED));
    const size_t page = static_cast<size_t>(::sysconf(_SC_PAGESIZE));
    const size_t length = (static_cast<size_t>(size) + page - 1) & ~(page - 1);
    void* p = ::mmap(nullptr, length, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    ::close(fd);
    if (p == MAP_FAILED) {
        return static_cast<uint64_t>(static_cast<int32_t>(VK_ERROR_MEMORY_MAP_FAILED));
    }
    auto& slot = shared_writes()[memory];
    if (slot.first != nullptr) ::munmap(slot.first, slot.second);
    slot = {p, length};
    static bool said = false;
    if (!said) {
        said = true;
        std::printf("stud-render-host: the engine's mapped writes are shared, not copied\n");
        std::fflush(stdout);
    }
    return static_cast<uint64_t>(static_cast<int32_t>(VK_SUCCESS));
}

// The same write, with the bytes already in this process: copy the named
// run out of the shared mapping into the real one.
uint64_t vk_write_shared_mapped_memory(uint64_t memory, uint64_t offset, uint64_t n) {
    Loader& l = loader();
    auto it = l.mapped.find(memory);
    auto sh = shared_writes().find(memory);
    if (it == l.mapped.end() || it->second == nullptr || sh == shared_writes().end() ||
        sh->second.first == nullptr) {
        return static_cast<uint64_t>(static_cast<int32_t>(VK_ERROR_MEMORY_MAP_FAILED));
    }
    if (offset + n > sh->second.second) {
        return static_cast<uint64_t>(static_cast<int32_t>(VK_ERROR_MEMORY_MAP_FAILED));
    }
    std::memcpy(static_cast<uint8_t*>(it->second) + offset,
                static_cast<const uint8_t*>(sh->second.first) + offset,
                static_cast<size_t>(n));
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

uint64_t vk_read_mapped_memory(uint64_t memory, uint64_t offset, uint64_t size,
                               std::vector<uint8_t>& out, uint32_t* out_len) {
    Loader& l = loader();
    auto it = l.mapped.find(memory);
    if (it == l.mapped.end() || it->second == nullptr) {
        return static_cast<uint64_t>(static_cast<int32_t>(VK_ERROR_MEMORY_MAP_FAILED));
    }
    // The caller asks for a range of its own allocation; this process
    // knows only that it mapped the whole thing, so a request past the end
    // is the caller's bug and is refused rather than read.
    const auto size_it = l.mapped_size.find(memory);
    const uint64_t mapped_size = size_it == l.mapped_size.end() ? 0 : size_it->second;
    if (mapped_size != 0 && offset + size > mapped_size) {
        return static_cast<uint64_t>(static_cast<int32_t>(VK_ERROR_MEMORY_MAP_FAILED));
    }
    out.resize(static_cast<size_t>(size));
    std::memcpy(out.data(), static_cast<const uint8_t*>(it->second) + offset,
                static_cast<size_t>(size));
    *out_len = static_cast<uint32_t>(size);
    return static_cast<uint64_t>(static_cast<int32_t>(VK_SUCCESS));
}

uint64_t vk_unmap_memory(uint64_t memory) {
    Loader& l = loader();
    if (l.unmap_memory == nullptr) return 0;
    l.unmap_memory(l.device, from_u64<VkDeviceMemory>(memory));
    l.mapped.erase(memory);
    l.mapped_size.erase(memory);
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
    std::shared_lock<std::shared_mutex> surface_lock(surface_mutex());
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
    std::shared_lock<std::shared_mutex> surface_lock(surface_mutex());
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
    std::shared_lock<std::shared_mutex> surface_lock(surface_mutex());
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
// frame at each refresh. No tearing, no ceiling, and IMMEDIATE is the
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

// ---- Stud's own upscale, between the engine's last draw and the present
//
// The engine is handed offscreen images the size it asked for and never
// learns they are not the swapchain's own. Stud owns the real swapchain,
// at the window's full resolution, and builds each presented frame from
// the engine's image. See vk_set_upscale_ratio_120() in the header for
// why the engine's own scale cannot be used instead.
//
// Phase one is a linear blit, which is what the compositor was already
// doing to the same image, so it must look identical to not upscaling
// at all. The pass is replaced with EASU+RCAS once this sync is proven.
struct UpscaleChain {
    VkSwapchainKHR real = VK_NULL_HANDLE;
    // Set only on a retired chain: the swapchain whose images this pass
    // is still writing into, to be destroyed once it has finished. See
    // sweep_retired_chains().
    VkSwapchainKHR destroy_with_chain = VK_NULL_HANDLE;
    VkExtent2D engine{};
    VkExtent2D present{};
    VkFormat format = VK_FORMAT_UNDEFINED;
    std::vector<VkImage> offscreen;
    std::vector<VkDeviceMemory> memory;
    std::vector<VkImage> real_images;
    VkCommandPool pool = VK_NULL_HANDLE;
    // One pre-recorded command buffer per image: the blit is identical
    // every frame, so nothing has to be re-recorded and no command-buffer
    // reset is needed. The fence is what says the previous submit of THIS
    // buffer has finished, which a re-submit requires.
    std::vector<VkCommandBuffer> cmd;
    // One per image, recorded once: nothing but the barrier that makes the
    // swapchain image presentable. Submitted when the upscale pass is
    // skipped, because the ONLY transition to PRESENT_SRC_KHR lives inside
    // the pass's own command buffer -- so a skipped frame used to present
    // an image in whatever layout it last held, and
    // VK_IMAGE_LAYOUT_UNDEFINED for an index the pass had never run on.
    std::vector<VkCommandBuffer> cmd_present_only;
    std::vector<VkSemaphore> done;
    // Semaphores a failed present may have left signalled.
    //
    // The pass signals done[index] and the present is what consumes it.
    // When the present fails -- which on X11 it does on every resize --
    // nothing consumed it, so it is still signalled, and signalling a
    // binary semaphore that is already signalled wedges the queue for
    // good: the device then never goes idle and the engine's own
    // vkDeviceWaitIdle never returns. Live-caught exactly that way.
    //
    // So the semaphore is retired on the spot and a fresh one takes its
    // place. Retiring rather than destroying because the submit that
    // signalled it may still be running; these are destroyed with the
    // rest of the chain, by which time nothing can be using them.
    std::vector<VkSemaphore> retired;
    // Set when a present did not return SUCCESS or SUBOPTIMAL.
    //
    // A failed present leaves the semaphores it was told to wait on in an
    // UNDEFINED state: the spec does not say whether they were consumed.
    // The pass signals `done[index]` every frame, so if the present that
    // should have consumed it did not, the next submit signals a binary
    // semaphore that is already signalled -- which wedges the queue, and a
    // wedged queue never goes idle, so the engine's own vkDeviceLoseIdle
    // on the next swapchain rebuild never returns. That is a resize
    // killing the window, live-caught twice: OUT_OF_DATE from acquire,
    // then `dispatch call=154` (vkDeviceWaitIdle) stuck for ever.
    bool semaphores_tainted = false;
    std::vector<VkFence> fence;
    std::vector<bool> in_flight;
    // The real upscale: one compute dispatch per frame, reading the
    // engine's image through a sampler and writing the swapchain's.
    // Absent (VK_NULL_HANDLE) when the pass could not be built, in which
    // case the recorded command buffers hold a plain blit instead, an
    // honest degrade rather than a black window.
    bool compute = false;
    // Whether the sharpening pass is in the chain at all. Off when
    // sharpness is zero, in which case EASU's output goes straight to the
    // swapchain and the second image and dispatch are never created.
    bool sharpen = false;
    VkShaderModule shader = VK_NULL_HANDLE;
    VkShaderModule sharpen_shader = VK_NULL_HANDLE;
    VkPipeline sharpen_pipeline = VK_NULL_HANDLE;
    std::vector<VkDescriptorSet> sharpen_sets;
    std::vector<VkImageView> sharpen_src_views;
    std::vector<VkImageView> sharpen_dst_views;
    // EASU's output, which RCAS then reads. Separate from `staging`
    // because a compute pass cannot read and write the same image.
    std::vector<VkImage> sharpened;
    std::vector<VkDeviceMemory> sharpened_memory;
    VkSampler sampler = VK_NULL_HANDLE;
    VkDescriptorSetLayout set_layout = VK_NULL_HANDLE;
    VkPipelineLayout pipeline_layout = VK_NULL_HANDLE;
    VkPipeline pipeline = VK_NULL_HANDLE;
    VkDescriptorPool descriptor_pool = VK_NULL_HANDLE;
    VkDescriptorPool sharpen_pool = VK_NULL_HANDLE;
    std::vector<VkDescriptorSet> sets;
    std::vector<VkImageView> src_views;
    std::vector<VkImageView> dst_views;
    // The compute pass writes HERE, not straight into the swapchain.
    //
    // A swapchain image on this driver is B8G8R8A8_UNORM, and storage-image
    // writes are not guaranteed for BGRA formats, nor does a swapchain
    // image carry STORAGE usage unless asked, which the surface need not
    // support. Writing into a plain R8G8B8A8_UNORM image (where storage
    // support IS mandatory) and then blitting 1:1 into the swapchain costs
    // one full-resolution copy and makes the channel order the blit's
    // problem, which it handles by definition. Live-caught as a black
    // window: the dispatch ran, wrote nothing the presentation engine
    // could show.
    std::vector<VkImage> staging;
    std::vector<VkDeviceMemory> staging_memory;
};

// What the shader needs to know, and all it needs to know.
struct UpscalePush {
    int32_t src_w;
    int32_t src_h;
    int32_t dst_w;
    int32_t dst_h;
    float sharpness;
};

std::map<uint64_t, UpscaleChain> g_upscale_chains;

// True only for images the presentation engine actually owns.
//
// With upscaling on, the engine is handed Stud's OWN images as its
// swapchain images (vk_get_swapchain_images) and renders into those; the
// real, presentable ones are the chain's real_images and the engine never
// sees them. So "the engine thinks this is a swapchain image" and "this
// image can be presented" are different questions, and PRESENT_SRC_KHR is
// only a legal layout for the second.
bool is_presentable_image(VkImage image) {
    for (const auto& entry : g_upscale_chains) {
        for (VkImage real : entry.second.real_images) {
            if (real == image) return true;
        }
        for (VkImage fake : entry.second.offscreen) {
            if (fake == image) return false;   // ours: never presentable
        }
    }
    // No upscale chain: the engine renders straight into the real
    // swapchain, which is exactly what it believes it is doing.
    return true;
}

// Makes a forwarded barrier LEGAL without making it weaker.
//
// The engine records its own barriers and Stud replays them. Validation
// on a real run says some of them are invalid: an access flag that the
// stage it names cannot perform (VUID-vkCmdPipelineBarrier-
// pImageMemoryBarriers-02819 -- SHADER_READ under ALL_TRANSFER), and a
// transition into TRANSFER_DST that does not cover the copy that
// immediately follows it (SYNC-HAZARD-WRITE-AFTER-WRITE on
// vkCmdCopyImage). A lenient driver runs them anyway. NVIDIA does not:
// every run of this session that put real work through this path on the
// RTX 3050 ended in VK_ERROR_DEVICE_LOST, while the identical code on
// Intel ran 41 minutes without one.
//
// Both repairs only ever ADD synchronisation, never remove it, so a
// barrier that was already correct is unchanged in effect:
//   - a stage mask that cannot perform the access it is given is widened
//     to ALL_COMMANDS, which can perform all of them;
//   - a transition into TRANSFER_DST/TRANSFER_SRC gains the transfer
//     access and stage it is missing, so the copy that follows is
//     ordered after the transition rather than racing it.
void make_barrier_legal(VkPipelineStageFlags* src_stage, VkPipelineStageFlags* dst_stage,
                        std::vector<VkMemoryBarrier>& mem,
                        std::vector<VkBufferMemoryBarrier>& buf,
                        std::vector<VkImageMemoryBarrier>& img) {
    // What each stage is allowed to do, kept deliberately coarse: the
    // question here is only "could this stage ever perform this access",
    // and a wrong answer is caught by validation, not guessed at.
    const VkAccessFlags transfer_access =
        VK_ACCESS_TRANSFER_READ_BIT | VK_ACCESS_TRANSFER_WRITE_BIT |
        VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT | VK_ACCESS_HOST_READ_BIT |
        VK_ACCESS_HOST_WRITE_BIT;
    const VkPipelineStageFlags transfer_only =
        VK_PIPELINE_STAGE_TRANSFER_BIT |
        VK_PIPELINE_STAGE_HOST_BIT | VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT |
        VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT;

    auto stage_cannot = [&](VkPipelineStageFlags stage, VkAccessFlags access) {
        if ((stage & ~transfer_only) != 0) return false;   // a general stage: fine
        if ((stage & VK_PIPELINE_STAGE_ALL_COMMANDS_BIT) != 0) return false;
        return (access & ~transfer_access) != 0;
    };

    VkAccessFlags all_src = 0;
    VkAccessFlags all_dst = 0;
    for (const auto& b : mem) { all_src |= b.srcAccessMask; all_dst |= b.dstAccessMask; }
    for (const auto& b : buf) { all_src |= b.srcAccessMask; all_dst |= b.dstAccessMask; }
    for (const auto& b : img) { all_src |= b.srcAccessMask; all_dst |= b.dstAccessMask; }

    static int widened = 0;
    if (stage_cannot(*src_stage, all_src) || stage_cannot(*dst_stage, all_dst)) {
        *src_stage |= VK_PIPELINE_STAGE_ALL_COMMANDS_BIT;
        *dst_stage |= VK_PIPELINE_STAGE_ALL_COMMANDS_BIT;
        if (widened < 4) {
            ++widened;
            std::printf("stud-render-host: widened a barrier whose stage could not perform the "
                        "access it was given (src=0x%x dst=0x%x)\n",
                        static_cast<unsigned>(all_src), static_cast<unsigned>(all_dst));
            std::fflush(stdout);
        }
    }

    // PRESENT_SRC_KHR on an image that cannot be presented.
    //
    // The spec allows that layout only for a presentable image, and with
    // upscaling on the engine's render target is not one: it is an
    // ordinary image of Stud's, handed over as if it came from the
    // swapchain, so the engine ends every frame by transitioning it to
    // PRESENT_SRC. On a driver where that layout is a no-op nothing is
    // ever wrong. On NVIDIA it is not a no-op -- the layout carries
    // display-specific tiling and compression -- and it was being applied
    // to an allocation that can never reach the screen. Every device loss
    // this session had this pass running; none of the runs without it did,
    // and the same code on Intel ran clean for 41 minutes.
    //
    // COLOR_ATTACHMENT_OPTIMAL is what the image really is, and the
    // engine's own next render pass expects to find it in that layout
    // either way, so the substitution is invisible to it.
    for (auto& b : img) {
        if (b.image == VK_NULL_HANDLE || is_presentable_image(b.image)) continue;
        static int remapped = 0;
        const bool interesting = b.oldLayout == VK_IMAGE_LAYOUT_PRESENT_SRC_KHR ||
                                 b.newLayout == VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
        if (interesting && remapped < 4) {
            ++remapped;
            std::printf("stud-render-host: an image that cannot be presented was being put in "
                        "PRESENT_SRC; using COLOR_ATTACHMENT_OPTIMAL instead\n");
            std::fflush(stdout);
        }
        if (b.oldLayout == VK_IMAGE_LAYOUT_PRESENT_SRC_KHR) {
            b.oldLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        }
        if (b.newLayout == VK_IMAGE_LAYOUT_PRESENT_SRC_KHR) {
            b.newLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        }
    }

    // A transition INTO a transfer layout is followed by a transfer. Say
    // so, or the copy is a second write racing the transition's own.
    bool needs_transfer = false;
    for (auto& b : img) {
        if (b.newLayout == VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL) {
            b.dstAccessMask |= VK_ACCESS_TRANSFER_WRITE_BIT;
            needs_transfer = true;
        } else if (b.newLayout == VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL) {
            b.dstAccessMask |= VK_ACCESS_TRANSFER_READ_BIT;
            needs_transfer = true;
        }
    }
    if (needs_transfer) *dst_stage |= VK_PIPELINE_STAGE_TRANSFER_BIT;
}

// Everything the pass owns EXCEPT the images the engine renders into.
//
// Those came out of vkGetSwapchainImagesKHR as far as the engine knows,
// so it holds them, makes views and framebuffers of them, and would be
// left with dangling handles if a rebuild threw them away. Everything on
// the window side -- the real images' views, the recorded command
// buffers, the descriptor sets that name them -- belongs to one real
// swapchain and goes with it.
void destroy_upscale_chain_keeping_offscreen(UpscaleChain& c);

// Waits for the pass's OWN work to finish, and says whether it did.
//
// This used to be a plain vkDeviceWaitIdle, and on X11 that is what
// turned a resize into a dead window: vkCreateSwapchainKHR tears the old
// chain down, the teardown waited for the whole device to go idle, and
// mid-resize the device cannot -- a submission is sitting on a semaphore
// a failed acquire never signalled, or a present the X server has not
// retired while the window manager is moving the window. Live-caught, as
// the connection serving vkCreateSwapchainKHR not moving for 5.3s and
// never moving again.
//
// The device going idle was never the question anyway. What has to be
// true before these objects can be destroyed is that the pass's own
// command buffers are done, and the pass records its own fence for every
// one of them. So those are what is waited on, with a bound.
bool upscale_work_finished(UpscaleChain& c, uint64_t timeout_ns) {
    Loader& l = loader();
    if (l.wait_for_fences == nullptr) return true;
    std::vector<VkFence> pending;
    for (size_t i = 0; i < c.fence.size(); ++i) {
        if (i < c.in_flight.size() && c.in_flight[i] && c.fence[i] != VK_NULL_HANDLE) {
            pending.push_back(c.fence[i]);
        }
    }
    if (pending.empty()) return true;
    return l.wait_for_fences(l.device, static_cast<uint32_t>(pending.size()), pending.data(),
                             VK_TRUE, timeout_ns) == VK_SUCCESS;
}

// Chains whose GPU work had not finished when they were torn down.
//
// Destroying them anyway is a use-after-free the driver is entitled to
// crash on, and waiting for ever is the hang above, so they are kept and
// swept later -- the frame that replaced them does not need them gone,
// only unused.
std::vector<UpscaleChain> g_retired_chains;
// Whether that vector has anything in it, readable without taking the
// lock. The sweep runs on every present, and every present but the few
// after a rebuild has nothing to sweep -- so the common case is one
// relaxed atomic load and nothing else.
std::atomic<bool> g_have_retired_chains{false};
// Guards it. A retired chain is pushed from whichever thread destroyed a
// swapchain and swept from whichever thread next builds or presents one,
// and vkCreateSwapchainKHR is deliberately exempt from the dispatch lock
// (it blocks in the driver), so those are genuinely different threads.
std::mutex g_retired_chains_mutex;

// `force` destroys the chain even if its GPU work has not finished,
// instead of deferring it again. Only for the points where deferring
// is itself the bug: see flush_retired_chains().
void destroy_upscale_chain(UpscaleChain& c, bool force = false);

// Frees whatever retired chains the GPU has since finished with. Called
// wherever a new chain is built, which is the only place they accumulate.
void sweep_retired_chains() {
    if (!g_have_retired_chains.load(std::memory_order_relaxed)) return;
    Loader& l = loader();
    if (l.device == VK_NULL_HANDLE) return;
    std::lock_guard<std::mutex> lock(g_retired_chains_mutex);
    for (size_t i = 0; i < g_retired_chains.size();) {
        if (upscale_work_finished(g_retired_chains[i], 0)) {
            UpscaleChain done = std::move(g_retired_chains[i]);
            g_retired_chains.erase(g_retired_chains.begin() + static_cast<long>(i));
            done.in_flight.assign(done.in_flight.size(), false);
            const VkSwapchainKHR held = done.destroy_with_chain;
            done.destroy_with_chain = VK_NULL_HANDLE;
            destroy_upscale_chain(done);
            // The swapchain this pass was writing into, held back until
            // now for exactly that reason.
            if (held != VK_NULL_HANDLE && l.destroy_swapchain != nullptr) {
                l.destroy_swapchain(l.device, held, nullptr);
            }
        } else {
            ++i;
        }
    }
    g_have_retired_chains.store(!g_retired_chains.empty(), std::memory_order_relaxed);
}

// Finishes every retired chain now, whether or not the GPU has let go.
//
// Deferring a chain is right while the renderer carries on -- the frame
// that replaced it does not need it gone, only unused -- but there are
// two moments where deferring is itself the bug, because the thing the
// chain is holding stops being destroyable:
//
//   * the engine destroys the surface. A swapchain still alive on that
//     surface makes destroying it undefined, and the Wayland WSI does not
//     forgive it: live-caught as a window backgrounded mid-game, a
//     present that blocked 12006ms, the engine tearing its RenderView
//     down while the pass was still running ("keeping it until the GPU is
//     done with it"), and then the rebuild's own
//     vkCreateSwapchainKHR -> VK_ERROR_SURFACE_LOST_KHR. The engine does
//     not check that one: it dereferenced null on the next line, Stud
//     recovered the fault by exiting that thread, and the render died
//     while the process and its tray icon stayed up.
//   * the engine builds a new device. l.device is replaced, and a chain
//     built on the outgoing device could then only be destroyed with a
//     device it was never created on.
//
// So both points call this, and it waits the same bounded second
// destroy_upscale_chain() would have, then destroys regardless. The
// alternative at these two points is not "wait a little longer", it is a
// swapchain that can never be destroyed at all.
void flush_retired_chains(const char* why) {
    if (!g_have_retired_chains.load(std::memory_order_relaxed)) return;
    Loader& l = loader();
    if (l.device == VK_NULL_HANDLE) return;
    std::vector<UpscaleChain> taken;
    {
        std::lock_guard<std::mutex> lock(g_retired_chains_mutex);
        taken.swap(g_retired_chains);
        g_have_retired_chains.store(false, std::memory_order_relaxed);
    }
    if (taken.empty()) return;
    std::printf("stud-render-host: finishing %zu retired upscale pass(es) before %s\n",
                taken.size(), why);
    std::fflush(stdout);
    for (UpscaleChain& c : taken) {
        // Give the GPU the same second it would have had, then take the
        // chain apart whether it used it or not.
        const bool quiet = upscale_work_finished(c, 1000000000ull);
        if (!quiet) {
            std::printf("stud-render-host: a retired upscale pass was still running; destroying it "
                        "anyway, its surface is going away\n");
            std::fflush(stdout);
        }
        c.in_flight.assign(c.in_flight.size(), false);
        const VkSwapchainKHR held = c.destroy_with_chain;
        c.destroy_with_chain = VK_NULL_HANDLE;
        destroy_upscale_chain(c, /*force=*/true);
        if (held != VK_NULL_HANDLE && l.destroy_swapchain != nullptr) {
            l.destroy_swapchain(l.device, held, nullptr);
        }
    }
}

void destroy_upscale_chain(UpscaleChain& c, bool force) {
    Loader& l = loader();
    if (l.device == VK_NULL_HANDLE) return;
    // One second is far longer than any frame this pass records, and far
    // shorter than "never".
    if (!force && !upscale_work_finished(c, 1000000000ull)) {
        static int said = 0;
        if (said < 8) {
            ++said;
            std::printf("stud-render-host: the upscale pass was still running when its swapchain "
                        "went away; keeping it until the GPU is done with it\n");
            std::fflush(stdout);
        }
        {
            std::lock_guard<std::mutex> lock(g_retired_chains_mutex);
            g_retired_chains.push_back(std::move(c));
            g_have_retired_chains.store(true, std::memory_order_relaxed);
        }
        c = UpscaleChain{};
        return;
    }
    // Every line below was the same three: check the handle, check the
    // entry point the driver may not have, then call it. Fifteen copies of
    // that is where this function's complexity came from, not the work it
    // does.
    //
    // Deliberately does NOT null the handle afterwards, because the
    // original did not either: `c` is assigned a fresh UpscaleChain at the
    // end, which is what clears it, and a half-cleared chain in between
    // would be a different thing than what shipped.
    const auto destroy = [&l](auto fn, auto handle) {
        if (handle != VK_NULL_HANDLE && fn != nullptr) fn(l.device, handle, nullptr);
    };
    const auto destroy_all = [&destroy](auto fn, const auto& handles) {
        for (auto h : handles) destroy(fn, h);
    };

    destroy_all(l.destroy_image_view, c.src_views);
    destroy_all(l.destroy_image_view, c.dst_views);
    destroy(l.destroy_descriptor_pool, c.descriptor_pool);
    destroy(l.destroy_descriptor_pool, c.sharpen_pool);
    destroy(l.destroy_pipeline, c.pipeline);
    destroy(l.destroy_pipeline_layout, c.pipeline_layout);
    destroy(l.destroy_descriptor_set_layout, c.set_layout);
    destroy(l.destroy_sampler, c.sampler);
    destroy(l.destroy_shader_module, c.shader);
    destroy(l.destroy_shader_module, c.sharpen_shader);
    destroy(l.destroy_pipeline, c.sharpen_pipeline);
    destroy_all(l.destroy_image_view, c.sharpen_src_views);
    destroy_all(l.destroy_image_view, c.sharpen_dst_views);
    destroy_all(l.destroy_image, c.sharpened);
    destroy_all(l.free_memory, c.sharpened_memory);
    destroy_all(l.destroy_fence, c.fence);
    destroy_all(l.destroy_semaphore, c.done);
    destroy(l.destroy_command_pool, c.pool);
    destroy_all(l.destroy_image, c.offscreen);
    destroy_all(l.destroy_image, c.staging);
    destroy_all(l.free_memory, c.memory);
    destroy_all(l.free_memory, c.staging_memory);
    destroy_all(l.destroy_semaphore, c.retired);
    c = UpscaleChain{};
}

void destroy_upscale_chain_keeping_offscreen(UpscaleChain& c) {
    // The engine's images and their memory are lifted out, the rest is
    // torn down exactly as usual, and they are put back.
    std::vector<VkImage> offscreen;
    std::vector<VkDeviceMemory> memory;
    offscreen.swap(c.offscreen);
    memory.swap(c.memory);
    const VkExtent2D engine = c.engine;
    const VkFormat format = c.format;
    destroy_upscale_chain(c);
    c.offscreen.swap(offscreen);
    c.memory.swap(memory);
    c.engine = engine;
    c.format = format;
}

// Device-local memory for an offscreen colour target. No host access is
// wanted here: the engine renders into it and Stud reads it on the GPU.
bool allocate_offscreen_memory(VkImage image, VkDeviceMemory& memory) {
    Loader& l = loader();
    if (l.get_image_memory_requirements == nullptr || l.allocate_memory == nullptr ||
        l.bind_image_memory == nullptr || l.get_physical_device_memory_properties == nullptr) {
        return false;
    }
    VkMemoryRequirements req{};
    l.get_image_memory_requirements(l.device, image, &req);
    VkPhysicalDeviceMemoryProperties props{};
    l.get_physical_device_memory_properties(l.physical_device, &props);
    uint32_t chosen = UINT32_MAX;
    for (uint32_t i = 0; i < props.memoryTypeCount; ++i) {
        const bool usable = (req.memoryTypeBits & (1u << i)) != 0;
        const bool device_local = (props.memoryTypes[i].propertyFlags &
                                   VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT) != 0;
        if (usable && device_local) { chosen = i; break; }
    }
    if (chosen == UINT32_MAX) {
        for (uint32_t i = 0; i < props.memoryTypeCount; ++i) {
            if ((req.memoryTypeBits & (1u << i)) != 0) { chosen = i; break; }
        }
    }
    if (chosen == UINT32_MAX) return false;
    VkMemoryAllocateInfo ai{};
    ai.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    ai.allocationSize = req.size;
    ai.memoryTypeIndex = chosen;
    if (l.allocate_memory(l.device, &ai, nullptr, &memory) != VK_SUCCESS) return false;
    return l.bind_image_memory(l.device, image, memory, 0) == VK_SUCCESS;
}

// Records one command buffer per image, once. Each is the same work every
// frame, read the engine's image, write the real one, so nothing needs
// re-recording and no command buffer ever has to be reset.
//
// The layouts are what a real swapchain image would be in at these
// points, which is what makes this invisible to the engine: it finishes
// its render pass leaving its image in PRESENT_SRC_KHR (it believes it is
// presenting), so that is the source layout here, and it is put back
// before the buffer is handed over again.
// The upscale pass: FSR1's algorithm (edge-adaptive resample, then
// contrast-adaptive sharpening) as one compute dispatch per frame.
//
// Vendor-neutral by construction. It is ordinary compute maths and runs
// the same on any GPU with Vulkan. There is deliberately no DLSS path:
// that needs motion vectors, depth and a jitter matrix from the renderer,
// which Stud cannot see (it forwards draw calls, not a G-buffer), plus a
// proprietary runtime. A vendor switch here would run the same shader
// down both branches.
bool build_upscale_compute(UpscaleChain& c) {
    Loader& l = loader();
    if (l.create_shader_module == nullptr || l.create_compute_pipelines == nullptr ||
        l.create_descriptor_set_layout == nullptr || l.create_pipeline_layout == nullptr ||
        l.create_descriptor_pool == nullptr || l.allocate_descriptor_sets == nullptr ||
        l.update_descriptor_sets == nullptr || l.create_sampler == nullptr ||
        l.create_image_view == nullptr || l.cmd_push_constants == nullptr ||
        l.cmd_bind_pipeline == nullptr || l.cmd_bind_descriptor_sets == nullptr ||
        l.cmd_dispatch == nullptr) {
        return false;
    }
    static const uint32_t kUpscaleSpv[] =
#include "upscale_spv.h"
        ;
    if (sizeof(kUpscaleSpv) < 32) return false;  // built without glslc

    VkShaderModuleCreateInfo smci{};
    smci.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
    smci.codeSize = sizeof(kUpscaleSpv);
    smci.pCode = kUpscaleSpv;
    if (l.create_shader_module(l.device, &smci, nullptr, &c.shader) != VK_SUCCESS) return false;

    // Linear filtering with clamped edges: the shader takes its own taps,
    // so the sampler only has to not wrap and not invent anything.
    VkSamplerCreateInfo sci{};
    sci.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
    sci.magFilter = VK_FILTER_LINEAR;
    sci.minFilter = VK_FILTER_LINEAR;
    sci.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
    sci.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    sci.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    sci.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    sci.maxLod = 0.0f;
    if (l.create_sampler(l.device, &sci, nullptr, &c.sampler) != VK_SUCCESS) return false;

    VkDescriptorSetLayoutBinding bindings[2]{};
    bindings[0].binding = 0;
    bindings[0].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    bindings[0].descriptorCount = 1;
    bindings[0].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
    bindings[1].binding = 1;
    bindings[1].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
    bindings[1].descriptorCount = 1;
    bindings[1].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
    VkDescriptorSetLayoutCreateInfo dslci{};
    dslci.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    dslci.bindingCount = 2;
    dslci.pBindings = bindings;
    if (l.create_descriptor_set_layout(l.device, &dslci, nullptr, &c.set_layout) != VK_SUCCESS) {
        return false;
    }

    VkPushConstantRange range{};
    range.stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
    range.offset = 0;
    range.size = sizeof(UpscalePush);
    VkPipelineLayoutCreateInfo plci{};
    plci.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    plci.setLayoutCount = 1;
    plci.pSetLayouts = &c.set_layout;
    plci.pushConstantRangeCount = 1;
    plci.pPushConstantRanges = &range;
    if (l.create_pipeline_layout(l.device, &plci, nullptr, &c.pipeline_layout) != VK_SUCCESS) {
        return false;
    }

    VkComputePipelineCreateInfo cpci{};
    cpci.sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO;
    cpci.stage.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    cpci.stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
    cpci.stage.module = c.shader;
    cpci.stage.pName = "main";
    cpci.layout = c.pipeline_layout;
    if (l.create_compute_pipelines(l.device, VK_NULL_HANDLE, 1, &cpci, nullptr, &c.pipeline) !=
        VK_SUCCESS) {
        return false;
    }

    const auto count = static_cast<uint32_t>(c.offscreen.size());
    VkDescriptorPoolSize sizes[2]{};
    sizes[0].type = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    sizes[0].descriptorCount = count;
    sizes[1].type = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
    sizes[1].descriptorCount = count;
    VkDescriptorPoolCreateInfo dpci{};
    dpci.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    dpci.maxSets = count;
    dpci.poolSizeCount = 2;
    dpci.pPoolSizes = sizes;
    if (l.create_descriptor_pool(l.device, &dpci, nullptr, &c.descriptor_pool) != VK_SUCCESS) {
        return false;
    }

    std::vector<VkDescriptorSetLayout> layouts(count, c.set_layout);
    VkDescriptorSetAllocateInfo dsai{};
    dsai.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
    dsai.descriptorPool = c.descriptor_pool;
    dsai.descriptorSetCount = count;
    dsai.pSetLayouts = layouts.data();
    c.sets.resize(count, VK_NULL_HANDLE);
    if (l.allocate_descriptor_sets(l.device, &dsai, c.sets.data()) != VK_SUCCESS) return false;

    c.src_views.resize(count, VK_NULL_HANDLE);
    c.dst_views.resize(count, VK_NULL_HANDLE);
    c.staging.resize(count, VK_NULL_HANDLE);
    c.staging_memory.resize(count, VK_NULL_HANDLE);
    for (uint32_t i = 0; i < count; ++i) {
        // The image the shader writes: R8G8B8A8_UNORM, which every Vulkan
        // implementation must support as a storage image, at the output
        // size. See UpscaleChain::staging.
        VkImageCreateInfo sii{};
        sii.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
        sii.imageType = VK_IMAGE_TYPE_2D;
        sii.format = VK_FORMAT_R8G8B8A8_UNORM;
        sii.extent = {c.present.width, c.present.height, 1};
        sii.mipLevels = 1;
        sii.arrayLayers = 1;
        sii.samples = VK_SAMPLE_COUNT_1_BIT;
        sii.tiling = VK_IMAGE_TILING_OPTIMAL;
        // Sampled too: when sharpening is on, the second pass reads this
        // image back.
        sii.usage = VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT |
                    VK_IMAGE_USAGE_SAMPLED_BIT;
        sii.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
        sii.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        if (l.create_image(l.device, &sii, nullptr, &c.staging[i]) != VK_SUCCESS) return false;
        if (!allocate_offscreen_memory(c.staging[i], c.staging_memory[i])) return false;

        VkImageViewCreateInfo vci{};
        vci.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
        vci.viewType = VK_IMAGE_VIEW_TYPE_2D;
        vci.format = c.format;
        vci.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        vci.image = c.offscreen[i];
        if (l.create_image_view(l.device, &vci, nullptr, &c.src_views[i]) != VK_SUCCESS) {
            return false;
        }
        // The storage view's format must match the shader's own qualifier
        // (rgba8), which is why this is not the swapchain's format.
        vci.format = VK_FORMAT_R8G8B8A8_UNORM;
        vci.image = c.staging[i];
        if (l.create_image_view(l.device, &vci, nullptr, &c.dst_views[i]) != VK_SUCCESS) {
            return false;
        }

        VkDescriptorImageInfo src{};
        src.sampler = c.sampler;
        src.imageView = c.src_views[i];
        src.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        VkDescriptorImageInfo dst{};
        dst.imageView = c.dst_views[i];
        dst.imageLayout = VK_IMAGE_LAYOUT_GENERAL;
        VkWriteDescriptorSet writes[2]{};
        writes[0].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        writes[0].dstSet = c.sets[i];
        writes[0].dstBinding = 0;
        writes[0].descriptorCount = 1;
        writes[0].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        writes[0].pImageInfo = &src;
        writes[1] = writes[0];
        writes[1].dstBinding = 1;
        writes[1].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
        writes[1].pImageInfo = &dst;
        l.update_descriptor_sets(l.device, 2, writes, 0, nullptr);
    }
    c.compute = true;

    // The sharpening pass, if it is wanted. Its own image, pipeline and
    // descriptor set, a compute pass cannot read and write one image, so
    // EASU's output and RCAS's output are different images.
    // Zero means no sharpening at all: no second image, no second
    // dispatch, and the upscale's own output goes straight to the
    // swapchain.
    if (upscale_sharpness() <= 0.0f) return true;
    static const uint32_t kSharpenSpv[] =
#include "sharpen_spv.h"
        ;
    if (sizeof(kSharpenSpv) < 32) return true;  // built without glslc

    VkShaderModuleCreateInfo ssci{};
    ssci.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
    ssci.codeSize = sizeof(kSharpenSpv);
    ssci.pCode = kSharpenSpv;
    if (l.create_shader_module(l.device, &ssci, nullptr, &c.sharpen_shader) != VK_SUCCESS) {
        return true;  // EASU alone still works
    }
    VkComputePipelineCreateInfo scpci{};
    scpci.sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO;
    scpci.stage.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    scpci.stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
    scpci.stage.module = c.sharpen_shader;
    scpci.stage.pName = "main";
    scpci.layout = c.pipeline_layout;  // same bindings and push constants
    if (l.create_compute_pipelines(l.device, VK_NULL_HANDLE, 1, &scpci, nullptr,
                                    &c.sharpen_pipeline) != VK_SUCCESS) {
        return true;
    }

    c.sharpened.resize(count, VK_NULL_HANDLE);
    c.sharpened_memory.resize(count, VK_NULL_HANDLE);
    c.sharpen_src_views.resize(count, VK_NULL_HANDLE);
    c.sharpen_dst_views.resize(count, VK_NULL_HANDLE);
    c.sharpen_sets.resize(count, VK_NULL_HANDLE);
    bool sharpen_ok = true;
    for (uint32_t i = 0; sharpen_ok && i < count; ++i) {
        VkImageCreateInfo sii{};
        sii.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
        sii.imageType = VK_IMAGE_TYPE_2D;
        sii.format = VK_FORMAT_R8G8B8A8_UNORM;
        sii.extent = {c.present.width, c.present.height, 1};
        sii.mipLevels = 1;
        sii.arrayLayers = 1;
        sii.samples = VK_SAMPLE_COUNT_1_BIT;
        sii.tiling = VK_IMAGE_TILING_OPTIMAL;
        sii.usage = VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
        sii.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
        sii.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        if (l.create_image(l.device, &sii, nullptr, &c.sharpened[i]) != VK_SUCCESS ||
            !allocate_offscreen_memory(c.sharpened[i], c.sharpened_memory[i])) {
            sharpen_ok = false;
            break;
        }
        VkImageViewCreateInfo vci{};
        vci.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
        vci.viewType = VK_IMAGE_VIEW_TYPE_2D;
        vci.format = VK_FORMAT_R8G8B8A8_UNORM;
        vci.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        vci.image = c.staging[i];
        if (l.create_image_view(l.device, &vci, nullptr, &c.sharpen_src_views[i]) != VK_SUCCESS) {
            sharpen_ok = false;
            break;
        }
        vci.image = c.sharpened[i];
        if (l.create_image_view(l.device, &vci, nullptr, &c.sharpen_dst_views[i]) != VK_SUCCESS) {
            sharpen_ok = false;
            break;
        }
    }
    if (sharpen_ok) {
        VkDescriptorPoolSize ssizes[2]{};
        ssizes[0].type = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        ssizes[0].descriptorCount = count;
        ssizes[1].type = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
        ssizes[1].descriptorCount = count;
        // The first pool was sized for one set per image; the second pass
        // needs its own, so it gets its own allocation from a pool of the
        // same shape.
        VkDescriptorPoolCreateInfo sdpci{};
        sdpci.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
        sdpci.maxSets = count;
        sdpci.poolSizeCount = 2;
        sdpci.pPoolSizes = ssizes;
        VkDescriptorPool sharpen_pool = VK_NULL_HANDLE;
        if (l.create_descriptor_pool(l.device, &sdpci, nullptr, &sharpen_pool) == VK_SUCCESS) {
            // Kept on the chain so it is destroyed with everything else;
            // the first pool handle is replaced by a pair.
            c.sharpen_pool = sharpen_pool;
            std::vector<VkDescriptorSetLayout> slayouts(count, c.set_layout);
            VkDescriptorSetAllocateInfo sdsai{};
            sdsai.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
            sdsai.descriptorPool = sharpen_pool;
            sdsai.descriptorSetCount = count;
            sdsai.pSetLayouts = slayouts.data();
            sharpen_ok = l.allocate_descriptor_sets(l.device, &sdsai, c.sharpen_sets.data()) ==
                          VK_SUCCESS;
        } else {
            sharpen_ok = false;
        }
    }
    if (sharpen_ok) {
        for (uint32_t i = 0; i < count; ++i) {
            VkDescriptorImageInfo src{};
            src.sampler = c.sampler;
            src.imageView = c.sharpen_src_views[i];
            src.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
            VkDescriptorImageInfo dst{};
            dst.imageView = c.sharpen_dst_views[i];
            dst.imageLayout = VK_IMAGE_LAYOUT_GENERAL;
            VkWriteDescriptorSet writes[2]{};
            writes[0].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
            writes[0].dstSet = c.sharpen_sets[i];
            writes[0].dstBinding = 0;
            writes[0].descriptorCount = 1;
            writes[0].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
            writes[0].pImageInfo = &src;
            writes[1] = writes[0];
            writes[1].dstBinding = 1;
            writes[1].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
            writes[1].pImageInfo = &dst;
            l.update_descriptor_sets(l.device, 2, writes, 0, nullptr);
        }
        c.sharpen = true;
    } else {
        std::printf("stud-render-host: sharpening pass unavailable, upscaling without it\n");
        std::fflush(stdout);
    }
    return true;
}

bool record_upscale_blits(UpscaleChain& c) {
    Loader& l = loader();
    if (l.begin_command_buffer == nullptr || l.end_command_buffer == nullptr ||
        l.cmd_pipeline_barrier == nullptr || l.cmd_blit_image == nullptr) {
        return false;
    }
    for (size_t i = 0; i < c.cmd.size(); ++i) {
        VkCommandBufferBeginInfo bi{};
        bi.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
        if (l.begin_command_buffer(c.cmd[i], &bi) != VK_SUCCESS) return false;
        if (l.cmd_set_checkpoint != nullptr) {
            l.cmd_set_checkpoint(c.cmd[i], "stud upscale: pass begin");
        }

        // Where the two images have to be for the pass that follows: the
        // compute path samples the engine's image and writes the real one
        // through a storage image; the blit path reads and writes them as
        // transfer operands.
        const VkImageLayout read_layout = c.compute ? VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL
                                                    : VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
        const VkImageLayout write_layout = c.compute ? VK_IMAGE_LAYOUT_GENERAL
                                                     : VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        const VkPipelineStageFlags pass_stage = c.compute ? VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT
                                                          : VK_PIPELINE_STAGE_TRANSFER_BIT;
        const VkAccessFlags read_access = c.compute ? VK_ACCESS_SHADER_READ_BIT
                                                    : VK_ACCESS_TRANSFER_READ_BIT;
        const VkAccessFlags write_access = c.compute ? VK_ACCESS_SHADER_WRITE_BIT
                                                     : VK_ACCESS_TRANSFER_WRITE_BIT;

        VkImageMemoryBarrier to_read{};
        to_read.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
        to_read.srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
        to_read.dstAccessMask = read_access;
        // Matches the remap in make_barrier_legal(): the engine's
        // PRESENT_SRC transitions on this image become
        // COLOR_ATTACHMENT_OPTIMAL, so that is where the pass finds it.
        to_read.oldLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        to_read.newLayout = read_layout;
        to_read.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        to_read.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        to_read.image = c.offscreen[i];
        to_read.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};

        // UNDEFINED as the old layout every frame, deliberately: the whole
        // image is overwritten, so its previous contents are worth
        // nothing, and a real acquired swapchain image is undefined on the
        // first frame anyway.
        VkImageMemoryBarrier to_write = to_read;
        to_write.srcAccessMask = 0;
        to_write.dstAccessMask = write_access;
        to_write.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        to_write.newLayout = write_layout;
        // Under compute the shader writes the staging image; the swapchain
        // image is prepared as a blit destination further down.
        to_write.image = c.compute ? c.staging[i] : c.real_images[i];

        VkImageMemoryBarrier before[2] = {to_read, to_write};
        l.cmd_pipeline_barrier(c.cmd[i], VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, pass_stage,
                               0, 0, nullptr, 0, nullptr, 2, before);

        if (c.compute) {
            l.cmd_bind_pipeline(c.cmd[i], VK_PIPELINE_BIND_POINT_COMPUTE, c.pipeline);
            l.cmd_bind_descriptor_sets(c.cmd[i], VK_PIPELINE_BIND_POINT_COMPUTE,
                                       c.pipeline_layout, 0, 1, &c.sets[i], 0, nullptr);
            UpscalePush push{};
            push.src_w = static_cast<int32_t>(c.engine.width);
            push.src_h = static_cast<int32_t>(c.engine.height);
            push.dst_w = static_cast<int32_t>(c.present.width);
            push.dst_h = static_cast<int32_t>(c.present.height);
            push.sharpness = 0.0f;  // EASU does no sharpening; RCAS below does
            l.cmd_push_constants(c.cmd[i], c.pipeline_layout, VK_SHADER_STAGE_COMPUTE_BIT, 0,
                                 sizeof(push), &push);
            // 8x8 per group, matching the shader's own local size.
            if (l.cmd_set_checkpoint != nullptr) {
                l.cmd_set_checkpoint(c.cmd[i], "stud upscale: EASU dispatch");
            }
            l.cmd_dispatch(c.cmd[i], (c.present.width + 7) / 8, (c.present.height + 7) / 8, 1);
            if (l.cmd_set_checkpoint != nullptr) {
                l.cmd_set_checkpoint(c.cmd[i], "stud upscale: EASU done");
            }

            // RCAS, at the output resolution, over what EASU just wrote.
            if (c.sharpen) {
                VkImageMemoryBarrier to_sharpen[2]{};
                to_sharpen[0].sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
                to_sharpen[0].srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
                to_sharpen[0].dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
                to_sharpen[0].oldLayout = VK_IMAGE_LAYOUT_GENERAL;
                to_sharpen[0].newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
                to_sharpen[0].srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
                to_sharpen[0].dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
                to_sharpen[0].image = c.staging[i];
                to_sharpen[0].subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
                to_sharpen[1] = to_sharpen[0];
                to_sharpen[1].srcAccessMask = 0;
                to_sharpen[1].dstAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
                to_sharpen[1].oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
                to_sharpen[1].newLayout = VK_IMAGE_LAYOUT_GENERAL;
                to_sharpen[1].image = c.sharpened[i];
                l.cmd_pipeline_barrier(c.cmd[i], VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                                       VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 0, nullptr, 0,
                                       nullptr, 2, to_sharpen);

                l.cmd_bind_pipeline(c.cmd[i], VK_PIPELINE_BIND_POINT_COMPUTE, c.sharpen_pipeline);
                l.cmd_bind_descriptor_sets(c.cmd[i], VK_PIPELINE_BIND_POINT_COMPUTE,
                                           c.pipeline_layout, 0, 1, &c.sharpen_sets[i], 0,
                                           nullptr);
                // Same size in and out for this pass, which is what lets
                // it read exactly the four neighbouring pixels.
                UpscalePush sharpen_push{};
                sharpen_push.src_w = static_cast<int32_t>(c.present.width);
                sharpen_push.src_h = static_cast<int32_t>(c.present.height);
                sharpen_push.dst_w = static_cast<int32_t>(c.present.width);
                sharpen_push.dst_h = static_cast<int32_t>(c.present.height);
                sharpen_push.sharpness = upscale_sharpness();
                l.cmd_push_constants(c.cmd[i], c.pipeline_layout, VK_SHADER_STAGE_COMPUTE_BIT, 0,
                                     sizeof(sharpen_push), &sharpen_push);
                if (l.cmd_set_checkpoint != nullptr) {
                    l.cmd_set_checkpoint(c.cmd[i], "stud upscale: RCAS sharpen dispatch");
                }
                l.cmd_dispatch(c.cmd[i], (c.present.width + 7) / 8, (c.present.height + 7) / 8, 1);
                if (l.cmd_set_checkpoint != nullptr) {
                    l.cmd_set_checkpoint(c.cmd[i], "stud upscale: RCAS sharpen done");
                }
            }

            // Hand the result to the swapchain: same size, so this is a
            // copy that also converts RGBA to the swapchain's BGRA.
            VkImage finished = c.sharpen ? c.sharpened[i] : c.staging[i];
            VkImageMemoryBarrier hand_over[2]{};
            hand_over[0].sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
            hand_over[0].srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
            hand_over[0].dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
            hand_over[0].oldLayout = VK_IMAGE_LAYOUT_GENERAL;
            hand_over[0].newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
            hand_over[0].srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            hand_over[0].dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            hand_over[0].image = finished;
            hand_over[0].subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
            hand_over[1] = hand_over[0];
            hand_over[1].srcAccessMask = 0;
            hand_over[1].dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
            hand_over[1].oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
            hand_over[1].newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
            hand_over[1].image = c.real_images[i];
            l.cmd_pipeline_barrier(c.cmd[i], VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                                   VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0, nullptr, 2,
                                   hand_over);

            VkImageBlit same{};
            same.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
            same.srcOffsets[0] = {0, 0, 0};
            same.srcOffsets[1] = {static_cast<int32_t>(c.present.width),
                                  static_cast<int32_t>(c.present.height), 1};
            same.dstSubresource = same.srcSubresource;
            same.dstOffsets[0] = same.srcOffsets[0];
            same.dstOffsets[1] = same.srcOffsets[1];
            l.cmd_blit_image(c.cmd[i], finished, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                             c.real_images[i], VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &same,
                             VK_FILTER_NEAREST);
        } else {
        VkImageBlit region{};
        region.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
        region.srcOffsets[0] = {0, 0, 0};
        region.srcOffsets[1] = {static_cast<int32_t>(c.engine.width),
                                static_cast<int32_t>(c.engine.height), 1};
        region.dstSubresource = region.srcSubresource;
        region.dstOffsets[0] = {0, 0, 0};
        region.dstOffsets[1] = {static_cast<int32_t>(c.present.width),
                                static_cast<int32_t>(c.present.height), 1};
        l.cmd_blit_image(c.cmd[i], c.offscreen[i], VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                         c.real_images[i], VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region,
                         VK_FILTER_LINEAR);
        }

        VkImageMemoryBarrier after_src = to_read;
        after_src.srcAccessMask = read_access;
        after_src.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
        after_src.oldLayout = read_layout;
        // Back where the engine left it. Not PRESENT_SRC: this image is
        // not presentable, and the engine's own transitions on it are
        // remapped the same way.
        after_src.newLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;

        VkImageMemoryBarrier after_dst = to_write;
        after_dst.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        after_dst.dstAccessMask = 0;
        after_dst.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        after_dst.newLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
        // Always the swapchain image here: both paths end with it as a
        // transfer destination, the compute one via the hand-over blit.
        after_dst.image = c.real_images[i];

        // The same destination barrier on its own, in its own buffer, for
        // the frames that skip the pass. oldLayout UNDEFINED because the
        // image may never have been through the pass at all, and
        // UNDEFINED is the one old layout that is always legal to
        // transition from; its contents are discarded, which is already
        // true of a frame the pass did not write.
        if (i < c.cmd_present_only.size() && c.cmd_present_only[i] != VK_NULL_HANDLE) {
            VkCommandBufferBeginInfo pbi{};
            pbi.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
            if (l.begin_command_buffer(c.cmd_present_only[i], &pbi) == VK_SUCCESS) {
                VkImageMemoryBarrier make_presentable = after_dst;
                make_presentable.srcAccessMask = 0;
                make_presentable.dstAccessMask = 0;
                make_presentable.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
                make_presentable.newLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
                l.cmd_pipeline_barrier(c.cmd_present_only[i],
                                       VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                                       VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, 0, 0, nullptr, 0,
                                       nullptr, 1, &make_presentable);
                l.end_command_buffer(c.cmd_present_only[i]);
            }
        }

        VkImageMemoryBarrier after[2] = {after_src, after_dst};
        // BOTH stages that actually did the work, and this was wrong.
        //
        // after_src releases a SHADER_READ (the compute pass sampled the
        // engine's image); after_dst releases a TRANSFER_WRITE (the blit
        // into the swapchain image). Naming only TRANSFER said the shader
        // read happened in the transfer stage, which no stage can do:
        // `srcAccessMask (VK_ACCESS_2_SHADER_READ_BIT) is not supported by
        // stage mask (VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT)`, caught by
        // the validation layer, once per frame, on the compute path only.
        //
        // An invalid barrier is not a barrier: the driver is free to make
        // nothing of it, and this one is what orders the pass against the
        // next frame's render. Every run of this session that put this
        // path on the RTX 3050 ended in VK_ERROR_DEVICE_LOST within
        // minutes, while the same code on Intel ran 41 minutes clean --
        // which is what a wrong barrier looks like: fine until the
        // hardware it lies to actually depends on it.
        l.cmd_pipeline_barrier(c.cmd[i], pass_stage | VK_PIPELINE_STAGE_TRANSFER_BIT,
                               VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, 0, 0, nullptr, 0,
                               nullptr, 2, after);
        if (l.end_command_buffer(c.cmd[i]) != VK_SUCCESS) return false;
    }
    return true;
}

// Builds everything on the WINDOW side of the pass for one real
// swapchain: the offscreen images the engine renders into, the command
// buffers that scale them, and the semaphores and fences that order it.
//
// Factored out of vkCreateSwapchainKHR so the window side can be rebuilt
// on its own. `reuse_offscreen` keeps the images the engine is already
// holding -- it was handed those from vkGetSwapchainImagesKHR and has no
// idea they are Stud's, so they must outlive any number of rebuilds.

// The swapchain the engine holds, and the one that really exists.
//
// On Wayland these are always the same handle: a surface there has no
// size of its own, so a resize never invalidates a swapchain and the
// engine's own rebuilds (79 of them in one measured session) all succeed.
// Android behaves the same way, which is the only contract this engine
// has ever been written against.
//
// X11 is the exception: the surface IS the window, so the driver
// invalidates the swapchain the moment it resizes and hands back
// VK_ERROR_OUT_OF_DATE_KHR -- an error the engine meets nowhere else and
// does not recover from. It abandons the frame mid-flight, and what is
// left queued can never complete, so the device never goes idle and its
// next vkDeviceWaitIdle never returns: the window dies, every time.
//
// So Stud answers that error itself. It rebuilds the real swapchain,
// keeping the images the engine was given, and the engine sees the
// success it would have seen on any other platform. It still learns about
// the resize the way it always does, through the surface capabilities.
std::map<uint64_t, uint64_t> g_swapchain_alias;

// Tried and reverted: destroying the old swapchain before building the
// new one, instead of handing it over as oldSwapchain.
//
// The measurement behind it is real and stands. On X11 the driver spends
// its time retiring the old swapchain INSIDE vkCreateSwapchainKHR --
// 22035ms of a 22040ms rebuild, with the window mapped -- while
// destroying that same swapchain outright takes under 50ms, which is why
// no SLOW destroy line has ever printed beside one of them.
//
// It cannot be done in that order regardless. With the old swapchain
// already destroyed, the driver hands the SAME ADDRESS straight back for
// the new one (live-confirmed, twice in one run). Every map in this file
// is keyed by that handle, so the new swapchain inherits the old one's
// upscale chain and its extent, and the pass writes a small old source
// into a large new swapchain: a picture that does not fill the window,
// with black bands down the left and along the bottom that never go
// away. It also leaves the engine holding one value for two swapchains,
// which makes its own destroy ambiguous -- keeping those handles distinct
// is exactly what oldSwapchain is for.
//
// So the cost stands, and whatever removes it has to make retiring the
// old swapchain cheap rather than move the retirement somewhere else.

struct SavedSwapchainCI {
    VkSwapchainCreateInfoKHR ci{};
    std::vector<uint32_t> families;
};
std::map<uint64_t, SavedSwapchainCI> g_swapchain_ci;

VkSwapchainKHR live_swapchain(uint64_t engine_handle) {
    auto it = g_swapchain_alias.find(engine_handle);
    return from_u64<VkSwapchainKHR>(it == g_swapchain_alias.end() ? engine_handle : it->second);
}

void build_upscale_chain(UpscaleChain& pending, VkSwapchainKHR swapchain,
                         const VkSwapchainCreateInfoKHR& ci, bool reuse_offscreen) {
    Loader& l = loader();
    // Anything a previous rebuild had to keep alive; see
    // sweep_retired_chains().
    sweep_retired_chains();
    // One offscreen image per real swapchain image, so an index means
    // the same thing on both sides and vkAcquireNextImageKHR needs no
    // translation at all.
    uint32_t count = 0;
    l.get_swapchain_images(l.device, swapchain, &count, nullptr);
    pending.real_images.resize(count);
    l.get_swapchain_images(l.device, swapchain, &count, pending.real_images.data());
    pending.real = swapchain;

    bool ok = count > 0;
    for (uint32_t i = 0; ok && !reuse_offscreen && i < count; ++i) {
        VkImageCreateInfo ii{};
        ii.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
        ii.imageType = VK_IMAGE_TYPE_2D;
        ii.format = ci.imageFormat;
        ii.extent = {pending.engine.width, pending.engine.height, 1};
        ii.mipLevels = 1;
        ii.arrayLayers = 1;
        ii.samples = VK_SAMPLE_COUNT_1_BIT;
        ii.tiling = VK_IMAGE_TILING_OPTIMAL;
        // Whatever the engine wanted from a swapchain image, plus the
        // ability for Stud to read it back out on the GPU.
        ii.usage = ci.imageUsage | VK_IMAGE_USAGE_TRANSFER_SRC_BIT |
                    VK_IMAGE_USAGE_SAMPLED_BIT;
        ii.usage &= ~static_cast<VkImageUsageFlags>(VK_IMAGE_USAGE_TRANSFER_DST_BIT);
        ii.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
        ii.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        VkImage image = VK_NULL_HANDLE;
        if (l.create_image(l.device, &ii, nullptr, &image) != VK_SUCCESS) { ok = false; break; }
        VkDeviceMemory mem = VK_NULL_HANDLE;
        if (!allocate_offscreen_memory(image, mem)) {
            if (l.destroy_image != nullptr) l.destroy_image(l.device, image, nullptr);
            ok = false;
            break;
        }
        pending.offscreen.push_back(image);
        pending.memory.push_back(mem);
    }

    if (ok) {
        VkCommandPoolCreateInfo pci{};
        pci.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
        pci.queueFamilyIndex = l.first_queue_family;
        ok = l.create_command_pool(l.device, &pci, nullptr, &pending.pool) == VK_SUCCESS;
    }
    if (ok) {
        pending.cmd.resize(count, VK_NULL_HANDLE);
        VkCommandBufferAllocateInfo cbai{};
        cbai.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
        cbai.commandPool = pending.pool;
        cbai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        cbai.commandBufferCount = count;
        ok = l.allocate_command_buffers(l.device, &cbai, pending.cmd.data()) == VK_SUCCESS;
    }
    if (ok) {
        pending.cmd_present_only.resize(count, VK_NULL_HANDLE);
        VkCommandBufferAllocateInfo cbai{};
        cbai.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
        cbai.commandPool = pending.pool;
        cbai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        cbai.commandBufferCount = count;
        ok = l.allocate_command_buffers(l.device, &cbai,
                                        pending.cmd_present_only.data()) == VK_SUCCESS;
    }
    for (uint32_t i = 0; ok && i < count; ++i) {
        VkSemaphoreCreateInfo sci{};
        sci.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;
        VkSemaphore sem = VK_NULL_HANDLE;
        if (l.create_semaphore(l.device, &sci, nullptr, &sem) != VK_SUCCESS) { ok = false; break; }
        pending.done.push_back(sem);
        VkFenceCreateInfo fci{};
        fci.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
        VkFence fence = VK_NULL_HANDLE;
        if (l.create_fence(l.device, &fci, nullptr, &fence) != VK_SUCCESS) { ok = false; break; }
        pending.fence.push_back(fence);
    }
    // The real pass. If it cannot be built, an old driver, a format
    // that will not take a storage image, a machine with no glslc at
    // build time, the recorded command buffers fall back to a plain
    // blit, which is what the compositor was doing anyway. Never a
    // black window.
    // A plain scaled blit when the filter is switched off: the same
    // pass without its shaders. X11 needs it even with upscaling off,
    // because nothing else there scales the engine's buffer up to the
    // window; see vk_set_upscale_use_compute().
    //
    // build_upscale_compute() is what SETS pending.compute, so the
    // flag has to gate the call rather than the field, or the field
    // is false before anyone has tried and every chain is a blit.
    const bool want_compute = g_upscale_use_compute.load(std::memory_order_relaxed);
    if (ok && want_compute && !build_upscale_compute(pending)) {
        std::printf("stud-render-host: the upscale shader could not be set up, falling back "
                    "to a plain scaled blit\n");
        std::fflush(stdout);
        pending.compute = false;
    }
    if (ok) ok = record_upscale_blits(pending);
    if (!ok) {
        // An honest degrade: tear the half-built chain down and let
        // the engine render straight into the real swapchain, which
        // is exactly today's behaviour.
        std::printf("stud-render-host: upscale unavailable for this swapchain, presenting "
                    "the engine's own image instead\n");
        std::fflush(stdout);
        destroy_upscale_chain(pending);
    } else {
        pending.in_flight.assign(count, false);
        g_upscale_chains[to_u64(swapchain)] = std::move(pending);
        const UpscaleChain& built = g_upscale_chains[to_u64(swapchain)];
        std::printf("stud-render-host: upscale %ux%u -> %ux%u (%u images, %s)\n",
                    built.engine.width, built.engine.height, built.present.width,
                    built.present.height, count,
                    built.compute ? (built.sharpen ? "EASU + RCAS" : "EASU")
                                  : "linear blit");
        std::fflush(stdout);
    }
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
    ci.imageFormat = static_cast<VkFormat>(h.image_format);
    // STUD_VK_FORCE_OPAQUE_FORMAT=1 swaps a UNORM swapchain for its SRGB
    // twin. Off by default: it is a real change to what the engine asked
    // for, not a fix in itself.
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

    // Stud's own upscale: the real swapchain is bigger than the engine
    // asked for, and the engine is given offscreen images of its own
    // requested size instead. Everything about the engine's view, the
    // extent it asked for, its scale, its UI, is left exactly as it
    // would have been.
    uint32_t out_w = g_upscale_output_w.load(std::memory_order_relaxed);
    uint32_t out_h = g_upscale_output_h.load(std::memory_order_relaxed);
    // On X11, ask the window what size it is NOW rather than trusting the
    // last value the display pump latched.
    //
    // This is what the 22-second rebuilds were. The latched value is set
    // when the pump sees a configure; the engine's rebuild arrives later,
    // and during a drag the window has moved on by then. Live-caught: a
    // real swapchain built as "upscale 1536x792 -> 1920x990" while the X
    // window was 1564x930, and then a second 22-second rebuild whose only
    // job was retiring that mismatched swapchain. A swapchain whose
    // extent does not match its window is what the driver spends those 22
    // seconds on -- every fast rebuild in the same session matched.
    //
    // Wayland keeps the latched value: a surface there has no size of its
    // own and the viewport scales whatever it is given, so nothing can
    // mismatch.
    if (stud::android_glue::display_backend() == stud::android_glue::DisplayBackend::X11) {
        int32_t now_w = 0;
        int32_t now_h = 0;
        stud::android_glue::native_window_display_pixel_size(&now_w, &now_h);
        if (now_w > 0 && now_h > 0 &&
            (static_cast<uint32_t>(now_w) != out_w || static_cast<uint32_t>(now_h) != out_h)) {
            // Worth saying, worth saying a few times, not worth saying
            // on every frame of a drag-resize.
            static int said = 0;
            if (said < 8) {
                ++said;
                std::printf("stud-render-host: the window is %dx%d now, not the %ux%u last "
                            "latched; building the swapchain for the window it has\n", now_w,
                            now_h, out_w, out_h);
                std::fflush(stdout);
            }
            out_w = static_cast<uint32_t>(now_w);
            out_h = static_cast<uint32_t>(now_h);
            g_upscale_output_w.store(out_w, std::memory_order_relaxed);
            g_upscale_output_h.store(out_h, std::memory_order_relaxed);
        }
    }
    UpscaleChain pending;
    // Only when the output really is bigger than what the engine asked
    // for. Equal sizes mean there is nothing to upscale, and a blit of an
    // image onto itself would be pure cost.
    const bool want_upscale = out_w > ci.imageExtent.width && out_h > ci.imageExtent.height &&
                               ci.imageExtent.width > 0 && ci.imageExtent.height > 0 &&
                               l.create_image != nullptr &&
                               l.allocate_command_buffers != nullptr && l.have_first_queue_family;
    if (want_upscale) {
        pending.engine = ci.imageExtent;
        pending.present = {out_w, out_h};
        pending.format = ci.imageFormat;
        // The real swapchain is what reaches the compositor, so it is the
        // one that carries the full resolution, and it has to be a blit
        // destination, which a swapchain image is not asked to be
        // otherwise.
        ci.imageExtent = pending.present;
        ci.imageUsage |= VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    }

    VkSwapchainKHR swapchain = VK_NULL_HANDLE;
    // Let the old swapchain go quiet before handing it to the driver.
    //
    // The driver has to retire everything outstanding on oldSwapchain
    // before it can build the new one, and Stud's upscale pass is one of
    // the things outstanding on it: its submit and its present are Stud's
    // own, in flight, on that swapchain. Measured without this, the
    // driver's own create took 22012ms of a 22022ms rebuild, with
    // render-host serving nothing for the whole of it -- which is what a
    // maximise or restore looked like from the outside.
    //
    // Bounded, because a pass that is genuinely stuck must not become a
    // window that is stuck: at worst this is the same wait the driver was
    // going to do anyway, capped.
    if (h.old_swapchain != 0) {
        auto old_chain = g_upscale_chains.find(h.old_swapchain);
        if (old_chain != g_upscale_chains.end()) {
            const auto quiet_t0 = std::chrono::steady_clock::now();
            const bool quiet = upscale_work_finished(old_chain->second, 500000000ull);
            const double quiet_ms = std::chrono::duration<double, std::milli>(
                std::chrono::steady_clock::now() - quiet_t0).count();
            if (quiet_ms > 5.0) {
                std::printf("stud-render-host: waited %.0fms for the old swapchain's upscale pass "
                            "to finish before rebuilding%s\n", quiet_ms,
                            quiet ? "" : " (it did not)");
                std::fflush(stdout);
            }
        }
    }
    const auto create_t0 = std::chrono::steady_clock::now();
    VkResult r;
    {
        // Nobody queries this surface while it is being built on; see
        // surface_mutex().
        std::unique_lock<std::shared_mutex> surface_lock(surface_mutex());
        r = l.create_swapchain(l.device, &ci, nullptr, &swapchain);
        // A surface that still has a swapchain on it refuses the next
        // one, and whether that reads as SURFACE_LOST or
        // NATIVE_WINDOW_IN_USE is the driver's choice. The two call sites
        // of flush_retired_chains() should mean there is nothing left
        // holding this surface -- but if the engine reached here by some
        // order they do not cover, one deferred pass must not cost the
        // whole renderer. So: let go of everything and ask once more.
        if (r == VK_ERROR_SURFACE_LOST_KHR || r == VK_ERROR_NATIVE_WINDOW_IN_USE_KHR) {
            std::printf("stud-render-host: vkCreateSwapchainKHR refused the surface (%d); letting "
                        "go of anything still holding it and retrying once\n",
                        static_cast<int>(r));
            std::fflush(stdout);
            flush_retired_chains("retrying a refused swapchain");
            r = l.create_swapchain(l.device, &ci, nullptr, &swapchain);
        }
    }
    {
        const double create_ms = std::chrono::duration<double, std::milli>(
            std::chrono::steady_clock::now() - create_t0).count();
        if (create_ms > 1000.0) {
            std::printf("stud-render-host: the driver took %.0fms to build the swapchain "
                        "(oldSwapchain %s, window %s)\n", create_ms,
                        h.old_swapchain != 0 ? "given" : "none",
                        stud::android_glue::native_window_is_visible() ? "up" : "down");
            std::fflush(stdout);
        }
    }
    std::printf("stud-render-host: vkCreateSwapchainKHR -> %d (%ux%u format=%u images>=%u)\n",
                static_cast<int>(r), h.width, h.height, h.image_format, h.min_image_count);
    std::fflush(stdout);
    if (r != VK_SUCCESS) return static_cast<uint64_t>(static_cast<int32_t>(r));
    g_swapchain_extents[to_u64(swapchain)] = ci.imageExtent;
    {
        // Kept so Stud can build this swapchain again at a new size
        // without the engine's help. The family list is copied, since the
        // create-info only points at it.
        SavedSwapchainCI saved;
        saved.ci = ci;
        saved.families = families;
        saved.ci.pQueueFamilyIndices =
            saved.families.empty() ? nullptr : saved.families.data();
        saved.ci.oldSwapchain = VK_NULL_HANDLE;
        g_swapchain_ci[to_u64(swapchain)] = std::move(saved);
    }
    const auto chain_t0 = std::chrono::steady_clock::now();
    if (want_upscale) {
        build_upscale_chain(pending, swapchain, ci, /*reuse_offscreen=*/false);
    }
    {
        const auto now = std::chrono::steady_clock::now();
        const double swap_ms =
            std::chrono::duration<double, std::milli>(chain_t0 - create_t0).count();
        const double chain_ms = std::chrono::duration<double, std::milli>(now - chain_t0).count();
        if (swap_ms + chain_ms > 20.0) {
            std::printf("stud-render-host: a swapchain rebuild cost %.0fms (driver %.0f, "
                        "upscale pass %.0f)\n", swap_ms + chain_ms, swap_ms, chain_ms);
            std::fflush(stdout);
        }
    }
    return write_handle(to_u64(swapchain), out, out_len);
}

// True when this swapchain no longer matches the window it presents to.
//
// Deliberately NOT wired into vkAcquireNextImageKHR or vkQueuePresentKHR.
// A Wayland surface never goes out of date on its own (the buffer defines
// the size), so returning VK_ERROR_OUT_OF_DATE_KHR on a resize looks like
// the textbook fix, and it was live-tested and is wrong for this engine:
// it logs `VULKAN ERROR: vkAcquireNextImageKHR ... returned -1000001004`
// and stops presenting entirely (frozen at the same frame, confirmed by a
// present counter that stopped advancing). The engine rebuilds its
// swapchain from its own resize path instead, which only needs
// vkGetPhysicalDeviceSurfaceCapabilitiesKHR to report the window's real
// current size. That is what sync_vk_window_size() in render-host's main
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

// Stud keeps handles of its own to repair a resize; both are dropped the
// moment the engine destroys the object. Defined with the trackers, which
// live in this file's own anonymous namespace.
namespace {
void forget_destroyed_fence(uint64_t fence);
void forget_destroyed_semaphore(uint64_t semaphore);
}  // namespace

uint64_t vk_get_swapchain_images(uint64_t swapchain, uint32_t capacity, std::vector<uint8_t>& out,
                                  uint32_t* out_len) {
    Loader& l = loader();
    if (l.get_swapchain_images == nullptr) {
        return static_cast<uint64_t>(static_cast<int32_t>(VK_ERROR_INITIALIZATION_FAILED));
    }
    // Upscaling: the engine gets the offscreen images. They are the same
    // count as the real ones and in the same order, so an index means the
    // same thing on both sides and nothing else has to translate.
    const auto chain = g_upscale_chains.find(swapchain);
    if (chain != g_upscale_chains.end()) {
        const auto& images = chain->second.offscreen;
        const uint32_t returned =
            capacity > 0 ? static_cast<uint32_t>(std::min<size_t>(capacity, images.size())) : 0;
        const uint32_t reported = capacity > 0 ? returned : static_cast<uint32_t>(images.size());
        out.resize(sizeof(uint32_t) + sizeof(uint64_t) * returned);
        std::memcpy(out.data(), &reported, sizeof(reported));
        for (uint32_t i = 0; i < returned; ++i) {
            uint64_t h = to_u64(images[i]);
            // Registered exactly as a real swapchain image would be: the
            // engine's views and framebuffers over them must still count
            // as "targets the screen".
            l.swapchain_images.insert(h);
            l.swapchain_image_list[swapchain].push_back(h);
            l.retired_images.erase(h);
            std::memcpy(out.data() + sizeof(uint32_t) + i * sizeof(h), &h, sizeof(h));
        }
        *out_len = static_cast<uint32_t>(out.size());
        const bool truncated = capacity > 0 && returned < images.size();
        return static_cast<uint64_t>(
            static_cast<int32_t>(truncated ? VK_INCOMPLETE : VK_SUCCESS));
    }

    // No upscale chain: the engine really does own this swapchain's
    // images, so it gets whichever swapchain its handle stands for.
    VkSwapchainKHR sc = live_swapchain(swapchain);
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
    // A buffer that may later be bound to imported host memory has to say
    // so when it is created: the handle type must appear here as well as
    // on the allocation, or binding the two is undefined behaviour.
    // Live-caught by validation (VUID-vkBindBufferMemory-memory-02985),
    // and the host cannot know at create time which buffers will be bound
    // to a shared allocation, so every buffer declares it while sharing
    // is available at all.
    VkExternalMemoryBufferCreateInfo external{};
    // Gated on the extension's own entry point, not on the alignment:
    // vkGetPhysicalDeviceProperties2 answers the host-pointer chain even
    // when VK_EXT_external_memory_host was never enabled on the device,
    // and naming a handle type the device does not have is itself
    // invalid (live-caught, VUID-...-handleTypes-parameter).
    if (l.get_memory_host_pointer_properties != nullptr &&
        imported_host_pointer_alignment() > 0) {
        external.sType = VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_BUFFER_CREATE_INFO;
        external.handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_HOST_ALLOCATION_BIT_EXT;
        ci.pNext = &external;
    }
    VkBuffer buffer = VK_NULL_HANDLE;
    VkResult r = l.create_buffer(l.device, &ci, nullptr, &buffer);
    // Deliberately no per-call print here. The engine creates buffers
    // sixteen times a frame, so a line each, with a flush, is a write
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
    // indistinguishable from every other cause of a black frame, so
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
            // aggressively, so an offscreen image created after a
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
            // Forget it FIRST. Stud holds semaphore handles to repair a
            // failed acquire, and a handle the engine has destroyed is a
            // handle nothing may name again -- using one is a crash
            // inside the driver, live-caught.
            forget_destroyed_semaphore(handle);
            if (l.destroy_semaphore) {
                l.destroy_semaphore(l.device, from_u64<VkSemaphore>(handle), nullptr);
            }
            break;
        case K::Fence:
            // Same, for the fences the engine reset and abandoned: the
            // crash was a submit carrying one of these, on a fence the
            // engine had already destroyed during its resize.
            forget_destroyed_fence(handle);
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
            // The engine names the handle it was given; what has to be
            // destroyed is whatever that stands for now.
            const uint64_t live = to_u64(live_swapchain(handle));
            g_swapchain_alias.erase(handle);
            g_swapchain_ci.erase(handle);
            // Stud's own offscreen images and the pass that reads them go
            // with the swapchain they belong to.
            //
            // And the swapchain does not go anywhere while that pass is
            // still running. destroy_upscale_chain() defers a chain whose
            // work has not finished, which is right -- but destroying the
            // swapchain underneath it anyway leaves the GPU writing into
            // images that no longer exist, and that is how a GPU hangs.
            //
            // Live-caught on Wayland with FSR on, in this order: "the
            // upscale pass was still running when its swapchain went away;
            // keeping it until the GPU is done with it", a present that
            // blocked for 12006ms and then succeeded, and the engine's own
            // vkWaitForFences coming back VK_ERROR_DEVICE_LOST with
            // "DeviceRecovery trigger: endRender reason=hung". The
            // driver's reset is what ended the freeze.
            //
            // So the swapchain is handed to the retired chain and
            // destroyed with it, once the fences say the GPU has let go.
            bool swapchain_held_by_chain = false;
            {
                auto chain = g_upscale_chains.find(handle);
                if (chain != g_upscale_chains.end()) {
                    size_t retired_before = 0;
                    {
                        std::lock_guard<std::mutex> lock(g_retired_chains_mutex);
                        retired_before = g_retired_chains.size();
                    }
                    destroy_upscale_chain(chain->second);
                    g_upscale_chains.erase(chain);
                    std::lock_guard<std::mutex> lock(g_retired_chains_mutex);
                    if (g_retired_chains.size() > retired_before) {
                        g_retired_chains.back().destroy_with_chain =
                            from_u64<VkSwapchainKHR>(live);
                        swapchain_held_by_chain = true;
                    }
                }
            }
            auto images = l.swapchain_image_list.find(handle);
            if (images != l.swapchain_image_list.end()) {
                for (uint64_t image : images->second) {
                    l.swapchain_images.erase(image);
                    l.swapchain_views.erase(image);
                    l.retired_images.insert(image);
                }
                l.swapchain_image_list.erase(images);
            }
            if (swapchain_held_by_chain) {
                // Destroyed by sweep_retired_chains(), with the pass that
                // is still writing into it.
                break;
            }
            if (l.destroy_swapchain) {
                // Timed for the same reason the create is: a rebuild is a
                // destroy and a create, and 22 seconds of it has to belong
                // to one of them. If the destroy is the slow half then
                // what the driver waits for is the old swapchain's
                // presents retiring, and destroying before creating rather
                // than handing it over as oldSwapchain would change
                // nothing; if the create is, it would.
                const auto destroy_t0 = std::chrono::steady_clock::now();
                l.destroy_swapchain(l.device, from_u64<VkSwapchainKHR>(live), nullptr);
                const double destroy_ms = std::chrono::duration<double, std::milli>(
                    std::chrono::steady_clock::now() - destroy_t0).count();
                if (destroy_ms > 50.0) {
                    std::printf("stud-render-host: SLOW vkDestroySwapchainKHR %.0fms\n",
                                destroy_ms);
                    std::fflush(stdout);
                }
            }
            break;
        }
        case K::Surface:
            // Nothing may still be presenting to it. A deferred upscale
            // pass holds its swapchain, and a swapchain outliving its
            // surface is what left the WSI answering SURFACE_LOST to
            // every later vkCreateSwapchainKHR.
            flush_retired_chains("the engine destroys its surface");
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
            // The real VkDevice is still deliberately NOT destroyed: the
            // engine tears its renderer down and rebuilds it repeatedly
            // (once per mode probe, and after every recovery), and
            // destroying it here would invalidate every resolved device
            // command while the engine still holds them. The comment that
            // used to sit here also claimed this process owns exactly one
            // device for its whole life; it does not -- a session creates
            // four to six.
            //
            // What IS safe to let go of is Stud's own upscale chains.
            // They are built here, referenced by nothing the engine can
            // see, and this message is the engine saying it has finished
            // with the device they live on. Destroying them now, while
            // l.device is still that device, is the only moment it can be
            // done correctly: after the next vkCreateDevice, l.device
            // names a different device and every handle in these chains
            // belongs to a device it was never created on.
            //
            // They are the largest allocations in this file -- full
            // resolution offscreen, staging and sharpened images, their
            // memory, views, descriptor pools, pipelines, semaphores and
            // fences, per swapchain image -- and stranding one per
            // recovery is the measured climb from 1941 MB to 2230 MB
            // across four of them, while the scene being drawn got
            // simpler.
            //
            // Deliberately NOT touching shared_memory(), buffer_memory()
            // or shared_writes() here. Those describe memory the engine
            // imported, and the real device is still alive underneath, so
            // the driver may still reference those pages. An earlier
            // attempt to free them at a new device's creation unmapped
            // pages out from under the driver and segfaulted inside
            // libnvidia-glcore. Their growth is real and is not fixable
            // without either destroying the device for real or keying
            // them per device; see Stud-Analysis/gpu/stud-freeze.md.
            if (from_u64<VkDevice>(handle) == l.device && l.device != VK_NULL_HANDLE) {
                // Stud's own chains go now, while l.device still names the
                // device they were built on. This is the only moment that
                // is true: after the next vkCreateDevice every handle in
                // them belongs to a device they were never created on.
                for (auto& kv : g_upscale_chains) destroy_upscale_chain(kv.second, true);
                g_upscale_chains.clear();
                flush_retired_chains("the engine is done with this device");

                // The device itself is parked, not destroyed; see
                // RetiredDevice. Everything of Stud's that belongs to it
                // travels with it, and the tables are emptied here so the
                // replacement device starts clean rather than inheriting
                // handles that describe a device the engine has abandoned.
                RetiredDevice parked;
                parked.device = l.device;
                parked.probe_pool = l.probe_pool;
                parked.repair_fence = g_repair_fence;
                parked.writes.swap(shared_writes());
                g_retired_devices.push_back(std::move(parked));
                std::printf("stud-render-host: the engine is done with this device; parked it "
                            "with %zu mapping(s) until the next one has proved itself (%zu "
                            "parked in total)\n",
                            g_retired_devices.back().writes.size(), g_retired_devices.size());
                std::fflush(stdout);

                l.probe_pool = VK_NULL_HANDLE;
                l.probe_queue = VK_NULL_HANDLE;
                l.have_first_queue_family = false;
                g_repair_fence = VK_NULL_HANDLE;
                g_swapchain_extents.clear();
                l.retired_images.clear();
                {
                    std::lock_guard<std::mutex> lock(buffer_memory_mutex());
                    buffer_memory().clear();
                }
                {
                    std::lock_guard<std::mutex> lock(shared_memory_mutex());
                    shared_memory().clear();
                }
                l.mapped.clear();
                l.mapped_size.clear();
                g_presents_since_new_device.store(0, std::memory_order_relaxed);
            }
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
            // the ids naming those sets must stop resolving, the same
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
    // any draw succeeding, the one way to colour the screen that does
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
        std::printf("stud-render-host: vkCreateRenderPass att=%u sub=%u dep=%u -> %d\n",
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
        if (vk_object_trace_enabled()) {
            std::printf("stud-render-host: framebuffer %llu targets the swapchain (%ux%u)\n",
                        static_cast<unsigned long long>(to_u64(fb)), ci.width, ci.height);
            std::fflush(stdout);
        }
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
// The engine allocates descriptor sets constantly, measured at 738 calls
// a frame in a real game, and a handle that has to be returned makes
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
        // its id must stop resolving, otherwise a later use would hand
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
    // descriptor in it carries handles. Those are already host handles.
    // Process B never invents one, it only ever echoes back what this
    // process gave it, so the blob passes through unchanged.
    // What the template says it needs versus what arrived. A blob that is
    // short leaves the driver reading whatever follows it, descriptors
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


namespace {

// Resets and waits belong in the same log as the submits. A wait that never returns while the same fence is being reset by
// another thread is a missed signal, and the only way to see it is to have
// all three in one ordered list.
// Fences the engine has RESET but not yet submitted anything with.
//
// This is the engine's own resize bug, and it is a well-known one: the
// frame's fence is waited on and reset BEFORE the image is acquired, so
// when vkAcquireNextImageKHR fails the frame is abandoned with nothing
// submitted, and that fence can never be signalled. Its next
// vkWaitForFences on the same fence -- or the vkDeviceWaitIdle in its
// swapchain rebuild -- then waits for ever. The submit ring caught it
// exactly: a WAIT and a RESET, and nothing after them.
//
// It only bites on X11, because it takes a failed acquire to reach it,
// and a failed acquire takes a surface whose size the driver enforces.
// Wayland and Android never produce one, which is why 79 rebuilds in a
// Wayland session all pass.
//
// So Stud keeps track of which fences are in that state and, when an
// acquire fails, signals them with an empty submit -- the submit the
// engine would have made if it had not given up. It is legal, it costs
// nothing, and it turns a permanent hang into a dropped frame.
std::set<uint64_t> g_fences_reset_unsubmitted;
std::mutex g_fences_reset_mutex;

// Swapchains whose last acquire failed.
//
// After a failed acquire the engine has no image and has rendered
// nothing, and it presents anyway -- live-caught, the present follows the
// failed acquire by four milliseconds. That present waits on semaphores
// its abandoned frame never signalled, so it can never execute, and
// neither can anything queued behind it: the device stops being able to
// go idle, and the engine's own vkDeviceWaitIdle waits for ever.
//
// The Vulkan specification's own issue tracker describes this hole
// exactly -- vkDeviceWaitIdle cannot cancel a pending acquire's
// semaphore, and a submission waiting on one never completes. So the
// present for a frame that was never acquired is dropped here, which is
// what the engine would do on any platform where acquire does not fail.
std::set<uint64_t> g_acquire_failed;
std::mutex g_acquire_failed_mutex;

void note_acquire_failed(uint64_t swapchain, bool failed) {
    std::lock_guard<std::mutex> lock(g_acquire_failed_mutex);
    if (failed) {
        g_acquire_failed.insert(swapchain);
    } else {
        g_acquire_failed.erase(swapchain);
    }
}

// Semaphores a failed acquire left behind, and which Stud has signalled
// on its behalf.
//
// This is the other half of the hole above, and the one that actually
// wedges a resize. vkAcquireNextImageKHR returning VK_ERROR_OUT_OF_DATE_KHR
// leaves the semaphore it was given UNSIGNALLED -- the spec is explicit
// that a failed acquire signals nothing. The engine, which never meets
// this error on Android or Wayland, goes on to submit its frame anyway
// with that semaphore in pWaitSemaphores and its frame fence attached.
// That submission can never execute, so the fence can never signal, and
// the engine's next vkWaitForFences on it waits for ever.
//
// Caught exactly, in the submit ring at the moment the window froze:
//   #1254 submit fence=0x...320 cmdbufs=1
//   #1258 WAIT   fence=0x...320          <-- never returns
// a real submission, not a fence the engine forgot to submit (that case
// is g_fences_reset_unsubmitted, above, and was already handled).
//
// So the semaphore is signalled here with an empty submit -- exactly what
// a successful acquire would have done -- and the engine's frame runs and
// retires instead of hanging.
//
// Binary semaphores make that a promise to keep track of: a signal that
// nobody waits on stays pending, and signalling an already-signalled
// binary semaphore is invalid. So every semaphore signalled this way is
// remembered until something actually waits on it (drained in
// vk_queue_submit), and if the engine hands the same one back to another
// acquire first, the stale signal is consumed with an empty wait before
// the acquire runs.
// Declared with the other file globals near the top, because
// vk_create_device() clears it long before this point in the file.

// What the GPU had actually reached when it died.
//
// The driver keeps the checkpoints a queue passed through across a device
// loss, so this is the one question worth asking at that moment: did the
// GPU stop inside Stud's own upscale pass, or somewhere in the engine's
// work? Printed rather than interpreted -- a marker that is present says
// the GPU got that far, and the absence of Stud's markers says it did
// not reach them at all.
void report_checkpoints_after_loss() {
    Loader& l = loader();
    if (l.get_queue_checkpoint_data == nullptr) {
        std::printf("stud-render-host: no checkpoint data: the driver does not offer "
                    "VK_NV_device_diagnostic_checkpoints, so where the GPU stopped is unknown\n");
        std::fflush(stdout);
        return;
    }
    // The queue Stud's own work goes to; the same one the engine's
    // submissions pass through, which is the point -- both are visible
    // in one checkpoint list.
    VkQueue q = l.probe_queue;
    if (q == VK_NULL_HANDLE) {
        std::printf("stud-render-host: no queue recorded yet, so no checkpoints to read\n");
        std::fflush(stdout);
        return;
    }
    uint32_t n = 0;
    l.get_queue_checkpoint_data(q, &n, nullptr);
    if (n == 0) {
        std::printf("stud-render-host: the queue reported no checkpoints at all, so the GPU "
                    "did not reach any of Stud's own recorded work\n");
        std::fflush(stdout);
        return;
    }
    std::vector<VkCheckpointDataNV> data(n);
    for (auto& d : data) {
        d.sType = VK_STRUCTURE_TYPE_CHECKPOINT_DATA_NV;
        d.pNext = nullptr;
    }
    l.get_queue_checkpoint_data(q, &n, data.data());
    std::printf("stud-render-host: the GPU's last checkpoints before the loss (%u):\n", n);
    for (uint32_t i = 0; i < n; ++i) {
        const char* name = data[i].pCheckpointMarker != nullptr
                               ? static_cast<const char*>(data[i].pCheckpointMarker)
                               : "(unnamed)";
        std::printf("stud-render-host:   stage 0x%x reached \"%s\"\n",
                    static_cast<unsigned>(data[i].stage), name);
    }
    std::fflush(stdout);
}

void note_result(VkResult r, const char* where) {
    if (r != VK_ERROR_DEVICE_LOST) return;
    // Said once, loudly, and never behind a switch.
    //
    // The device being lost is the difference between "Stud is slow" and
    // "the GPU went away and everything on it is gone", and the two look
    // identical from the outside: the window stops, audio keeps playing
    // because it is not on the GPU, and if the driver recovers, the
    // engine rebuilds every resource it had -- which is why every texture
    // reloads from its lowest mip afterwards. Without this line a report
    // of that says only "it froze for ten seconds", and the log agrees
    // with everything and explains nothing.
    if (!g_device_lost.exchange(true, std::memory_order_relaxed)) {
        std::printf("stud-render-host: THE VULKAN DEVICE WAS LOST, reported by %s. The GPU "
                    "dropped its work; the engine has to rebuild everything it had there, so "
                    "expect a stall and every texture to stream in again. Stud stops issuing "
                    "work of its own from here.\n",
                    where != nullptr ? where : "an unnamed call");
        std::fflush(stdout);
        report_checkpoints_after_loss();
    }
}

// Waits for a repair submission to finish before returning to the engine.
//
// Not optional, and it is what a first version of this got wrong: these
// submissions name a semaphore the engine is about to destroy. It answers
// a failed acquire by tearing its swapchain down, and vkDestroySemaphore
// requires that no submitted batch still references it -- destroying one
// out from under a batch that is merely queued is invalid, and the driver
// answers with a segfault inside the next queue operation (live-caught,
// the return address landing on the submit inside vk_acquire_next_image).
//
// Waiting costs a stall on a path that only runs when an acquire has
// already failed, which is a frame the engine has given up on anyway. The
// semaphore's own signalled state survives the wait -- that is state, not
// a pending operation, and destroying a semaphore that carries one is
// allowed.
void wait_for_repair(VkFence fence) {
    Loader& l = loader();
    if (fence == VK_NULL_HANDLE || l.wait_for_fences == nullptr) return;
    // 100ms is far longer than an empty batch takes and short enough that
    // a wedged queue does not become a wedged window.
    const VkResult r = l.wait_for_fences(l.device, 1, &fence, VK_TRUE, 100000000ull);
    note_result(r, "vkWaitForFences");
    if (r == VK_SUCCESS && l.reset_fences != nullptr) l.reset_fences(l.device, 1, &fence);
}

// One fence, reused, for the repair submissions above.
VkFence repair_fence() {
    Loader& l = loader();
    VkFence& fence = g_repair_fence;
    if (fence == VK_NULL_HANDLE && l.create_fence != nullptr && l.device != VK_NULL_HANDLE) {
        VkFenceCreateInfo fci{};
        fci.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
        if (l.create_fence(l.device, &fci, nullptr, &fence) != VK_SUCCESS) fence = VK_NULL_HANDLE;
    }
    return fence;
}

std::set<uint64_t> g_semaphores_stud_signalled;
// Whether that set has anything in it, readable without the lock.
//
// Both users sit on the frame path -- one per acquire, one per wait
// semaphore per submit -- and outside a failed acquire the set is empty,
// which is every frame of a normal session. A relaxed read costs an
// instruction; taking a mutex on the engine's own render thread, a few
// times a frame, does not.
std::atomic<bool> g_have_stud_signalled{false};
std::mutex g_semaphores_stud_signalled_mutex;

// Marks a semaphore as consumed once a real submission waits on it.
void note_semaphore_waited(uint64_t semaphore) {
    if (semaphore == 0) return;
    if (!g_have_stud_signalled.load(std::memory_order_relaxed)) return;
    std::lock_guard<std::mutex> lock(g_semaphores_stud_signalled_mutex);
    g_semaphores_stud_signalled.erase(semaphore);
    g_have_stud_signalled.store(!g_semaphores_stud_signalled.empty(),
                                std::memory_order_relaxed);
}

// Signals the semaphore, and the fence, that a failed acquire left
// unsignalled. Either may be null; the engine passes one, the other, or
// both.
void signal_what_the_failed_acquire_left(VkQueue queue, uint64_t semaphore, uint64_t fence) {
    Loader& l = loader();
    if (g_device_lost.load(std::memory_order_relaxed)) return;
    if (l.queue_submit == nullptr || queue == VK_NULL_HANDLE) return;
    if (semaphore == 0 && fence == 0) return;
    VkSemaphore sem = from_u64<VkSemaphore>(semaphore);
    VkSubmitInfo si{};
    si.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    if (semaphore != 0) {
        si.signalSemaphoreCount = 1;
        si.pSignalSemaphores = &sem;
    }
    // The engine's own fence if it passed one to the acquire -- it waits
    // on that itself -- otherwise Stud's, so the batch can be waited for
    // before this returns. See wait_for_repair().
    const VkFence own = fence == 0 ? repair_fence() : VK_NULL_HANDLE;
    {
        std::lock_guard<std::mutex> queue_lock(queue_mutex());
        l.queue_submit(queue, 1, &si, fence != 0 ? from_u64<VkFence>(fence) : own);
    }
    if (own != VK_NULL_HANDLE) {
        wait_for_repair(own);
    } else if (l.queue_wait_idle != nullptr) {
        // The engine's fence went on the batch, so there is nothing of
        // Stud's to wait on; wait for the queue instead, for the same
        // reason.
        std::lock_guard<std::mutex> queue_lock(queue_mutex());
        note_result(l.queue_wait_idle(queue), "vkQueueWaitIdle");
    }
    if (semaphore != 0) {
        std::lock_guard<std::mutex> lock(g_semaphores_stud_signalled_mutex);
        g_semaphores_stud_signalled.insert(semaphore);
        g_have_stud_signalled.store(true, std::memory_order_relaxed);
    }
    static int said = 0;
    if (said < 8) {
        ++said;
        std::printf("stud-render-host: an acquire failed without signalling what the engine's "
                    "own frame waits on; signalled it so the frame can retire\n");
        std::fflush(stdout);
    }
}

// Consumes a signal Stud left on this semaphore, if the engine is about
// to acquire into it again and nothing ever waited on it. An empty
// submission that waits is the only way to take a binary semaphore's
// signal back.
void drain_stale_signal(VkQueue queue, uint64_t semaphore) {
    if (semaphore == 0) return;
    if (!g_have_stud_signalled.load(std::memory_order_relaxed)) return;
    if (g_device_lost.load(std::memory_order_relaxed)) return;
    {
        std::lock_guard<std::mutex> lock(g_semaphores_stud_signalled_mutex);
        if (g_semaphores_stud_signalled.erase(semaphore) == 0) return;
        g_have_stud_signalled.store(!g_semaphores_stud_signalled.empty(),
                                    std::memory_order_relaxed);
    }
    Loader& l = loader();
    if (l.queue_submit == nullptr || queue == VK_NULL_HANDLE) return;
    VkSemaphore sem = from_u64<VkSemaphore>(semaphore);
    VkPipelineStageFlags stage = VK_PIPELINE_STAGE_ALL_COMMANDS_BIT;
    VkSubmitInfo si{};
    si.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    si.waitSemaphoreCount = 1;
    si.pWaitSemaphores = &sem;
    si.pWaitDstStageMask = &stage;
    const VkFence f = repair_fence();
    {
        std::lock_guard<std::mutex> queue_lock(queue_mutex());
        l.queue_submit(queue, 1, &si, f);
    }
    // Same reason as the signal above: this batch names a semaphore the
    // engine may destroy as soon as it hears about the resize.
    wait_for_repair(f);
}

bool take_acquire_failed(uint64_t swapchain) {
    std::lock_guard<std::mutex> lock(g_acquire_failed_mutex);
    return g_acquire_failed.erase(swapchain) != 0;
}

void forget_destroyed_fence(uint64_t fence) {
    if (fence == 0) return;
    std::lock_guard<std::mutex> lock(g_fences_reset_mutex);
    g_fences_reset_unsubmitted.erase(fence);
}

void forget_destroyed_semaphore(uint64_t semaphore) {
    if (semaphore == 0) return;
    if (!g_have_stud_signalled.load(std::memory_order_relaxed)) return;
    std::lock_guard<std::mutex> lock(g_semaphores_stud_signalled_mutex);
    g_semaphores_stud_signalled.erase(semaphore);
    g_have_stud_signalled.store(!g_semaphores_stud_signalled.empty(),
                                std::memory_order_relaxed);
}

void note_fence_reset(uint64_t fence) {
    if (fence == 0) return;
    std::lock_guard<std::mutex> lock(g_fences_reset_mutex);
    g_fences_reset_unsubmitted.insert(fence);
}

void note_fence_submitted(uint64_t fence) {
    if (fence == 0) return;
    std::lock_guard<std::mutex> lock(g_fences_reset_mutex);
    g_fences_reset_unsubmitted.erase(fence);
}

// Signals whatever the engine left behind, so its next wait can finish.
void signal_fences_the_engine_reset(VkQueue queue) {
    Loader& l = loader();
    if (g_device_lost.load(std::memory_order_relaxed)) return;
    if (l.queue_submit == nullptr || queue == VK_NULL_HANDLE) return;
    std::vector<uint64_t> orphans;
    {
        std::lock_guard<std::mutex> lock(g_fences_reset_mutex);
        orphans.assign(g_fences_reset_unsubmitted.begin(), g_fences_reset_unsubmitted.end());
        g_fences_reset_unsubmitted.clear();
    }
    for (uint64_t fence : orphans) {
        VkSubmitInfo si{};
        si.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
        std::lock_guard<std::mutex> queue_lock(queue_mutex());
        l.queue_submit(queue, 0, nullptr, from_u64<VkFence>(fence));
    }
    if (!orphans.empty()) {
        static int said = 0;
        if (said < 8) {
            ++said;
            std::printf("stud-render-host: an acquire failed after the engine had already reset "
                        "its frame fence; signalled %zu of them so its next wait can finish\n",
                        orphans.size());
            std::fflush(stdout);
        }
    }
}


}  // namespace

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
    {
        uint32_t total_cbs = 0;
        uint64_t first_cb = 0;
        for (uint32_t i = 0; i < n; ++i) {
            total_cbs += static_cast<uint32_t>(buffers[i].size());
            if (first_cb == 0 && !buffers[i].empty()) first_cb = to_u64(buffers[i][0]);
        }
        note_fence_submitted(fence);
        // A real wait retires any signal Stud left on that semaphore; see
        // g_semaphores_stud_signalled.
        for (uint32_t i = 0; i < n; ++i) {
            for (VkSemaphore w : waits[i]) note_semaphore_waited(to_u64(w));
        }
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
    std::lock_guard<std::mutex> queue_lock(queue_mutex());
    VkResult res = l.queue_submit(from_u64<VkQueue>(queue), n, submits.empty() ? nullptr : submits.data(),
                                   from_u64<VkFence>(fence));
    note_result(res, "vkQueueSubmit");
    return static_cast<uint64_t>(static_cast<int32_t>(res));
}

// Fences waited on right now, and the resets waiting for those waits.
//
// In the engine's own process a wait returns before the reset that follows
// it is issued, so the two never overlap. Stud breaks that: the wait blocks
// one connection thread inside the driver for a whole round trip while a
// reset for the same fence arrives on another connection and runs at once.
// The fence goes back to unsignalled between the signal and the waiter
// noticing, and with a ten millisecond frame cycle the waiter can lose that
// race every time, which is a wait that never returns while the engine
// carries on submitting. Live-caught: the fence log shows the stuck fence
// cycling WAIT/RESET/submit every 21ms with a wait on it outstanding.
//
// So a reset waits for any in-flight wait on the same fence. That is the
// ordering the engine already believes it has.
std::mutex& fence_wait_mutex() {
    static std::mutex m;
    return m;
}
std::condition_variable& fence_wait_cv() {
    static std::condition_variable cv;
    return cv;
}
std::set<uint64_t>& fences_being_waited_on() {
    static std::set<uint64_t> s;
    return s;
}

struct FenceWaitGuard {
    std::vector<uint64_t> held;
    explicit FenceWaitGuard(const std::vector<VkFence>& fences) {
        std::lock_guard<std::mutex> lock(fence_wait_mutex());
        for (const VkFence f : fences) {
            const uint64_t h = to_u64(f);
            if (fences_being_waited_on().insert(h).second) held.push_back(h);
        }
    }
    ~FenceWaitGuard() {
        {
            std::lock_guard<std::mutex> lock(fence_wait_mutex());
            for (const uint64_t h : held) fences_being_waited_on().erase(h);
        }
        fence_wait_cv().notify_all();
    }
};

uint64_t vk_wait_for_fences(const std::vector<uint8_t>& in, uint32_t wait_all, uint64_t timeout) {
    Loader& l = loader();
    if (l.wait_for_fences == nullptr) {
        return static_cast<uint64_t>(static_cast<int32_t>(VK_ERROR_INITIALIZATION_FAILED));
    }
    vk_wire::Reader r(in.data(), in.size());
    const uint32_t n = r.u32();
    std::vector<VkFence> fences(n);
    for (auto& f : fences) f = from_u64<VkFence>(r.u64());
    // Announce the wait so a reset for the same fence cannot land in the
    // middle of it and take the signal away.
    FenceWaitGuard wait_guard(fences);
    const auto t0 = std::chrono::steady_clock::now();
    // Never wait unbounded in one call.
    //
    // The engine asks for UINT64_MAX, so a submission that never retires
    // parks this thread inside the driver forever. The client is blocked
    // reading the reply, no frame ever completes, and the SLOW print
    // below cannot fire because the call does not return: a total freeze
    // that says nothing at all. Waiting in slices keeps the semantics the
    // engine asked for and lets a stuck fence announce itself.
    VkResult res = VK_TIMEOUT;
    {
        const uint64_t slice_ns = 1000ull * 1000ull * 1000ull;  // 1s
        uint64_t left = timeout;
        int reports = 0;
        for (;;) {
            const uint64_t slice = left < slice_ns ? left : slice_ns;
            res = l.wait_for_fences(l.device, n, fences.empty() ? nullptr : fences.data(),
                                    wait_all, slice);
            if (res != VK_TIMEOUT) break;
            if (timeout != UINT64_MAX) {
                left -= slice;
                if (left == 0) break;
            }
            const double waited = std::chrono::duration<double>(
                                      std::chrono::steady_clock::now() - t0).count();
            if (waited >= 5.0 * static_cast<double>(reports + 1)) {
                ++reports;
                std::printf("stud-render-host: FENCE STUCK: %u fence(s) have not signalled in "
                            "%.0fs (wait_all=%u, engine timeout=%llu). The GPU has not retired "
                            "the submission they belong to.\n",
                            n, waited, wait_all,
                            static_cast<unsigned long long>(timeout));
                std::fflush(stdout);
            }
        }
    }
    const double ms = std::chrono::duration<double, std::milli>(
                          std::chrono::steady_clock::now() - t0).count();
    if (ms > 50.0) {
        std::printf("stud-render-host: SLOW vkWaitForFences %.1fms (n=%u timeout=%llu) -> %d\n", ms,
                    n, static_cast<unsigned long long>(timeout), static_cast<int>(res));
        std::fflush(stdout);
    }
    // The engine's own wait is a place the device really is lost: a GPU
    // that stops retiring work is noticed HERE first, because this is the
    // call that sits on the fence. It was the one result in this file
    // that nobody told, so g_device_lost stayed false through a real
    // loss and Stud went on issuing work of its own afterwards -- which
    // the flag exists precisely to stop, since a submit after the loss is
    // where the driver was seen to segfault.
    //
    // Caught in a live trace: `SLOW vkWaitForFences 4463.0ms -> -4`, with
    // the kernel logging `Xid 109 CTX SWITCH TIMEOUT` against
    // stud-render-host at the same second, and not one line in Stud's own
    // output saying the device had gone.
    note_result(res, "vkWaitForFences (the engine's own)");
    return static_cast<uint64_t>(static_cast<int32_t>(res));
}

uint64_t vk_reset_fences(const std::vector<uint8_t>& in) {
    {
        vk_wire::Reader probe(in.data(), in.size());
        const uint32_t n = probe.u32();
        for (uint32_t i = 0; i < n; ++i) {
            const uint64_t f = probe.u64();
            note_fence_reset(f);
        }
    }
    Loader& l = loader();
    if (l.reset_fences == nullptr) {
        return static_cast<uint64_t>(static_cast<int32_t>(VK_ERROR_INITIALIZATION_FAILED));
    }
    vk_wire::Reader r(in.data(), in.size());
    const uint32_t n = r.u32();
    std::vector<VkFence> fences(n);
    for (auto& f : fences) f = from_u64<VkFence>(r.u64());
    // Hold the reset until nothing is waiting on any of these fences.
    //
    // BOUNDED, and it has to be. The comment here used to say the wait
    // could not deadlock "because vk_wait_for_fences is bounded" -- it is
    // not. That function slices its wait into one-second pieces but only
    // decrements and gives up when the timeout is not UINT64_MAX, and
    // UINT64_MAX is exactly what the engine passes. So a fence that never
    // signals keeps its entry in fences_being_waited_on() for ever, and
    // this wait, which was unbounded too, would have blocked here for
    // ever with it.
    //
    // Two seconds is far longer than any real overlap between a reset and
    // a wait on the same fence, and giving up is safe: resetting a fence
    // another thread is waiting on is a validation error, not a crash,
    // and it is strictly better than wedging this connection.
    {
        std::unique_lock<std::mutex> lock(fence_wait_mutex());
        const bool clear = fence_wait_cv().wait_for(lock, std::chrono::seconds(2), [&fences] {
            for (const VkFence f : fences) {
                if (fences_being_waited_on().count(to_u64(f)) != 0) return false;
            }
            return true;
        });
        if (!clear) {
            static bool said = false;
            if (!said) {
                said = true;
                std::printf("stud-render-host: resetting a fence something is still waiting on; "
                            "the wait has not finished in two seconds and is not going to. This "
                            "means a fence was never signalled -- see the FENCE STUCK line "
                            "above.\n");
                std::fflush(stdout);
            }
        }
    }
    VkResult res = l.reset_fences(l.device, n, fences.empty() ? nullptr : fences.data());
    return static_cast<uint64_t>(static_cast<int32_t>(res));
}

// Builds the real swapchain again, at whatever size the window is now,
// without telling the engine.
//
// Called when the driver reports the current one stale -- which on X11 is
// every resize. The engine keeps the handle it was given and the images
// it renders into; only the window side is replaced.
bool recreate_real_swapchain(uint64_t engine_handle) {
    // OFF, and kept for the record rather than deleted.
    //
    // Rebuilding the window side under the engine works right up until
    // the new swapchain comes back with a different image count than the
    // images the engine is already holding, at which point the chain
    // indexes past them and render-host dies. Making that safe means
    // owning the image mapping as well, which is a much larger change
    // than the problem needs: the engine's actual fault is one fence (see
    // signal_fences_the_engine_reset), and that is addressed directly.
    //
    // STUD_VK_REBUILD_SWAPCHAIN=1 turns it back on for anyone measuring.
    static const bool enabled = std::getenv("STUD_VK_REBUILD_SWAPCHAIN") != nullptr;
    if (!enabled) return false;
    Loader& l = loader();
    auto saved = g_swapchain_ci.find(engine_handle);
    if (saved == g_swapchain_ci.end() || l.create_swapchain == nullptr) return false;

    const uint32_t out_w = g_upscale_output_w.load(std::memory_order_relaxed);
    const uint32_t out_h = g_upscale_output_h.load(std::memory_order_relaxed);
    auto chain = g_upscale_chains.find(engine_handle);
    VkExtent2D extent = saved->second.ci.imageExtent;
    if (chain != g_upscale_chains.end() && out_w > 0 && out_h > 0) {
        // With the pass in place the real swapchain carries the window's
        // own resolution, not the engine's.
        extent = {out_w, out_h};
    } else {
        extent = {g_window_width.load(std::memory_order_relaxed),
                  g_window_height.load(std::memory_order_relaxed)};
    }
    if (extent.width == 0 || extent.height == 0) return false;

    VkSwapchainKHR old = live_swapchain(engine_handle);
    VkSwapchainCreateInfoKHR ci = saved->second.ci;
    ci.imageExtent = extent;
    ci.oldSwapchain = old;
    VkSwapchainKHR fresh = VK_NULL_HANDLE;
    const VkResult r = l.create_swapchain(l.device, &ci, nullptr, &fresh);
    if (r != VK_SUCCESS) {
        std::printf("stud-render-host: could not rebuild the swapchain at %ux%u (%d)\n",
                    extent.width, extent.height, static_cast<int>(r));
        std::fflush(stdout);
        return false;
    }

    if (chain != g_upscale_chains.end()) {
        destroy_upscale_chain_keeping_offscreen(chain->second);
        chain->second.present = extent;
        build_upscale_chain(chain->second, fresh, ci, /*reuse_offscreen=*/true);
    }
    if (l.destroy_swapchain != nullptr && old != VK_NULL_HANDLE) {
        l.destroy_swapchain(l.device, old, nullptr);
    }
    g_swapchain_alias[engine_handle] = to_u64(fresh);
    g_swapchain_extents[engine_handle] = extent;
    static int rebuilds = 0;
    if (rebuilds < 8) {
        ++rebuilds;
        std::printf("stud-render-host: the window changed under the swapchain; rebuilt it at "
                    "%ux%u without disturbing the engine\n", extent.width, extent.height);
        std::fflush(stdout);
    }
    return true;
}

// What the engine is told about a suboptimal real swapchain: nothing.
//
// VK_SUBOPTIMAL_KHR is a SUCCESS code -- the image really was acquired,
// the frame really was presented -- and it refers to the REAL swapchain,
// which is Stud's, not the engine's. The engine renders into offscreen
// images Stud hands it and Stud owns everything between those and the
// compositor, so "the swapchain you never created is no longer ideal for
// the surface you do not own" is not a question the engine can answer.
//
// It does not treat it as an answer either. It logs every one as
// `VULKAN ERROR: vkAcquireNextImageKHR ... returned VK_SUBOPTIMAL_KHR`
// and mishandles the frame, and what that looks like is a flickering,
// wrongly drawn window. Live-caught on X11 under a Wayland compositor:
// 676 unbroken suboptimal frames from launch, with the window and the
// swapchain agreeing on 1728x971 the whole time.
//
// Hidden here for the same reason VK_ERROR_OUT_OF_DATE_KHR already is a
// few lines above -- the engine has no recovery for either, and every
// other platform it ships on never produces them -- and hidden rather
// than fixed because there is nothing on this side to fix: the extents
// match, rebuilding the swapchain clears it for about ten frames and it
// returns, and the present mode makes no difference. All three were
// measured before this was written.
//
// Stud's own handling above still runs; only what crosses back to the
// engine is changed.
uint64_t engine_visible_result(VkResult r) {
    return static_cast<uint64_t>(
        static_cast<int32_t>(r == VK_SUBOPTIMAL_KHR ? VK_SUCCESS : r));
}

uint64_t vk_acquire_next_image(uint64_t swapchain, uint64_t timeout, uint64_t semaphore,
                                uint64_t fence, std::vector<uint8_t>& out, uint32_t* out_len) {
    Loader& l = loader();
    if (l.acquire_next_image == nullptr) {
        return static_cast<uint64_t>(static_cast<int32_t>(VK_ERROR_INITIALIZATION_FAILED));
    }
    uint32_t index = 0;
    static int acquires = 0;
    if (acquires < 5 && vk_object_trace_enabled()) {
        std::printf("stud-render-host: vkAcquireNextImageKHR #%d\n", acquires);
        std::fflush(stdout);
    }
    ++acquires;
    // If the last failed acquire left a signal on this semaphore that
    // nothing consumed, take it back before the driver signals it again.
    drain_stale_signal(l.probe_queue, semaphore);
    const auto t0 = std::chrono::steady_clock::now();
    VkResult r = l.acquire_next_image(l.device, live_swapchain(swapchain), timeout,
                                       from_u64<VkSemaphore>(semaphore), from_u64<VkFence>(fence),
                                       &index);
    if (r == VK_ERROR_OUT_OF_DATE_KHR) {
        // The engine is about to abandon this frame, having already waited
        // on and reset its fence. Signal it, or its next wait never ends.
        signal_fences_the_engine_reset(l.probe_queue);
    }
    note_acquire_failed(swapchain, r != VK_SUCCESS && r != VK_SUBOPTIMAL_KHR);
    if (r == VK_ERROR_OUT_OF_DATE_KHR && recreate_real_swapchain(swapchain)) {
        // The engine has no recovery for this; every other platform it
        // runs on never produces it. Rebuilt above, so ask again -- and
        // note the semaphore it passed was NOT signalled by the failed
        // attempt, so this second one signals it exactly once, which is
        // what the engine's own frame is waiting on.
        r = l.acquire_next_image(l.device, live_swapchain(swapchain), timeout,
                                 from_u64<VkSemaphore>(semaphore), from_u64<VkFence>(fence),
                                 &index);
    }
    note_result(r, "vkAcquireNextImageKHR");
    // A retry that succeeded really did acquire an image, so the present
    // that follows is the engine's own and must not be dropped.
    if (r == VK_SUCCESS || r == VK_SUBOPTIMAL_KHR) note_acquire_failed(swapchain, false);
    if (r != VK_SUCCESS && r != VK_SUBOPTIMAL_KHR) {
        // Still failed, so nothing signalled the semaphore or the fence
        // the engine handed in -- and it submits its frame regardless.
        // Checked after the retry above, so a retry that succeeds signals
        // them itself and they are never signalled twice.
        signal_what_the_failed_acquire_left(l.probe_queue, semaphore, fence);
        // The present that follows is STILL dropped (note_acquire_failed
        // above stands). Tried the other way and it is much worse: a
        // failed acquire produces no image index, so the engine presents
        // whichever one it used last, which it does not own -- and the
        // driver answers that with VK_ERROR_DEVICE_LOST and then a
        // segfault inside the next acquire. Live-caught, in that order.
        //
        // So the frame runs and retires, which is what its fence needs,
        // and its present is thrown away, which is what the swapchain
        // needs.
    }
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
    return engine_visible_result(r);
}


// Reads one swapchain image back and reports whether it is all black.
// Uses its own command buffer and a full queue wait, slow and only ever
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
    {
        std::lock_guard<std::mutex> queue_lock(queue_mutex());
        l.queue_submit(l.probe_queue, 1, &si, VK_NULL_HANDLE);
        if (l.queue_wait_idle != nullptr) l.queue_wait_idle(l.probe_queue);
    }

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


// Make the swapchain image legal to present when the upscale pass did not
// run.
//
// The pass's own command buffer carries the only transition to
// PRESENT_SRC_KHR, so every path that skips it used to hand the
// presentation engine an image in whatever layout it last held --
// UNDEFINED for an index the pass had never run on, which is
// VUID-VkPresentInfoKHR-pImageIndices-01430 and undefined behaviour on a
// driver that enforces it.
//
// Submitted exactly like the pass it replaces: it consumes the engine's
// own wait semaphores and signals the same done[index] the present then
// waits on, so the semaphore bookkeeping is identical to the working
// path. On failure it changes nothing and the caller presents as it did
// before.
//
// It does NOT put the engine's frame on screen -- that frame is in
// offscreen[i] and only the pass moves it. A skipped frame still shows
// the previous contents. Making it legal is the part that matters for
// stability; showing the right pixels needs the pass to run.
void make_presentable_on_skip(UpscaleChain& c, uint32_t index, uint64_t queue,
                              VkPresentInfoKHR& pi,
                              std::vector<VkSemaphore>& upscaled_waits) {
    Loader& l = loader();
    if (index >= c.cmd_present_only.size() || c.cmd_present_only[index] == VK_NULL_HANDLE ||
        index >= c.done.size() || l.queue_submit == nullptr) {
        return;
    }
    VkSubmitInfo si{};
    si.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    std::vector<VkPipelineStageFlags> stages(pi.waitSemaphoreCount,
                                             VK_PIPELINE_STAGE_ALL_COMMANDS_BIT);
    si.waitSemaphoreCount = pi.waitSemaphoreCount;
    si.pWaitSemaphores = pi.pWaitSemaphores;
    si.pWaitDstStageMask = stages.empty() ? nullptr : stages.data();
    si.commandBufferCount = 1;
    si.pCommandBuffers = &c.cmd_present_only[index];
    si.signalSemaphoreCount = 1;
    si.pSignalSemaphores = &c.done[index];
    VkResult sr = VK_ERROR_UNKNOWN;
    {
        std::lock_guard<std::mutex> queue_lock(queue_mutex());
        sr = l.queue_submit(from_u64<VkQueue>(queue), 1, &si, VK_NULL_HANDLE);
    }
    if (sr != VK_SUCCESS) return;
    upscaled_waits.assign(1, c.done[index]);
    pi.waitSemaphoreCount = 1;
    pi.pWaitSemaphores = upscaled_waits.data();
}

uint64_t vk_queue_present(uint64_t queue, const std::vector<uint8_t>& in) {
    Loader& l = loader();
    if (l.queue_present == nullptr) {
        return static_cast<uint64_t>(static_cast<int32_t>(VK_ERROR_INITIALIZATION_FAILED));
    }
    vk_wire::Reader r(in.data(), in.size());
    const uint32_t nw = r.u32();
    std::vector<VkSemaphore> waits(nw);
    // Read below and checked before anything is submitted: a present for
    // a frame whose acquire failed is dropped, see g_acquire_failed.
    for (auto& w : waits) w = from_u64<VkSemaphore>(r.u64());
    const uint32_t ns = r.u32();
    std::vector<VkSwapchainKHR> chains(ns);
    std::vector<VkSwapchainKHR> live(ns);
    std::vector<uint32_t> indices(ns);
    for (auto& c : chains) c = from_u64<VkSwapchainKHR>(r.u64());
    // The engine names the handle it was given; the present goes to
    // whichever swapchain that handle currently stands for.
    for (size_t i = 0; i < chains.size(); ++i) live[i] = live_swapchain(to_u64(chains[i]));
    for (auto& i : indices) i = r.u32();

    // The frame that was never acquired.
    //
    // Its wait semaphores were never signalled, so both the pass and the
    // present itself would sit in the queue for ever. Reported as success:
    // the engine loses one frame during a resize, which is what it loses
    // anyway on a platform where the acquire would have succeeded.
    bool dropped = false;
    for (const auto& chain_handle : chains) {
        if (take_acquire_failed(to_u64(chain_handle))) dropped = true;
    }
    if (dropped) {
        static int said = 0;
        if (said < 8) {
            ++said;
            std::printf("stud-render-host: dropped a present for a frame whose acquire had "
                        "failed; nothing would ever have signalled what it waits on\n");
            std::fflush(stdout);
        }
        return static_cast<uint64_t>(static_cast<int32_t>(VK_SUCCESS));
    }

    VkPresentInfoKHR pi{};
    pi.sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR;
    pi.waitSemaphoreCount = nw;
    pi.pWaitSemaphores = waits.empty() ? nullptr : waits.data();
    pi.swapchainCount = ns;
    pi.pSwapchains = live.empty() ? nullptr : live.data();
    pi.pImageIndices = indices.empty() ? nullptr : indices.data();
    // STUD_VK_PROBE_PIXELS=1: before presenting, copy the swapchain image
    // to a host-visible buffer and report whether it holds anything but
    // black. This is the difference between "the engine renders nothing"
    // and "it renders and the compositor never shows it", the two
    // halves of a black window, which nothing upstream of here can tell
    // apart.
    static const bool probe = std::getenv("STUD_VK_PROBE_PIXELS") != nullptr;
    static int probed = 0;
    if (probe && probed < 3 && ns > 0 && !indices.empty()) {
        probe_swapchain_pixels(chains[0], indices[0]);
        ++probed;
    }

    // Stud's own upscale pass, between the engine's last draw and the
    // present it asked for.
    //
    // The engine's wait semaphores become the pass's wait semaphores, and
    // the present then waits on the pass instead, so the chain is
    // engine render -> blit -> present, with nothing running early. The
    // per-image fence is what makes re-submitting the same pre-recorded
    // command buffer legal: it says the previous submit of it has
    // finished.
    std::vector<VkSemaphore> upscaled_waits;
    if (ns > 0 && !indices.empty()) {
        auto chain = g_upscale_chains.find(to_u64(chains[0]));
        if (chain != g_upscale_chains.end() && l.queue_submit != nullptr) {
            UpscaleChain& c = chain->second;
            const uint32_t index = indices[0];
            // A tainted semaphore cannot be signalled again, and cannot be
            // destroyed while its own submit is still running either, so
            // the pass's work is waited out first (bounded, like every
            // other wait in here) and then they are replaced wholesale.
            if (c.semaphores_tainted) {
                c.semaphores_tainted = false;
                bool idle = true;
                if (l.wait_for_fences != nullptr && l.device_wait_idle != nullptr) {
                    for (size_t i = 0; i < c.fence.size(); ++i) {
                        if (!c.in_flight[i]) continue;
                        VkResult fr = VK_TIMEOUT;
                        for (int slice = 0; slice < 4 && fr == VK_TIMEOUT; ++slice) {
                            fr = l.wait_for_fences(l.device, 1, &c.fence[i], VK_TRUE,
                                                    250ull * 1000ull * 1000ull);
                        }
                        if (fr != VK_SUCCESS) idle = false;
                    }
                }
                if (idle && l.destroy_semaphore != nullptr && l.create_semaphore != nullptr) {
                    VkSemaphoreCreateInfo sci{};
                    sci.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;
                    for (auto& sem : c.done) {
                        if (sem != VK_NULL_HANDLE) l.destroy_semaphore(l.device, sem, nullptr);
                        sem = VK_NULL_HANDLE;
                        l.create_semaphore(l.device, &sci, nullptr, &sem);
                    }
                    if (l.reset_fences != nullptr) {
                        for (size_t i = 0; i < c.fence.size(); ++i) {
                            if (c.in_flight[i]) l.reset_fences(l.device, 1, &c.fence[i]);
                            c.in_flight[i] = false;
                        }
                    }
                    static int said = 0;
                    if (said < 4) {
                        ++said;
                        std::printf("stud-render-host: a present did not succeed, so the pass's "
                                    "semaphores were replaced before reuse\n");
                        std::fflush(stdout);
                    }
                }
            }
            // With no semaphore to wait on there is NOTHING ordering this
            // pass against the engine's render: two submissions on one
            // queue may overlap freely, and the pass reads the image the
            // engine just drew. The frame goes through unmodified
            // instead, which is a frame without upscaling, not a race.
            if (waits.empty() && index < c.cmd.size()) {
                static int said = 0;
                if (said < 4) {
                    ++said;
                    std::printf("stud-render-host: the engine presented with no wait semaphore; "
                                "presenting this frame without the upscale pass\n");
                    std::fflush(stdout);
                }
            } else if (index < c.cmd.size()) {
                // The previous submit of this image's pre-recorded command
                // buffer has to have finished before it can be re-submitted.
                //
                // BOUNDED, and that is the whole point. This ran with
                // UINT64_MAX and it is the one wait in this file that the
                // bounded-slice treatment in vk_wait_for_fences never
                // covered, so it could not even report itself. Live-caught:
                // the window went to the background, the compositor stopped
                // releasing swapchain images, this fence never signalled,
                // and the wait sat here forever -- on the thread serving the
                // client's connection, with the connection's own lock held,
                // so every later call from every thread queued behind a
                // present that was never coming back. That is the freeze:
                // not the GPU, one unbounded wait in the wrong place.
                //
                // On timeout the upscale pass is simply skipped for this
                // frame and the engine's image is presented directly, which
                // is exactly what the submit-failed path below already
                // does. The engine's own semaphores are still unconsumed
                // (nothing was submitted), so presenting on them is
                // correct.
                bool image_ready = true;
                if (c.in_flight[index] && l.wait_for_fences != nullptr) {
                    const auto fence_t0 = std::chrono::steady_clock::now();
                    VkResult fr = VK_TIMEOUT;
                    for (int slice = 0; slice < 4 && fr == VK_TIMEOUT; ++slice) {
                        // 250ms a slice: long enough not to spin, short
                        // enough that a stall is a hitch rather than a
                        // freeze.
                        fr = l.wait_for_fences(l.device, 1, &c.fence[index], VK_TRUE,
                                                250ull * 1000ull * 1000ull);
                    }
                    if (fr == VK_SUCCESS) {
                        if (l.reset_fences != nullptr) l.reset_fences(l.device, 1, &c.fence[index]);
                    } else {
                        image_ready = false;
                        const double waited = std::chrono::duration<double>(
                                                  std::chrono::steady_clock::now() - fence_t0)
                                                  .count();
                        static int reports = 0;
                        if (reports < 8) {
                            ++reports;
                            std::printf("stud-render-host: the upscale pass for swapchain image "
                                        "%u has not finished after %.1fs (%d); presenting this "
                                        "frame without it rather than waiting\n",
                                        index, waited, static_cast<int>(fr));
                            std::fflush(stdout);
                        }
                    }
                }
                if (!image_ready) {
                    // Straight to the present below, on the engine's own
                    // semaphores, with no upscale submit for this frame.
                    goto present_without_upscale;
                }
                // The stages this pass actually runs in, and that is the
                // whole point of the mask.
                //
                // A semaphore wait only holds back the stages named here.
                // This said COLOR_ATTACHMENT_OUTPUT, which appears
                // nowhere in the recorded command buffer: it is two
                // compute dispatches and a blit. So nothing in the pass
                // was held back by the engine's render-finished
                // semaphore, and the dispatches were free to start
                // sampling the engine's image while the engine was still
                // drawing into it -- a real race against live GPU work,
                // every frame, on every frame's own image.
                //
                // Intel tolerated it for 41 minutes; the RTX 3050 lost
                // the device within minutes, every run, which is what a
                // race looks like on hardware that overlaps work
                // properly. ALL_COMMANDS rather than an enumeration: the
                // wait happens once per frame, and being exhaustive here
                // is worth more than saving a stage.
                std::vector<VkPipelineStageFlags> stages(
                    waits.size(), VK_PIPELINE_STAGE_ALL_COMMANDS_BIT);
                VkSubmitInfo si{};
                si.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
                si.waitSemaphoreCount = static_cast<uint32_t>(waits.size());
                si.pWaitSemaphores = waits.empty() ? nullptr : waits.data();
                si.pWaitDstStageMask = stages.empty() ? nullptr : stages.data();
                si.commandBufferCount = 1;
                si.pCommandBuffers = &c.cmd[index];
                si.signalSemaphoreCount = 1;
                si.pSignalSemaphores = &c.done[index];
                VkResult sr = VK_SUCCESS;
                {
                    std::lock_guard<std::mutex> queue_lock(queue_mutex());
                    sr = l.queue_submit(from_u64<VkQueue>(queue), 1, &si, c.fence[index]);
                }
                if (sr == VK_SUCCESS) {
                    c.in_flight[index] = true;
                    // The present now waits on the pass, not on the
                    // engine, the engine's own semaphores have already
                    // been consumed by the submit above, and waiting on
                    // them twice would hang.
                    upscaled_waits.push_back(c.done[index]);
                    pi.waitSemaphoreCount = 1;
                    pi.pWaitSemaphores = upscaled_waits.data();
                } else {
                    // Nothing was submitted, so nothing will ever signal
                    // c.fence[index] -- which was reset just above. Leaving
                    // in_flight set marks this index as having GPU work
                    // outstanding for ever: every later present of it burns
                    // the full fence timeout and skips the pass, and
                    // upscale_work_finished() never returns true again, so
                    // the chain is deferred for ever and the swapchain it
                    // holds is never destroyed. That is a presentable pool
                    // that shrinks and never recovers.
                    c.in_flight[index] = false;
                    // The pass did not run, so nothing transitioned this
                    // image to PRESENT_SRC_KHR.
                    //
                    // THE FAILURE BRANCH, and only it. Called from the
                    // success branch by mistake, this submitted a second
                    // command buffer that waited on the very semaphore the
                    // pass had just signalled and signalled it again --
                    // a self-dependency and a double-signal of a binary
                    // semaphore, once per frame. It cost 10 fps in Stud,
                    // and the unsignalled fences it left on the shared
                    // buffers propagated to the compositor: the iGPU
                    // logged `Fence expiration time out` against the
                    // browser and the taskbar, and the desktop artefacted.
                    make_presentable_on_skip(c, index, queue, pi, upscaled_waits);
                    static bool said = false;
                    if (!said) {
                        said = true;
                        std::printf("stud-render-host: upscale submit failed (%d), presenting "
                                    "without it\n", static_cast<int>(sr));
                        std::fflush(stdout);
                    }
                }
            }
        }
    }

    // Anything a rebuild had to hold on to, freed as soon as the GPU has
    // finished with it. Costs one lock and an empty-vector check when
    // there is nothing to sweep, which is every ordinary frame.
    sweep_retired_chains();

present_without_upscale:
    // Held across the present itself. It blocks in the driver, so this
    // does hold submits from other threads for its duration -- which is
    // exactly the serialisation the spec asks for, and what was missing.
    VkResult res;
    const auto present_t0 = std::chrono::steady_clock::now();
    std::chrono::steady_clock::time_point present_t1;
    std::chrono::steady_clock::time_point lock_t;
    {
        std::lock_guard<std::mutex> queue_lock(queue_mutex());
        lock_t = std::chrono::steady_clock::now();
        res = l.queue_present(from_u64<VkQueue>(queue), &pi);
        present_t1 = std::chrono::steady_clock::now();
    }
    // What the present itself costs THIS process, which the client's own
    // frame breakdown cannot see: it hands the present over and returns.
    // Off unless asked for, and two counters when it is.
    // The same warning vkAcquireNextImageKHR already carries, for the
    // same reason: a present that takes this long is not a frame, it is a
    // wait, and which call is doing the waiting decides whose problem it
    // is. Rate-limited, and silent on any healthy frame.
    {
        const double present_ms = std::chrono::duration<double, std::milli>(
            present_t1 - present_t0).count();
        if (present_ms > 50.0) {
            static int said = 0;
            if (said < 24) {
                ++said;
                // Split, because the total answers nothing on its own.
                //
                // present_t0 is taken BEFORE the queue lock, so this span
                // covers two quite different waits: queuing behind another
                // thread that holds the queue, which is Stud's own
                // problem, and the driver's own call, which is the
                // compositor's or the GPU's. A twelve-second present was
                // traced with only the total to go on and the two were
                // indistinguishable; they are not any more.
                const double lock_ms = std::chrono::duration<double, std::milli>(
                    lock_t - present_t0).count();
                const double driver_ms = std::chrono::duration<double, std::milli>(
                    present_t1 - lock_t).count();
                std::printf("stud-render-host: SLOW vkQueuePresentKHR %.1fms -> %d "
                            "(waiting for the queue %.1fms, inside the driver %.1fms)\n",
                            present_ms, static_cast<int>(res), lock_ms, driver_ms);
                std::fflush(stdout);
            }
        }
    }
    // A parked device, released once the replacement has proved itself.
    //
    // The proof is frames: the engine cannot have presented this many on a
    // device it did not finish building, and by then nothing of Stud's can
    // still be inside a call on the old one -- every device command in
    // flight was issued against the device these presents are going to.
    // That is what makes destroying it, and unmapping what it imported,
    // safe here and not at the moment the engine said it was finished.
    //
    // Deliberately frames rather than a timer: a stall is exactly when a
    // timer would fire early, and a stall is exactly when this must not.
    if (!g_retired_devices.empty()) {
        constexpr uint64_t kProofFrames = 240;
        if (g_presents_since_new_device.fetch_add(1, std::memory_order_relaxed) + 1 >=
            kProofFrames) {
            std::vector<RetiredDevice> releasing;
            releasing.swap(g_retired_devices);
            for (RetiredDevice& d : releasing) {
                if (d.device == VK_NULL_HANDLE) continue;
                if (d.repair_fence != VK_NULL_HANDLE && l.destroy_fence != nullptr) {
                    l.destroy_fence(d.device, d.repair_fence, nullptr);
                }
                if (d.probe_pool != VK_NULL_HANDLE && l.destroy_command_pool != nullptr) {
                    l.destroy_command_pool(d.device, d.probe_pool, nullptr);
                }
                if (l.device_wait_idle != nullptr) l.device_wait_idle(d.device);
                if (l.destroy_device != nullptr) l.destroy_device(d.device, nullptr);
                // Only now. While that device lived, the driver was still
                // importing these pages.
                for (auto& w : d.writes) {
                    if (w.second.first != nullptr) ::munmap(w.second.first, w.second.second);
                }
                std::printf("stud-render-host: released a device the engine finished with, and "
                            "the %zu mapping(s) it held\n", d.writes.size());
                std::fflush(stdout);
            }
        }
    }

    // A census of what Stud itself is still holding.
    //
    // Some sessions freeze repeatedly and some never do, with identical
    // startup logs, and the user's reading is that something accumulates
    // on the bad ones. Nothing Stud printed could confirm or deny that:
    // the write barrier's own counters are flat, texture errors do not
    // correlate, and an event-rate profile of a freezing session against
    // a clean one differs only in how much the player moved.
    //
    // So this counts the things that could grow without anyone noticing.
    // Every one of them is a container Stud adds to and is supposed to
    // erase from; a number that climbs across a session and never comes
    // down is the accumulation, and a session where they all stay flat
    // says the idea is wrong. Printed rarely, so it costs nothing.
    {
        static uint64_t presents_seen = 0;
        if ((++presents_seen % 1800) == 0) {
            size_t shared = 0;
            size_t buffers = 0;
            {
                std::lock_guard<std::mutex> lock(shared_memory_mutex());
                shared = shared_memory().size();
            }
            {
                std::lock_guard<std::mutex> lock(buffer_memory_mutex());
                buffers = buffer_memory().size();
            }
            size_t retired = 0;
            {
                std::lock_guard<std::mutex> lock(g_retired_chains_mutex);
                retired = g_retired_chains.size();
            }
            std::printf("stud-render-host: still held after %llu presents: %zu shared mappings, "
                        "%zu buffer bindings, %zu upscale chains, %zu retired chains, "
                        "%zu swapchain extents\n",
                        static_cast<unsigned long long>(presents_seen), shared, buffers,
                        g_upscale_chains.size(), retired, g_swapchain_extents.size());
            std::fflush(stdout);
        }
    }

    static const bool time_presents = std::getenv("STUD_VK_HOST_TIME") != nullptr;
    if (time_presents) {
        static int n = 0;
        static double wait_us = 0.0;
        static double call_us = 0.0;
        static int suboptimal = 0;
        wait_us += std::chrono::duration<double, std::micro>(lock_t - present_t0).count();
        call_us += std::chrono::duration<double, std::micro>(present_t1 - lock_t).count();
        if (res == VK_SUBOPTIMAL_KHR) ++suboptimal;
        if (++n >= 120) {
            std::printf("stud-render-host: %d presents: queue lock %.2fms, driver %.2fms, "
                        "%d suboptimal\n", n, wait_us / 1000.0 / n, call_us / 1000.0 / n,
                        suboptimal);
            std::fflush(stdout);
            n = 0;
            wait_us = 0.0;
            call_us = 0.0;
            suboptimal = 0;
        }
    }
    note_result(res, "vkQueuePresentKHR");
    if (res == VK_ERROR_OUT_OF_DATE_KHR && ns > 0 && !chains.empty()) {
        // Same reasoning as the acquire above: rebuild and report success.
        // The frame itself is lost, which is one frame during a resize.
        if (recreate_real_swapchain(to_u64(chains[0]))) res = VK_SUCCESS;
    }
    static int presents = 0;
    // Which thread presents matters: only the main loop pumps Wayland,
    // and a driver completing a present may need the display dispatched.
    // So it is worth saying when it CHANGES, not once every two seconds
    // for the life of the session. These two lines were 615 of one
    // in-game log.
    // The window is shown on the first frame rather than at creation,
    // so it never sits empty through the engine's bring-up. No-op on
    // Wayland and after the first call.
    stud::android_glue::x11_ensure_mapped();
    // Cached per thread, not asked per present. gettid() has no vDSO
    // entry, so this was a real syscall on every frame to answer a
    // question whose answer cannot change for a given thread -- and the
    // line it feeds prints once or twice in a session.
    static thread_local const int present_tid = static_cast<int>(::syscall(SYS_gettid));
    static int announced_tid = -1;
    if (present_tid != announced_tid) {
        announced_tid = present_tid;
        std::printf("stud-render-host: present on tid=%d (main=%d)\n", present_tid,
                    static_cast<int>(::getpid()));
        std::fflush(stdout);
    }
    // A failure, or a heartbeat rare enough to be worth reading (~a minute
    // at 60fps). The first few frames also say the pipeline started, which
    // is worth a line only while tracing.
    if (res != VK_SUCCESS || (presents % 3600) == 0 ||
        (presents < 5 && vk_object_trace_enabled())) {
        std::printf("stud-render-host: vkQueuePresentKHR #%d -> %d (%u swapchain(s))\n", presents,
                    static_cast<int>(res), ns);
        std::fflush(stdout);
    }
    ++presents;
    if (res != VK_SUCCESS && res != VK_SUBOPTIMAL_KHR && ns > 0 && !chains.empty() &&
        !indices.empty()) {
        auto failed = g_upscale_chains.find(to_u64(chains[0]));
        if (failed != g_upscale_chains.end()) {
            UpscaleChain& fc = failed->second;
            const uint32_t index = indices[0];
            if (index < fc.done.size() && l.create_semaphore != nullptr) {
                fc.retired.push_back(fc.done[index]);
                VkSemaphoreCreateInfo sci{};
                sci.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;
                VkSemaphore fresh = VK_NULL_HANDLE;
                if (l.create_semaphore(l.device, &sci, nullptr, &fresh) == VK_SUCCESS) {
                    fc.done[index] = fresh;
                    static int said = 0;
                    if (said < 8) {
                        ++said;
                        std::printf("stud-render-host: a present failed, so the semaphore it "
                                    "should have consumed was retired rather than signalled "
                                    "twice\n");
                        std::fflush(stdout);
                    }
                }
            }
        }
    }
    // Suboptimal is Stud's business, not the engine's; see engine_visible_result().
    return engine_visible_result(res);
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
// The commands that move pixels and bytes between buffers and images.
//
// Six arms of vk_cmd_record()'s switch, together because they are the same
// shape: read a source, a destination, a layout pair and a region list off
// the wire, then hand them to the driver. They share nothing with the
// render-pass, binding and draw arms beside them.
//
// The arms below still `break` rather than return, exactly as they did
// inside the larger switch, and that is equivalent: vk_cmd_record() does
// nothing after its switch but `return 0`.
void record_copy_command(vk_wire::CmdKind kind, Loader& l, VkCommandBuffer cb,
                         vk_wire::Reader& r) {
    using K = vk_wire::CmdKind;
    switch (kind) {
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
            // it, then the GPU writes straight into the pages the
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
                                    "memory this process does not share with the engine, the "
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
                if (blits < 3 && vk_object_trace_enabled()) {
                    std::printf("stud-render-host: blit #%d draws to the screen\n", blits);
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
        default:
            break;
    }
}



// One entry point for the whole vkCmd* family. Every member takes a
// command buffer, returns nothing, and differs only in payload, so
// they share a call id and, on the client side, the reply-free path.
uint64_t vk_cmd_record(uint64_t cb_handle, uint32_t kind, const uint8_t* data, size_t size) {
    Loader& l = loader();
    VkCommandBuffer cb = from_u64<VkCommandBuffer>(cb_handle);
    vk_wire::Reader r(data, size);
    using K = vk_wire::CmdKind;

    // Which command was being recorded, for the crash handler. A
    // backtrace that stops at this function names the function and not
    // the command, which is the one thing needed to act on it.
    // Left on deliberately. It is what makes a crash report name the last
    // Vulkan command instead of only the function it died in, which is
    // the one thing needed to act on a user's crash log.
    {
        char note[96];
        std::snprintf(note, sizeof(note), "vk_cmd_record kind=%u cb=%llx in=%zu", kind,
                      static_cast<unsigned long long>(cb_handle), size);
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
            const bool trace_passes = vk_trace_passes_enabled();
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
                if (to_screen < 3 && vk_object_trace_enabled()) {
                    std::printf("stud-render-host: render pass #%d draws to the screen\n",
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
                const bool trace = vk_trace_passes_enabled();
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
            // Not const: make_barrier_legal() may widen them below.
            VkPipelineStageFlags src_stage = r.u32(), dst_stage = r.u32();
            const uint32_t dep_flags = r.u32();
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
            // Same treatment for buffers, for the same reason, a
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
            // records these at all, in short, a resize makes the
            // acquire fail and the frame already in flight carries on
            // regardless. Passing the freed handle through crashes the
            // driver inside render-host, taking the whole session with
            // it; dropping the barrier loses a layout transition for an
            // image that is never going to be presented anyway.
            {
                const size_t before = img.size();
                // Counted apart, because the two arms mean opposite
                // things. A null handle is the engine's own doing and
                // dropping it is free. A RETIRED image means Stud
                // decided that image was dead while the engine still
                // thought otherwise, and then a real layout transition
                // is being silently thrown away, which is Stud's bug.
                static uint64_t dropped_null = 0;
                static uint64_t dropped_retired = 0;
                for (const VkImageMemoryBarrier& b : img) {
                    if (b.image == VK_NULL_HANDLE) {
                        ++dropped_null;
                    } else if (l.retired_images.count(to_u64(b.image)) != 0) {
                        ++dropped_retired;
                    }
                }
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
                    // Armed by default this wrote a line every 64 drops
                    // 2.1 million of them in one real session. It is
                    // investigation output; it says so now.
                    static const bool trace = std::getenv("STUD_VK_TRACE_BARRIERS") != nullptr;
                    static uint64_t announced = 0;
                    if (trace && (dropped >= announced + 256 || announced == 0)) {
                        announced = dropped;
                        std::printf("stud-render-host: dropped %llu image barrier(s): %llu with "
                                    "no image, %llu against a retired one\n",
                                    static_cast<unsigned long long>(dropped),
                                    static_cast<unsigned long long>(dropped_null),
                                    static_cast<unsigned long long>(dropped_retired));
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
            // job to record, so it is worth knowing whether it does.
            const bool trace_barriers = vk_trace_passes_enabled();
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
            make_barrier_legal(&src_stage, &dst_stage, mem, buf, img);
            l.cmd_pipeline_barrier(cb, src_stage, dst_stage, dep_flags,
                                    static_cast<uint32_t>(mem.size()),
                                    mem.empty() ? nullptr : mem.data(),
                                    static_cast<uint32_t>(buf.size()),
                                    buf.empty() ? nullptr : buf.data(),
                                    static_cast<uint32_t>(img.size()),
                                    img.empty() ? nullptr : img.data());
            break;
        }
        case K::CopyBuffer:
        case K::CopyBufferToImage:
        case K::CopyImageToBuffer:
        case K::CopyImage:
        case K::BlitImage:
        case K::ResolveImage:
            record_copy_command(static_cast<K>(kind), l, cb, r);
            break;
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
