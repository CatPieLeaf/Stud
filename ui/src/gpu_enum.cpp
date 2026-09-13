#include "gpu_enum.h"

#include <vulkan/vulkan.h>

namespace stud::ui {

std::vector<GpuInfo> enumerate_gpus() {
    std::vector<GpuInfo> result;

    VkApplicationInfo app_info{};
    app_info.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
    app_info.pApplicationName = "Stud Settings";
    app_info.applicationVersion = VK_MAKE_VERSION(1, 0, 0);
    app_info.pEngineName = "Stud";
    app_info.apiVersion = VK_API_VERSION_1_0;

    VkInstanceCreateInfo instance_info{};
    instance_info.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
    instance_info.pApplicationInfo = &app_info;

    VkInstance instance = VK_NULL_HANDLE;
    if (vkCreateInstance(&instance_info, nullptr, &instance) != VK_SUCCESS) {
        // Honest empty result -- no usable Vulkan loader/driver on this
        // host. The settings window shows "no GPUs found" rather than
        // crashing.
        return result;
    }

    uint32_t device_count = 0;
    vkEnumeratePhysicalDevices(instance, &device_count, nullptr);
    if (device_count > 0) {
        std::vector<VkPhysicalDevice> devices(device_count);
        vkEnumeratePhysicalDevices(instance, &device_count, devices.data());

        for (uint32_t i = 0; i < device_count; ++i) {
            VkPhysicalDeviceProperties props{};
            vkGetPhysicalDeviceProperties(devices[i], &props);
            result.push_back(GpuInfo{i, std::string(props.deviceName)});
        }
    }

    vkDestroyInstance(instance, nullptr);
    return result;
}

}  // namespace stud::ui
