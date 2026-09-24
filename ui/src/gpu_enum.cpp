#include "gpu_enum.h"

#include <vulkan/vulkan.h>

#include <algorithm>
#include <array>
#include <cstring>
#include <string>

namespace stud::ui {

std::vector<GpuInfo> enumerate_gpus() {
    std::vector<GpuInfo> result;

    VkApplicationInfo app_info{};
    app_info.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
    app_info.pApplicationName = "Stud Settings";
    app_info.applicationVersion = VK_MAKE_VERSION(1, 0, 0);
    app_info.pEngineName = "Stud";
    // 1.1 for VkPhysicalDeviceIDProperties, which is what tells one real
    // GPU apart from the same GPU enumerated twice (see below). A loader
    // too old for it still works: the request drops back to 1.0 and the
    // duplicate check falls back to the device's own identifiers.
    app_info.apiVersion = VK_API_VERSION_1_1;

    VkInstanceCreateInfo instance_info{};
    instance_info.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
    instance_info.pApplicationInfo = &app_info;

    bool have_device_uuid = true;
    VkInstance instance = VK_NULL_HANDLE;
    if (vkCreateInstance(&instance_info, nullptr, &instance) != VK_SUCCESS) {
        have_device_uuid = false;
        app_info.apiVersion = VK_API_VERSION_1_0;
        if (vkCreateInstance(&instance_info, nullptr, &instance) != VK_SUCCESS) {
            // Honest empty result. No usable Vulkan loader/driver on this
            // host. The settings window shows "no GPUs found" rather than
            // crashing.
            return result;
        }
    }

    uint32_t device_count = 0;
    vkEnumeratePhysicalDevices(instance, &device_count, nullptr);
    if (device_count > 0) {
        std::vector<VkPhysicalDevice> devices(device_count);
        vkEnumeratePhysicalDevices(instance, &device_count, devices.data());

        // One entry per real GPU, even when the loader offers it more than
        // once.
        //
        // A Flatpak does exactly that: its runtime carries one GL
        // extension directory per driver (GL/default, GL/nvidia-<ver>)
        // and reaches them through several entries of XDG_DATA_DIRS as
        // well as /usr/share/vulkan/icd.d, so every ICD is read twice and
        // every device is enumerated twice, the GPU list showed each
        // card, the integrated one and llvmpipe in pairs.
        //
        // The identity is the device UUID, not the name: a machine with
        // two identical cards must still list both, and those differ only
        // by UUID. Where that is unavailable, the vendor/device/driver
        // triple plus the name is the closest honest substitute.
        std::vector<std::array<uint8_t, VK_UUID_SIZE>> seen_uuids;
        std::vector<std::string> seen_keys;
        for (uint32_t i = 0; i < device_count; ++i) {
            VkPhysicalDeviceProperties props{};
            vkGetPhysicalDeviceProperties(devices[i], &props);

            bool duplicate = false;
            if (have_device_uuid) {
                VkPhysicalDeviceIDProperties id_props{};
                id_props.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ID_PROPERTIES;
                VkPhysicalDeviceProperties2 props2{};
                props2.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2;
                props2.pNext = &id_props;
                vkGetPhysicalDeviceProperties2(devices[i], &props2);
                std::array<uint8_t, VK_UUID_SIZE> uuid{};
                std::memcpy(uuid.data(), id_props.deviceUUID, VK_UUID_SIZE);
                duplicate = std::find(seen_uuids.begin(), seen_uuids.end(), uuid) !=
                            seen_uuids.end();
                if (!duplicate) seen_uuids.push_back(uuid);
            } else {
                std::string key = std::string(props.deviceName) + "|" +
                                  std::to_string(props.vendorID) + "|" +
                                  std::to_string(props.deviceID) + "|" +
                                  std::to_string(props.driverVersion);
                duplicate = std::find(seen_keys.begin(), seen_keys.end(), key) != seen_keys.end();
                if (!duplicate) seen_keys.push_back(key);
            }
            if (duplicate) continue;

            // The index is this device's position in the RAW enumeration,
            // duplicates included, and has to stay that way: it is what
            // Settings stores and what render-host uses to pick a device
            // out of its own vkEnumeratePhysicalDevices (see
            // vk_set_preferred_device_index). Renumbering here would point
            // a saved choice at a different card.
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
// tiled image, the same question the render client asks before deciding
// to transcode into it (see runtime/render-client/src/vulkan_client.cpp),
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

uint32_t default_gpu_index() {
    const auto gpus = enumerate_gpus();
    for (const auto& gpu : gpus) {
        if (gpu.discrete) return gpu.device_index;
    }
    return 0;
}

}  // namespace stud::ui
