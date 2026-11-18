#pragma once

#include "stud/settings.h"

#include <cstdint>
#include <string>
#include <vector>


namespace stud::ui {

struct GpuInfo {
    uint32_t device_index = 0;  // the DRM card number
    std::string name;
    // Picks the GPU selection variable: PRIME offload for NVIDIA, DRI_PRIME for Mesa.
    uint32_t vendor_id = 0;
    bool discrete = false;
    // "pci-<address>", stable when card numbers change.
    std::string uuid;
};

// The kernel's GPUs, named from the PCI ID database; read once per process.
std::vector<GpuInfo> enumerate_gpus();

// Whether any Vulkan driver is installed; without one Roblox runs on OpenGL.
bool vulkan_available();

inline constexpr uint32_t kVendorNvidia = 0x10de;
// Shown on the About cube's tenth click and the welcome page, on NVIDIA.
inline constexpr const char* kNvidiaNote =
    "This GPU model may experience stability due to a decent Vulkan memory allocator in its driver. "
    "It's recommended you use Vulkan if you experience stability.";

// Card order isn't a ranking. Prefers the GPU that didn't boot the display, else 0.
uint32_t default_gpu_index();

// The PCI address decides and the index is a fallback. Nothing saved, or a choice
// from the old Vulkan list, means default_gpu_index().
uint32_t chosen_gpu_index(const stud::config::GpuSelection& selection);

}  // namespace stud::ui
