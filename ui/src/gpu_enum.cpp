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
            result.push_back(GpuInfo{i, std::string(props.deviceName), props.vendorID,
                                     props.deviceType ==
                                         VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU});
        }
    }

    vkDestroyInstance(instance, nullptr);
    return result;
}

namespace {

// A format is usable for a texture if it can be sampled from an optimal-
// tiled image -- the same question the render client asks before deciding
// to transcode into it (see runtime/render-client/src/vulkan_stub.cpp),
// so the two cannot disagree about what this machine can do.
bool sampleable(VkPhysicalDevice device, VkFormat format) {
    VkFormatProperties props{};
    vkGetPhysicalDeviceFormatProperties(device, format, &props);
    return (props.optimalTilingFeatures & VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT) != 0;
}

}  // namespace

TextureFormatSupport query_texture_formats(uint32_t device_index) {
    TextureFormatSupport support;

    VkApplicationInfo app_info{};
    app_info.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
    app_info.pApplicationName = "Stud Settings";
    app_info.apiVersion = VK_API_VERSION_1_0;

    VkInstanceCreateInfo instance_info{};
    instance_info.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
    instance_info.pApplicationInfo = &app_info;

    VkInstance instance = VK_NULL_HANDLE;
    if (vkCreateInstance(&instance_info, nullptr, &instance) != VK_SUCCESS) return support;

    uint32_t device_count = 0;
    vkEnumeratePhysicalDevices(instance, &device_count, nullptr);
    if (device_count > device_index) {
        std::vector<VkPhysicalDevice> devices(device_count);
        vkEnumeratePhysicalDevices(instance, &device_count, devices.data());
        VkPhysicalDevice device = devices[device_index];
        support.queried = true;
        support.etc2 = sampleable(device, VK_FORMAT_ETC2_R8G8B8_UNORM_BLOCK) &&
                       sampleable(device, VK_FORMAT_ETC2_R8G8B8A8_UNORM_BLOCK);
        support.eac = sampleable(device, VK_FORMAT_EAC_R11_UNORM_BLOCK) &&
                      sampleable(device, VK_FORMAT_EAC_R11G11_UNORM_BLOCK);
        support.astc_ldr = sampleable(device, VK_FORMAT_ASTC_4x4_UNORM_BLOCK);
        support.pvrtc = sampleable(device, VK_FORMAT_PVRTC1_4BPP_UNORM_BLOCK_IMG);
        support.bc1 = sampleable(device, VK_FORMAT_BC1_RGBA_UNORM_BLOCK);
        support.bc3 = sampleable(device, VK_FORMAT_BC3_UNORM_BLOCK);
        support.bc4_bc5 = sampleable(device, VK_FORMAT_BC4_UNORM_BLOCK) &&
                          sampleable(device, VK_FORMAT_BC5_UNORM_BLOCK);
        support.bc7 = sampleable(device, VK_FORMAT_BC7_UNORM_BLOCK);
    }

    vkDestroyInstance(instance, nullptr);
    return support;
}

}  // namespace stud::ui
