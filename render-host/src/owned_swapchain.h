#pragma once

#include "volk.h"

#include <string>
#include <vector>

struct wl_display;
struct wl_surface;

// Stud's own swapchain on Wayland, in place of the Vulkan driver's.
//
// Why it exists: on NVIDIA every measured 12s freeze sat inside the
// driver's presentation code -- vkQueuePresentKHR, vkDestroySwapchainKHR,
// vkDestroyDevice -- spinning with the GPU idle until an internal timeout
// gave up waiting for the compositor to hand a buffer back. Nothing Stud
// does around those calls can shorten a wait that lives inside them. So
// Stud makes none of them: the engine's swapchain images are plain images,
// and presenting copies a finished frame into a dma-buf of Stud's own and
// commits it to the window with explicit sync. A buffer the compositor has
// not released yet is simply not used; with none free the frame is
// dropped and the window keeps the last one. No call here waits on the
// compositor, so a late release costs one frame, never a freeze.
//
// What it decides from the system rather than assumes: which GPU the
// compositor uses (its dma-buf feedback names the device) against the one
// Vulkan renders on. The same GPU gets buffers in device memory, in a
// layout both sides list; a different one -- a PRIME laptop -- gets them
// in system memory, the only place the other GPU can read them from.
//
// It runs as a Vulkan layer, VK_LAYER_STUD_present (present_layer.cpp),
// so implicit layers such as MangoHud sit above it and see an ordinary
// swapchain. A swapchain it cannot own -- X11, a compositor without
// linux-dmabuf v4 or linux-drm-syncobj, a format both GPUs cannot share --
// is passed down to the driver unchanged.
namespace stud::render_host::owned_swapchain {

// The layer's vkCreateWaylandSurfaceKHR, for every surface made.
void note_wayland_surface(VkSurfaceKHR surface, wl_display* display, wl_surface* target);

// The layer's vkCreateDevice, before calling down: the extensions to add.
// Empty when the driver cannot do what the owned swapchain needs.
std::vector<std::string> device_extensions(VkPhysicalDevice physical_device);

// The layer's vkCreateDevice, after: the next layer's device commands.
// False when they lack what the owned swapchain needs.
bool install(const VolkDeviceTable& next, VkDevice device, VkPhysicalDevice physical_device,
             uint32_t queue_family);

// The layer's vkDestroyDevice.
void forget_device(VkDevice device);

// The layer's vkGetDeviceProcAddr: the swapchain commands it answers
// itself, null for everything else.
PFN_vkVoidFunction device_function(const char* name);

}  // namespace stud::render_host::owned_swapchain
