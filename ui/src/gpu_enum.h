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
    // PCI vendor id, as Vulkan reports it. Needed because selecting a GPU
    // for the OpenGL path is not a Vulkan question: GLX hands out the
    // system default, and which environment variable moves it off that
    // depends on whose driver is being asked (NVIDIA's PRIME offload, or
    // Mesa's DRI_PRIME).
    uint32_t vendor_id = 0;
    // Whether it is a discrete GPU (VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU).
    bool discrete = false;
};

// Real, not stubbed: creates a throwaway VkInstance, enumerates real
// physical devices, reads each one's real vkGetPhysicalDeviceProperties
// name. Returns an empty vector (not an error) if no Vulkan-capable
// instance/devices are available -- the settings window should show
// "no GPUs found" rather than crash, matching this project's established
// honest-degradation pattern.
std::vector<GpuInfo> enumerate_gpus();

// Which device a machine that has never been asked should use.
//
// Not index 0: Vulkan's enumeration order is the driver's, not a
// ranking, and on a hybrid laptop the integrated GPU commonly comes
// first -- measured here, where index 0 is an Intel Iris Xe and index 1
// is an RTX 3050. Defaulting to 0 quietly hands a new install the slower
// device, with nothing on screen to say so. A discrete device wins when
// one exists; otherwise this is 0 and nothing changes.
uint32_t default_gpu_index();

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
