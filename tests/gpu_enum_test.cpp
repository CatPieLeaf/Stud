// Real GPU enumeration (ui/src/gpu_enum.h) against this machine's Vulkan driver.
// Skipped on a machine with none.

#include "gpu_enum.h"
#include "check.h"

#include <cstdint>
#include <cstdio>
#include <cstdlib>

int main() {
    auto gpus = stud::ui::enumerate_gpus();

    // Reaching this line with an empty list is the no-driver path working.
    if (gpus.empty()) {
        std::printf("no Vulkan device on this machine\n");
        return kSkipped;
    }

    for (const auto& gpu : gpus) {
        check(!gpu.name.empty(), "enumerated GPU has a real, non-empty device name");
        std::printf("  found GPU %u: %s\n", gpu.device_index, gpu.name.c_str());
    }

    // device_index is a position in vkEnumeratePhysicalDevices()'s own
    // result order (the settings schema's real, stable identifier; see
    // stud-config/settings.h), so the values rise but are NOT required to
    // be 0..N-1: a loader that offers the same device twice, which a
    // Flatpak's runtime really does, through two ICD directories, has
    // its duplicates dropped, and the survivors keep their raw indices.
    for (size_t i = 0; i < gpus.size(); ++i) {
        check(gpus[i].device_index >= static_cast<uint32_t>(i),
              "device_index is a raw vkEnumeratePhysicalDevices() position");
        if (i > 0) {
            check(gpus[i].device_index > gpus[i - 1].device_index,
                  "device_index values rise in enumeration order");
        }
    }

    // And no real GPU is listed twice. Only the UUID can say so: a duplicate
    // from a second ICD directory has an index of its own.
    for (size_t i = 0; i < gpus.size(); ++i) {
        for (size_t j = i + 1; j < gpus.size(); ++j) {
            check(gpus[i].uuid.empty() || gpus[i].uuid != gpus[j].uuid,
                  "no two entries are the same device");
        }
    }

    // The saved choice: the UUID decides, and the index stands in without one.
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
    gone.device_uuid = "not a real uuid";
    gone.device_index = 42;
    check(chosen_gpu_index(gone) == 42, "a UUID nothing has falls back to the saved index");

    std::printf("all gpu-enum checks passed\n");
    return 0;
}
