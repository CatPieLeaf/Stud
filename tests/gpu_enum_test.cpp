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

    // device_index values are exactly 0..N-1, matching
    // vkEnumeratePhysicalDevices()'s own result order (the settings
    // schema's real, stable identifier; see stud-config/settings.h).
    for (size_t i = 0; i < gpus.size(); ++i) {
        check(gpus[i].device_index == static_cast<uint32_t>(i),
              "device_index matches vkEnumeratePhysicalDevices()'s own result order");
    }

    std::printf("all gpu-enum checks passed\n");
    return 0;
}
