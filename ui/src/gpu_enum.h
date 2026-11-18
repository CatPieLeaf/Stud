#pragma once

#include "stud/settings.h"

#include <cstdint>
#include <string>
#include <vector>


namespace stud::ui {

struct GpuInfo {
    uint32_t device_index = 0;  // vkEnumeratePhysicalDevices order
    std::string name;
    // Picks the GPU selection variable: PRIME offload for NVIDIA, DRI_PRIME for Mesa.
    uint32_t vendor_id = 0;
    bool discrete = false;
    // deviceUUID in hex, stable when the order changes. Empty on a Vulkan 1.0 loader.
    std::string uuid;
};

// Read once per process, since each read loads every driver. Empty when there's
// no Vulkan.
std::vector<GpuInfo> enumerate_gpus();

inline constexpr uint32_t kVendorNvidia = 0x10de;
// Shown on the About cube's tenth click and the welcome page, on NVIDIA.
inline constexpr const char* kNvidiaNote =
    "This GPU model may experience stability due to a decent Vulkan memory allocator in its driver. "
    "It's recommended you use Vulkan if you experience stability.";

// Enumeration order isn't a ranking: hybrid laptops often list the integrated GPU
// first. Prefers a discrete GPU, else 0.
uint32_t default_gpu_index();

// Drivers or external GPUs can reorder the list, so the UUID decides and the index
// is a fallback. Nothing saved means default_gpu_index().
uint32_t chosen_gpu_index(const stud::config::GpuSelection& selection);

}  // namespace stud::ui
