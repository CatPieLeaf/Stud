// VK_LAYER_STUD_present: Stud's own swapchain (owned_swapchain.h) as a
// Vulkan layer.
//
// A layer rather than calls patched into render-host's own dispatch table,
// because of what sits between render-host and the driver. MangoHud draws
// its overlay from inside the swapchain commands it intercepts; patched in
// above it, Stud's swapchain took those commands away and the overlay was
// gone. As a layer it sits below every implicit layer -- the loader puts
// implicit layers closest to the application and the ones it is asked for
// after them -- so MangoHud keeps seeing an ordinary swapchain, and what
// it calls down into is Stud's instead of the driver's.
//
// render-host enables it for its whole process on Wayland and X11
// (VK_ADD_LAYER_PATH and VK_INSTANCE_LAYERS, before any Vulkan call), which
// also covers the instance ANGLE creates for itself.

#include "volk.h"

#include <vulkan/vk_layer.h>

#include <algorithm>
#include <cstring>
#include <map>
#include <mutex>
#include <string>
#include <vector>

#include "owned_swapchain.h"

namespace os = stud::render_host::owned_swapchain;

namespace {

constexpr const char* kLayerName = "VK_LAYER_STUD_present";

// What the driver's swapchain offers and Stud's does not: present timing
// and per-present fences. Hidden, so nothing above asks Stud's swapchain
// for either.
//
// Both names of swapchain_maintenance1: the KHR one offers the same
// per-present fences as the EXT one, and a fence chained to a present on
// Stud's swapchain is never signalled, so anything that waits on it waits
// forever. Spelled out rather than taken from the headers, since older
// Vulkan headers lack the KHR one.
//
// Only what Stud's swapchain is known not to honour. present_id2 and
// present_wait2 were hidden as well once, and the Vulkan path played worse
// for it, measured; nothing here was found to read them.
const char* const kHidden[] = {
    "VK_KHR_present_wait",
    "VK_KHR_present_id",
    "VK_EXT_swapchain_maintenance1",
    "VK_KHR_swapchain_maintenance1",
};

bool hidden(const char* name) {
    for (const char* h : kHidden) {
        if (std::strcmp(h, name) == 0) return true;
    }
    return false;
}

struct Instance {
    PFN_vkGetInstanceProcAddr gipa = nullptr;
    VolkInstanceTable vk{};  // the next layer's instance commands
};

struct Device {
    PFN_vkGetDeviceProcAddr gdpa = nullptr;
    PFN_vkDestroyDevice destroy = nullptr;
    bool owned = false;
};

std::mutex g_mutex;
std::map<void*, Instance> g_instances;  // by dispatch key, shared with physical devices
std::map<void*, Device> g_devices;      // by dispatch key, shared with queues
// Which physical devices can run Stud's swapchain at all, asked once each.
std::map<VkPhysicalDevice, std::vector<std::string>> g_capable;

void* key(const void* dispatchable) {
    return *static_cast<void* const*>(dispatchable);
}

Instance instance_of(const void* dispatchable) {
    std::lock_guard<std::mutex> lock(g_mutex);
    const auto it = g_instances.find(key(dispatchable));
    return it == g_instances.end() ? Instance{} : it->second;
}

const std::vector<std::string>& capable(VkPhysicalDevice physical_device) {
    std::lock_guard<std::mutex> lock(g_mutex);
    auto it = g_capable.find(physical_device);
    if (it == g_capable.end()) {
        it = g_capable.emplace(physical_device, os::device_extensions(physical_device)).first;
    }
    return it->second;
}

VKAPI_ATTR VkResult VKAPI_CALL create_instance(const VkInstanceCreateInfo* ci,
                                               const VkAllocationCallbacks* alloc,
                                               VkInstance* out) {
    auto* link = static_cast<VkLayerInstanceCreateInfo*>(const_cast<void*>(ci->pNext));
    while (link != nullptr && !(link->sType == VK_STRUCTURE_TYPE_LOADER_INSTANCE_CREATE_INFO &&
                                link->function == VK_LAYER_LINK_INFO)) {
        link = static_cast<VkLayerInstanceCreateInfo*>(const_cast<void*>(link->pNext));
    }
    if (link == nullptr) return VK_ERROR_INITIALIZATION_FAILED;
    const PFN_vkGetInstanceProcAddr gipa = link->u.pLayerInfo->pfnNextGetInstanceProcAddr;
    link->u.pLayerInfo = link->u.pLayerInfo->pNext;
    auto create = reinterpret_cast<PFN_vkCreateInstance>(gipa(VK_NULL_HANDLE, "vkCreateInstance"));
    const VkResult r = create(ci, alloc, out);
    if (r != VK_SUCCESS) return r;

    // owned_swapchain asks the physical device through volk's globals;
    // pointed at the next layer down. One process, one loader, one chain
    // below this layer, so the last instance's are as good as any.
    volkInitializeCustom(gipa);
    volkLoadInstanceOnly(*out);

    Instance inst;
    inst.gipa = gipa;
    volkLoadInstanceTable(&inst.vk, *out);
    std::lock_guard<std::mutex> lock(g_mutex);
    g_instances[key(*out)] = inst;
    return VK_SUCCESS;
}

VKAPI_ATTR void VKAPI_CALL destroy_instance(VkInstance instance, const VkAllocationCallbacks* alloc) {
    PFN_vkDestroyInstance destroy = nullptr;
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        const auto it = g_instances.find(key(instance));
        if (it == g_instances.end()) return;
        destroy = it->second.vk.vkDestroyInstance;
        g_instances.erase(it);
        // Physical device handles belong to an instance, so a later
        // instance can hand out the same address for another GPU. Asked
        // again rather than remembered.
        g_capable.clear();
    }
    destroy(instance, alloc);
}

VKAPI_ATTR VkResult VKAPI_CALL create_wayland_surface(VkInstance instance,
                                                      const VkWaylandSurfaceCreateInfoKHR* ci,
                                                      const VkAllocationCallbacks* alloc,
                                                      VkSurfaceKHR* out) {
    const Instance inst = instance_of(instance);
    if (inst.vk.vkCreateWaylandSurfaceKHR == nullptr) return VK_ERROR_EXTENSION_NOT_PRESENT;
    const VkResult r = inst.vk.vkCreateWaylandSurfaceKHR(instance, ci, alloc, out);
    if (r == VK_SUCCESS) os::note_wayland_surface(*out, ci->display, ci->surface);
    return r;
}

VKAPI_ATTR VkResult VKAPI_CALL create_xlib_surface(VkInstance instance,
                                                   const VkXlibSurfaceCreateInfoKHR* ci,
                                                   const VkAllocationCallbacks* alloc,
                                                   VkSurfaceKHR* out) {
    const Instance inst = instance_of(instance);
    if (inst.vk.vkCreateXlibSurfaceKHR == nullptr) return VK_ERROR_EXTENSION_NOT_PRESENT;
    const VkResult r = inst.vk.vkCreateXlibSurfaceKHR(instance, ci, alloc, out);
    if (r == VK_SUCCESS) os::note_xlib_surface(*out, ci->dpy, ci->window);
    return r;
}

VKAPI_ATTR VkResult VKAPI_CALL create_xcb_surface(VkInstance instance,
                                                  const VkXcbSurfaceCreateInfoKHR* ci,
                                                  const VkAllocationCallbacks* alloc,
                                                  VkSurfaceKHR* out) {
    const Instance inst = instance_of(instance);
    if (inst.vk.vkCreateXcbSurfaceKHR == nullptr) return VK_ERROR_EXTENSION_NOT_PRESENT;
    const VkResult r = inst.vk.vkCreateXcbSurfaceKHR(instance, ci, alloc, out);
    if (r == VK_SUCCESS) os::note_xcb_surface(*out, ci->connection, ci->window);
    return r;
}

VKAPI_ATTR void VKAPI_CALL destroy_surface(VkInstance instance, VkSurfaceKHR surface,
                                           const VkAllocationCallbacks* alloc) {
    const Instance inst = instance_of(instance);
    os::forget_surface(surface);
    if (inst.vk.vkDestroySurfaceKHR != nullptr) inst.vk.vkDestroySurfaceKHR(instance, surface, alloc);
}

VKAPI_ATTR VkResult VKAPI_CALL enumerate_device_extensions(VkPhysicalDevice physical_device,
                                                           const char* layer,
                                                           uint32_t* count,
                                                           VkExtensionProperties* props) {
    if (layer != nullptr && std::strcmp(layer, kLayerName) == 0) {
        *count = 0;
        return VK_SUCCESS;
    }
    const Instance inst = instance_of(physical_device);
    if (layer != nullptr || capable(physical_device).empty()) {
        return inst.vk.vkEnumerateDeviceExtensionProperties(physical_device, layer, count, props);
    }
    uint32_t n = 0;
    VkResult r = inst.vk.vkEnumerateDeviceExtensionProperties(physical_device, nullptr, &n, nullptr);
    if (r != VK_SUCCESS) return r;
    std::vector<VkExtensionProperties> all(n);
    r = inst.vk.vkEnumerateDeviceExtensionProperties(physical_device, nullptr, &n, all.data());
    if (r != VK_SUCCESS && r != VK_INCOMPLETE) return r;
    all.resize(n);
    std::vector<VkExtensionProperties> shown;
    for (const auto& e : all) {
        if (!hidden(e.extensionName)) shown.push_back(e);
    }
    if (props == nullptr) {
        *count = static_cast<uint32_t>(shown.size());
        return VK_SUCCESS;
    }
    const uint32_t written = *count < shown.size() ? *count : static_cast<uint32_t>(shown.size());
    for (uint32_t i = 0; i < written; ++i) props[i] = shown[i];
    *count = written;
    return written < shown.size() ? VK_INCOMPLETE : VK_SUCCESS;
}

VKAPI_ATTR VkResult VKAPI_CALL create_device(VkPhysicalDevice physical_device,
                                             const VkDeviceCreateInfo* ci,
                                             const VkAllocationCallbacks* alloc, VkDevice* out) {
    auto* link = static_cast<VkLayerDeviceCreateInfo*>(const_cast<void*>(ci->pNext));
    while (link != nullptr && !(link->sType == VK_STRUCTURE_TYPE_LOADER_DEVICE_CREATE_INFO &&
                                link->function == VK_LAYER_LINK_INFO)) {
        link = static_cast<VkLayerDeviceCreateInfo*>(const_cast<void*>(link->pNext));
    }
    if (link == nullptr) return VK_ERROR_INITIALIZATION_FAILED;
    const PFN_vkGetInstanceProcAddr gipa = link->u.pLayerInfo->pfnNextGetInstanceProcAddr;
    const PFN_vkGetDeviceProcAddr gdpa = link->u.pLayerInfo->pfnNextGetDeviceProcAddr;
    link->u.pLayerInfo = link->u.pLayerInfo->pNext;
    auto create = reinterpret_cast<PFN_vkCreateDevice>(gipa(VK_NULL_HANDLE, "vkCreateDevice"));

    // The caller's extensions, less the hidden ones, plus what Stud's
    // swapchain needs where the device has it.
    const std::vector<std::string>& wanted = capable(physical_device);
    std::vector<std::string> names;
    for (uint32_t i = 0; i < ci->enabledExtensionCount; ++i) {
        const char* name = ci->ppEnabledExtensionNames[i];
        if (!wanted.empty() && hidden(name)) continue;
        names.emplace_back(name);
    }
    for (const std::string& name : wanted) {
        if (std::find(names.begin(), names.end(), name) == names.end()) names.push_back(name);
    }
    std::vector<const char*> pointers;
    for (const std::string& name : names) pointers.push_back(name.c_str());
    VkDeviceCreateInfo modified = *ci;
    modified.enabledExtensionCount = static_cast<uint32_t>(pointers.size());
    modified.ppEnabledExtensionNames = pointers.data();

    const VkResult r = create(physical_device, &modified, alloc, out);
    if (r != VK_SUCCESS) return r;

    // The next layer's commands for this device, through the entry point
    // the loader handed down for it.
    vkGetDeviceProcAddr = gdpa;
    VolkDeviceTable table{};
    volkLoadDeviceTable(&table, *out);
    Device dev;
    dev.gdpa = gdpa;
    dev.destroy = table.vkDestroyDevice;
    dev.owned = !wanted.empty() &&
                os::install(table, *out, physical_device,
                            ci->queueCreateInfoCount > 0 ? ci->pQueueCreateInfos[0].queueFamilyIndex
                                                         : 0);
    std::lock_guard<std::mutex> lock(g_mutex);
    g_devices[key(*out)] = dev;
    return VK_SUCCESS;
}

VKAPI_ATTR void VKAPI_CALL destroy_device(VkDevice device, const VkAllocationCallbacks* alloc) {
    PFN_vkDestroyDevice destroy = nullptr;
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        const auto it = g_devices.find(key(device));
        if (it == g_devices.end()) return;
        destroy = it->second.destroy;
        g_devices.erase(it);
    }
    os::forget_device(device);
    destroy(device, alloc);
}

VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL get_device_proc_addr(VkDevice device, const char* name);

VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL get_instance_proc_addr(VkInstance instance,
                                                                const char* name) {
#define STUD_INTERCEPT(vk_name, fn) \
    if (std::strcmp(name, vk_name) == 0) return reinterpret_cast<PFN_vkVoidFunction>(fn)
    STUD_INTERCEPT("vkGetInstanceProcAddr", get_instance_proc_addr);
    STUD_INTERCEPT("vkCreateInstance", create_instance);
    STUD_INTERCEPT("vkDestroyInstance", destroy_instance);
    STUD_INTERCEPT("vkCreateWaylandSurfaceKHR", create_wayland_surface);
    STUD_INTERCEPT("vkCreateXlibSurfaceKHR", create_xlib_surface);
    STUD_INTERCEPT("vkCreateXcbSurfaceKHR", create_xcb_surface);
    STUD_INTERCEPT("vkDestroySurfaceKHR", destroy_surface);
    STUD_INTERCEPT("vkEnumerateDeviceExtensionProperties", enumerate_device_extensions);
    STUD_INTERCEPT("vkCreateDevice", create_device);
    STUD_INTERCEPT("vkGetDeviceProcAddr", get_device_proc_addr);
#undef STUD_INTERCEPT
    if (instance == VK_NULL_HANDLE) return nullptr;
    const Instance inst = instance_of(instance);
    return inst.gipa != nullptr ? inst.gipa(instance, name) : nullptr;
}

VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL get_device_proc_addr(VkDevice device, const char* name) {
    if (std::strcmp(name, "vkGetDeviceProcAddr") == 0) {
        return reinterpret_cast<PFN_vkVoidFunction>(get_device_proc_addr);
    }
    if (std::strcmp(name, "vkDestroyDevice") == 0) {
        return reinterpret_cast<PFN_vkVoidFunction>(destroy_device);
    }
    Device dev;
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        const auto it = g_devices.find(key(device));
        if (it == g_devices.end()) return nullptr;
        dev = it->second;
    }
    if (dev.owned) {
        if (PFN_vkVoidFunction ours = os::device_function(name)) return ours;
    }
    return dev.gdpa(device, name);
}

}  // namespace

extern "C" __attribute__((visibility("default"))) VKAPI_ATTR VkResult VKAPI_CALL
vkNegotiateLoaderLayerInterfaceVersion(VkNegotiateLayerInterface* version) {
    if (version == nullptr || version->loaderLayerInterfaceVersion < 2) {
        return VK_ERROR_INITIALIZATION_FAILED;
    }
    version->loaderLayerInterfaceVersion = 2;
    version->pfnGetInstanceProcAddr = get_instance_proc_addr;
    version->pfnGetDeviceProcAddr = get_device_proc_addr;
    version->pfnGetPhysicalDeviceProcAddr = nullptr;
    return VK_SUCCESS;
}
