// Minimal Vulkan presentation test against the same real Wayland window
// Stud uses. No engine, no IPC, no bionic, just: create a window,
// build a swapchain, clear it to a solid colour, present.
//
// This exists to answer one question that reasoning could not settle:
// when Stud's Vulkan path renders a full frame, reports success at every
// call, attaches and commits real dmabuf buffers, and the window is
// still black, is the fault in Stud, or in this machine's
// Vulkan/Wayland presentation path itself?
//
// If this shows colour, presentation works and Stud is doing something
// wrong. If this is black too, nothing Stud does to its Vulkan path can
// fix it and the problem is below us.

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

#include <vulkan/vulkan_core.h>
#include <vulkan/vulkan_wayland.h>

#include "stud/android_glue.h"
#include "stud/ndk_types.h"

namespace {

#define CHECK(expr, what)                                                     \
    do {                                                                      \
        VkResult r_ = (expr);                                                 \
        if (r_ != VK_SUCCESS) {                                               \
            std::printf("FAIL %s -> %d\n", what, static_cast<int>(r_));       \
            return 1;                                                         \
        }                                                                     \
    } while (0)

}  // namespace

int main() {
    ANativeWindow* window = ANativeWindow_fromSurface(nullptr, nullptr);
    if (window == nullptr) {
        std::printf("FAIL no window\n");
        return 1;
    }
    wl_display* display = stud::android_glue::native_window_wl_display(window);
    wl_surface* surface = stud::android_glue::native_window_wl_surface(window);
    const uint32_t width = static_cast<uint32_t>(ANativeWindow_getWidth(window));
    const uint32_t height = static_cast<uint32_t>(ANativeWindow_getHeight(window));
    std::printf("window %ux%u display=%p surface=%p\n", width, height,
                static_cast<void*>(display), static_cast<void*>(surface));

    VkApplicationInfo app{};
    app.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
    app.pApplicationName = "stud_try_vulkan_window";
    app.apiVersion = VK_API_VERSION_1_1;
    const char* exts[] = {"VK_KHR_surface", "VK_KHR_wayland_surface"};
    VkInstanceCreateInfo ici{};
    ici.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
    ici.pApplicationInfo = &app;
    ici.enabledExtensionCount = 2;
    ici.ppEnabledExtensionNames = exts;
    VkInstance instance = VK_NULL_HANDLE;
    CHECK(vkCreateInstance(&ici, nullptr, &instance), "vkCreateInstance");

    VkWaylandSurfaceCreateInfoKHR sci{};
    sci.sType = VK_STRUCTURE_TYPE_WAYLAND_SURFACE_CREATE_INFO_KHR;
    sci.display = display;
    sci.surface = surface;
    VkSurfaceKHR vk_surface = VK_NULL_HANDLE;
    CHECK(vkCreateWaylandSurfaceKHR(instance, &sci, nullptr, &vk_surface), "wayland surface");

    uint32_t n = 0;
    vkEnumeratePhysicalDevices(instance, &n, nullptr);
    std::vector<VkPhysicalDevice> devices(n);
    vkEnumeratePhysicalDevices(instance, &n, devices.data());
    // STUD_VK_GPU=<substring> picks a specific GPU by name. The default
    // is whichever device presents first, which on a hybrid laptop is the
    // integrated one, and the whole question here is whether the
    // discrete GPU behaves the same, since that is the one Stud's engine
    // selects.
    const char* want = std::getenv("STUD_VK_GPU");
    VkPhysicalDevice pd = VK_NULL_HANDLE;
    uint32_t family = 0;
    for (VkPhysicalDevice d : devices) {
        if (want != nullptr) {
            VkPhysicalDeviceProperties p{};
            vkGetPhysicalDeviceProperties(d, &p);
            if (std::strstr(p.deviceName, want) == nullptr) continue;
        }
        uint32_t qn = 0;
        vkGetPhysicalDeviceQueueFamilyProperties(d, &qn, nullptr);
        std::vector<VkQueueFamilyProperties> qs(qn);
        vkGetPhysicalDeviceQueueFamilyProperties(d, &qn, qs.data());
        for (uint32_t i = 0; i < qn; ++i) {
            VkBool32 present = VK_FALSE;
            vkGetPhysicalDeviceSurfaceSupportKHR(d, i, vk_surface, &present);
            if ((qs[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) != 0 && present == VK_TRUE) {
                pd = d;
                family = i;
                break;
            }
        }
        if (pd != VK_NULL_HANDLE) break;
    }
    if (pd == VK_NULL_HANDLE) {
        std::printf("FAIL no device can present to this surface\n");
        return 1;
    }
    VkPhysicalDeviceProperties props{};
    vkGetPhysicalDeviceProperties(pd, &props);
    std::printf("device: %s (queue family %u)\n", props.deviceName, family);

    const float prio = 1.0f;
    VkDeviceQueueCreateInfo qci{};
    qci.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
    qci.queueFamilyIndex = family;
    qci.queueCount = 1;
    qci.pQueuePriorities = &prio;
    const char* dexts[] = {"VK_KHR_swapchain"};
    VkDeviceCreateInfo dci{};
    dci.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
    dci.queueCreateInfoCount = 1;
    dci.pQueueCreateInfos = &qci;
    dci.enabledExtensionCount = 1;
    dci.ppEnabledExtensionNames = dexts;
    VkDevice device = VK_NULL_HANDLE;
    CHECK(vkCreateDevice(pd, &dci, nullptr, &device), "vkCreateDevice");
    VkQueue queue = VK_NULL_HANDLE;
    vkGetDeviceQueue(device, family, 0, &queue);

    VkSurfaceCapabilitiesKHR caps{};
    vkGetPhysicalDeviceSurfaceCapabilitiesKHR(pd, vk_surface, &caps);
    VkExtent2D extent = caps.currentExtent;
    if (extent.width == 0xFFFFFFFFu) extent = {width, height};
    std::printf("currentExtent %ux%u -> using %ux%u\n", caps.currentExtent.width,
                caps.currentExtent.height, extent.width, extent.height);

    VkSwapchainCreateInfoKHR swci{};
    swci.sType = VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR;
    swci.surface = vk_surface;
    swci.minImageCount = caps.minImageCount < 3 ? 3 : caps.minImageCount;
    swci.imageFormat = VK_FORMAT_B8G8R8A8_UNORM;
    swci.imageColorSpace = VK_COLOR_SPACE_SRGB_NONLINEAR_KHR;
    swci.imageExtent = extent;
    swci.imageArrayLayers = 1;
    swci.imageUsage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    swci.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE;
    swci.preTransform = caps.currentTransform;
    swci.compositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
    swci.presentMode = VK_PRESENT_MODE_FIFO_KHR;
    swci.clipped = VK_TRUE;
    VkSwapchainKHR swapchain = VK_NULL_HANDLE;
    CHECK(vkCreateSwapchainKHR(device, &swci, nullptr, &swapchain), "vkCreateSwapchainKHR");

    uint32_t image_count = 0;
    vkGetSwapchainImagesKHR(device, swapchain, &image_count, nullptr);
    std::vector<VkImage> images(image_count);
    vkGetSwapchainImagesKHR(device, swapchain, &image_count, images.data());
    std::printf("swapchain: %u images\n", image_count);

    VkCommandPoolCreateInfo pci{};
    pci.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
    pci.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    pci.queueFamilyIndex = family;
    VkCommandPool pool = VK_NULL_HANDLE;
    CHECK(vkCreateCommandPool(device, &pci, nullptr, &pool), "vkCreateCommandPool");
    VkCommandBufferAllocateInfo cbai{};
    cbai.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    cbai.commandPool = pool;
    cbai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    cbai.commandBufferCount = 1;
    VkCommandBuffer cb = VK_NULL_HANDLE;
    CHECK(vkAllocateCommandBuffers(device, &cbai, &cb), "vkAllocateCommandBuffers");

    VkSemaphoreCreateInfo semi{};
    semi.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;
    VkSemaphore acquired = VK_NULL_HANDLE, rendered = VK_NULL_HANDLE;
    vkCreateSemaphore(device, &semi, nullptr, &acquired);
    vkCreateSemaphore(device, &semi, nullptr, &rendered);

    // 300 frames of solid magenta, cleared straight into the swapchain
    // image with vkCmdClearColorImage: no render pass, no pipeline, the
    // least machinery that can put a colour on screen.
    for (int frame = 0; frame < 300; ++frame) {
        uint32_t index = 0;
        VkResult ar = vkAcquireNextImageKHR(device, swapchain, UINT64_MAX, acquired,
                                             VK_NULL_HANDLE, &index);
        if (ar != VK_SUCCESS && ar != VK_SUBOPTIMAL_KHR) {
            std::printf("FAIL acquire -> %d\n", static_cast<int>(ar));
            return 1;
        }

        vkResetCommandBuffer(cb, 0);
        VkCommandBufferBeginInfo bi{};
        bi.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
        bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        vkBeginCommandBuffer(cb, &bi);

        VkImageMemoryBarrier to_dst{};
        to_dst.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
        to_dst.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        to_dst.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        to_dst.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        to_dst.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        to_dst.image = images[index];
        to_dst.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        to_dst.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                              VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0, nullptr, 1,
                              &to_dst);

        VkClearColorValue colour{};
        colour.float32[0] = 1.0f;
        colour.float32[1] = 0.0f;
        colour.float32[2] = 1.0f;
        colour.float32[3] = 1.0f;
        VkImageSubresourceRange range = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        vkCmdClearColorImage(cb, images[index], VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &colour, 1,
                              &range);

        VkImageMemoryBarrier to_present = to_dst;
        to_present.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        to_present.newLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
        to_present.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        to_present.dstAccessMask = 0;
        vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TRANSFER_BIT,
                              VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, 0, 0, nullptr, 0, nullptr, 1,
                              &to_present);
        vkEndCommandBuffer(cb);

        VkPipelineStageFlags wait_stage = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
        VkSubmitInfo si{};
        si.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
        si.waitSemaphoreCount = 1;
        si.pWaitSemaphores = &acquired;
        si.pWaitDstStageMask = &wait_stage;
        si.commandBufferCount = 1;
        si.pCommandBuffers = &cb;
        si.signalSemaphoreCount = 1;
        si.pSignalSemaphores = &rendered;
        CHECK(vkQueueSubmit(queue, 1, &si, VK_NULL_HANDLE), "vkQueueSubmit");

        VkPresentInfoKHR pi{};
        pi.sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR;
        pi.waitSemaphoreCount = 1;
        pi.pWaitSemaphores = &rendered;
        pi.swapchainCount = 1;
        pi.pSwapchains = &swapchain;
        pi.pImageIndices = &index;
        VkResult pr = vkQueuePresentKHR(queue, &pi);
        if (pr != VK_SUCCESS && pr != VK_SUBOPTIMAL_KHR) {
            std::printf("FAIL present -> %d\n", static_cast<int>(pr));
            return 1;
        }
        vkQueueWaitIdle(queue);

        if (frame == 0 || frame == 60) {
            std::printf("frame %d presented ok\n", frame);
            std::fflush(stdout);
        }
        stud::android_glue::poll_orphaned_loopers_once();
    }

    std::printf("OK: 300 magenta frames presented\n");
    return 0;
}
