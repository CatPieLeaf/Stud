#pragma once

#include <cstdint>
#include <string>
#include <vector>

// Real GPU enumeration for the settings window's GPU picker (M8, locked
// decision: "Settings dropdown enumerating vkEnumeratePhysicalDevices
// output"). Stud's OWN direct Vulkan usage -- unlike vulkan-wsi/ (which
// exists specifically to intercept Roblox's dlopen of libvulkan.so.1),
// this code is the UI process talking to the real system Vulkan loader
// directly, so linking Vulkan::Vulkan here is correct and has nothing to
// do with the interception concern vulkan-wsi/ solves for the runtime
// process.

namespace stud::ui {

struct GpuInfo {
    uint32_t device_index = 0;  // vkEnumeratePhysicalDevices()'s own result order
    std::string name;
};

// Real, not stubbed: creates a throwaway VkInstance, enumerates real
// physical devices, reads each one's real vkGetPhysicalDeviceProperties
// name. Returns an empty vector (not an error) if no Vulkan-capable
// instance/devices are available -- the settings window should show
// "no GPUs found" rather than crash, matching this project's established
// honest-degradation pattern.
std::vector<GpuInfo> enumerate_gpus();

}  // namespace stud::ui
