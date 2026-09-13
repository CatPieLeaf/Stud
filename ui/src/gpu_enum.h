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

// Which compressed texture formats a device can sample natively.
//
// This is the question that decides how much CPU a session spends on
// textures. Roblox ships its Android content in ETC2/EAC (and PVRTC for
// older hardware), which no desktop GPU can sample -- so Stud decodes
// those blocks on the CPU and re-encodes them to a BC format the device
// does support. Whether BC7 is available decides the quality of that
// re-encode; whether BC is available at all decides whether textures are
// stored compressed or uncompressed.
struct TextureFormatSupport {
    bool queried = false;   // false when no device could be opened at all
    bool etc2 = false;      // what the APK actually ships
    bool eac = false;
    bool astc_ldr = false;
    bool pvrtc = false;
    bool bc1 = false;       // the transcode targets
    bool bc3 = false;
    bool bc4_bc5 = false;
    bool bc7 = false;
};

// Asks one device, by the same index enumerate_gpus() reports.
TextureFormatSupport query_texture_formats(uint32_t device_index);

}  // namespace stud::ui
