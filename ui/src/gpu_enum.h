#pragma once

#include "stud/settings.h"

#include <cstdint>
#include <string>
#include <vector>

// The GPUs the system Vulkan loader lists, for Settings' GPU picker and for
// the launch to pick the chosen one.

namespace stud::ui {

struct GpuInfo {
    uint32_t device_index = 0;  // vkEnumeratePhysicalDevices()'s own result order
    std::string name;
    // PCI vendor id, as Vulkan reports it. Needed because selecting a GPU
    // for the OpenGL path is not a Vulkan question: GLX hands out the
    // system default, and which environment variable moves it off that
    // depends on whose driver is being asked (NVIDIA's PRIME offload, or
    // Mesa's DRI_PRIME).
    uint32_t vendor_id = 0;
    // Whether it is a discrete GPU (VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU).
    bool discrete = false;
    // VkPhysicalDeviceIDProperties::deviceUUID in hex, which stays the same
    // when the enumeration order does not. Empty on a Vulkan 1.0 loader.
    std::string uuid;
};

// Not stubbed: creates a throwaway VkInstance, enumerates real
// physical devices, reads each one's real vkGetPhysicalDeviceProperties
// name. Returns an empty vector (not an error) if no Vulkan-capable
// instance/devices are available, the settings window should show
// "no GPUs found" rather than crash, matching this project's established
// honest-degradation pattern.
std::vector<GpuInfo> enumerate_gpus();

// Which device a machine that has never been asked should use.
//
// Not index 0: Vulkan's enumeration order is the driver's, not a
// ranking, and on a hybrid laptop the integrated GPU commonly comes
// first, measured here, where index 0 is an Intel Iris Xe and index 1
// is an RTX 3050. Defaulting to 0 quietly hands a new install the slower
// device, with nothing on screen to say so. A discrete device wins when
// one exists; otherwise this is 0 and nothing changes.
uint32_t default_gpu_index();

// The device a saved choice names today. An index is only a position, and a
// driver update or an external GPU can reorder the list, so the UUID decides
// and the index stands in when it cannot. Nothing chosen yet means
// default_gpu_index().
uint32_t chosen_gpu_index(const stud::config::GpuSelection& selection);

}  // namespace stud::ui
