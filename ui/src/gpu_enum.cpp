#include "gpu_enum.h"

// volk opens the loader at run time, so Settings works without Vulkan and lists no GPUs.
#include "volk.h"

#include <algorithm>
#include <array>
#include <cstring>
#include <string>

namespace stud::ui {

namespace {

bool loader_available() {
    static const bool available = volkInitialize() == VK_SUCCESS;
    return available;
}

// Per instance, so two enumerations never share function pointers.
VolkInstanceTable instance_table(VkInstance instance) {
    VolkInstanceTable table{};
    volkLoadInstanceTable(&table, instance);
    return table;
}

std::vector<GpuInfo> read_gpus() {
    std::vector<GpuInfo> result;

    VkApplicationInfo app_info{};
    app_info.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
    app_info.pApplicationName = "Stud Settings";
    app_info.applicationVersion = VK_MAKE_VERSION(1, 0, 0);
    app_info.pEngineName = "Stud";
    // 1.1 for VkPhysicalDeviceIDProperties, to drop duplicates; older loaders fall
    // back to 1.0 and the device's other identifiers.
    app_info.apiVersion = VK_API_VERSION_1_1;

    VkInstanceCreateInfo instance_info{};
    instance_info.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
    instance_info.pApplicationInfo = &app_info;

    if (!loader_available()) return result;

    bool have_device_uuid = true;
    VkInstance instance = VK_NULL_HANDLE;
    if (vkCreateInstance(&instance_info, nullptr, &instance) != VK_SUCCESS) {
        have_device_uuid = false;
        app_info.apiVersion = VK_API_VERSION_1_0;
        if (vkCreateInstance(&instance_info, nullptr, &instance) != VK_SUCCESS) {
            return result;
        }
    }
    const VolkInstanceTable vk = instance_table(instance);

    uint32_t device_count = 0;
    vk.vkEnumeratePhysicalDevices(instance, &device_count, nullptr);
    if (device_count > 0) {
        std::vector<VkPhysicalDevice> devices(device_count);
        vk.vkEnumeratePhysicalDevices(instance, &device_count, devices.data());

        // One entry per GPU: Flatpak's runtime reads each ICD twice, which listed
        // every device in pairs. Keyed by UUID, since two identical cards differ
        // only there; vendor, device, driver and name otherwise.
        std::vector<std::array<uint8_t, VK_UUID_SIZE>> seen_uuids;
        std::vector<std::string> seen_keys;
        for (uint32_t i = 0; i < device_count; ++i) {
            VkPhysicalDeviceProperties props{};
            vk.vkGetPhysicalDeviceProperties(devices[i], &props);

            bool duplicate = false;
            std::string uuid_hex;
            if (have_device_uuid) {
                VkPhysicalDeviceIDProperties id_props{};
                id_props.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ID_PROPERTIES;
                VkPhysicalDeviceProperties2 props2{};
                props2.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2;
                props2.pNext = &id_props;
                vk.vkGetPhysicalDeviceProperties2(devices[i], &props2);
                std::array<uint8_t, VK_UUID_SIZE> uuid{};
                std::memcpy(uuid.data(), id_props.deviceUUID, VK_UUID_SIZE);
                duplicate = std::find(seen_uuids.begin(), seen_uuids.end(), uuid) !=
                            seen_uuids.end();
                if (!duplicate) seen_uuids.push_back(uuid);
                static constexpr char kHex[] = "0123456789abcdef";
                for (const uint8_t byte : uuid) {
                    uuid_hex += kHex[byte >> 4];
                    uuid_hex += kHex[byte & 0xf];
                }
            } else {
                std::string key = std::string(props.deviceName) + "|" +
                                  std::to_string(props.vendorID) + "|" +
                                  std::to_string(props.deviceID) + "|" +
                                  std::to_string(props.driverVersion);
                duplicate = std::find(seen_keys.begin(), seen_keys.end(), key) != seen_keys.end();
                if (!duplicate) seen_keys.push_back(key);
            }
            if (duplicate) continue;

            // The raw enumeration index, duplicates included: Settings stores it
            // and apply_gpu_selection looks it up.
            result.push_back(GpuInfo{i, std::string(props.deviceName), props.vendorID,
                                     props.deviceType == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU,
                                     uuid_hex});
        }
    }

    vk.vkDestroyInstance(instance, nullptr);
    return result;
}

}  // namespace

std::vector<GpuInfo> enumerate_gpus() {
    static const std::vector<GpuInfo> gpus = read_gpus();
    return gpus;
}

uint32_t default_gpu_index() {
    const auto gpus = enumerate_gpus();
    for (const auto& gpu : gpus) {
        if (gpu.discrete) return gpu.device_index;
    }
    return 0;
}

uint32_t chosen_gpu_index(const stud::config::GpuSelection& selection) {
    if (selection.device_name.empty()) return default_gpu_index();
    if (!selection.device_uuid.empty()) {
        for (const auto& gpu : enumerate_gpus()) {
            if (gpu.uuid == selection.device_uuid) return gpu.device_index;
        }
    }
    return selection.device_index;
}

}  // namespace stud::ui
