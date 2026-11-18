// GPU enumeration from the kernel and the PCI ID database; skipped without a GPU.

#include "gpu_enum.h"
#include "check.h"

#include <cstdint>
#include <cstdio>
#include <cstdlib>

int main() {
    auto gpus = stud::ui::enumerate_gpus();

    if (gpus.empty()) {
        std::printf("no GPU on this machine\n");
        return kSkipped;
    }

    for (const auto& gpu : gpus) {
        check(!gpu.name.empty(), "enumerated GPU has a real, non-empty device name");
        std::printf("  found GPU %u: %s\n", gpu.device_index, gpu.name.c_str());
    }

    // Raw enumeration indices: rising but with gaps where duplicates (Flatpak's
    // second ICD directory) were dropped.
    for (size_t i = 0; i < gpus.size(); ++i) {
        check(gpus[i].device_index >= static_cast<uint32_t>(i),
              "device_index is the DRM card number");
        if (i > 0) {
            check(gpus[i].device_index > gpus[i - 1].device_index,
                  "device_index values rise in enumeration order");
        }
    }

    // No GPU twice, by UUID.
    for (size_t i = 0; i < gpus.size(); ++i) {
        for (size_t j = i + 1; j < gpus.size(); ++j) {
            check(gpus[i].uuid.empty() || gpus[i].uuid != gpus[j].uuid,
                  "no two entries are the same device");
        }
    }

    // The UUID decides; the index stands in without one.
    using stud::config::GpuSelection;
    using stud::ui::chosen_gpu_index;
    const auto& last = gpus.back();
    check(chosen_gpu_index(GpuSelection{}) == stud::ui::default_gpu_index(),
          "nothing chosen yet gets the default device");
    if (!last.uuid.empty()) {
        GpuSelection moved;
        moved.device_name = last.name;
        moved.device_uuid = last.uuid;
        moved.device_index = last.device_index + 1000;
        check(chosen_gpu_index(moved) == last.device_index,
              "a device that moved is found by its UUID, not the stale index");
    }
    GpuSelection gone;
    gone.device_name = "A card that was removed";
    gone.device_uuid = "pci-0000:99:00.0";
    gone.device_index = 42;
    check(chosen_gpu_index(gone) == 42, "an address nothing has falls back to the saved index");
    GpuSelection old_vulkan = gone;
    old_vulkan.device_uuid = "0123456789abcdef0123456789abcdef";
    check(chosen_gpu_index(old_vulkan) == stud::ui::default_gpu_index(),
          "a choice from the old Vulkan list goes to the default GPU");

    std::printf("all gpu-enum checks passed\n");
    return 0;
}
