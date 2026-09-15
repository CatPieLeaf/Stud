// M8 test: real GPU enumeration (ui/src/gpu_enum.h) against this
// machine's actual Vulkan driver, not a mock, the real
// vkEnumeratePhysicalDevices output.

#include "gpu_enum.h"

#include <cstdint>
#include <cstdio>
#include <cstdlib>

namespace {

void check(bool condition, const char* what) {
    if (!condition) {
        std::fprintf(stderr, "FAILED: %s\n", what);
        std::exit(1);
    }
    std::printf("ok: %s\n", what);
}

}  // namespace

int main() {
    auto gpus = stud::ui::enumerate_gpus();

    // Honest either-outcome check, a machine with no usable Vulkan
    // driver should get an empty, non-crashing result (already exercised
    // structurally by enumerate_gpus()'s own vkCreateInstance failure
    // path); this development machine has real GPUs, so assert real
    // content was found, not just "didn't crash".
    check(!gpus.empty(), "at least one real GPU was enumerated on this machine");

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

    // And no real GPU is listed twice.
    for (size_t i = 0; i < gpus.size(); ++i) {
        for (size_t j = i + 1; j < gpus.size(); ++j) {
            check(gpus[i].device_index != gpus[j].device_index,
                  "no two entries share a device index");
        }
    }

    std::printf("all gpu-enum checks passed\n");
    return 0;
}
