#include "gpu_enum.h"

// The loader is opened at run time, so Settings works without Vulkan and lists no GPUs.
#define VK_NO_PROTOTYPES
#include <vulkan/vulkan.h>

#include <dlfcn.h>

#include <algorithm>
#include <array>
#include <cstring>
#include <string>

namespace stud::ui {

namespace {

PFN_vkGetInstanceProcAddr loader() {
    static const auto get = [] {
        void* lib = dlopen("libvulkan.so.1", RTLD_NOW | RTLD_LOCAL);
        if (lib == nullptr) lib = dlopen("libvulkan.so", RTLD_NOW | RTLD_LOCAL);
        return lib == nullptr ? nullptr
                              : reinterpret_cast<PFN_vkGetInstanceProcAddr>(dlsym(lib, "vkGetInstanceProcAddr"));
    }();
    return get;
}

template <typename Fn>
Fn load(VkInstance instance, const char* name) {
    return reinterpret_cast<Fn>(loader()(instance, name));
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

    if (loader() == nullptr) return result;
    const auto vkCreateInstance = load<PFN_vkCreateInstance>(VK_NULL_HANDLE, "vkCreateInstance");
    if (vkCreateInstance == nullptr) return result;

    bool have_device_uuid = true;
    VkInstance instance = VK_NULL_HANDLE;
    if (vkCreateInstance(&instance_info, nullptr, &instance) != VK_SUCCESS) {
        have_device_uuid = false;
        app_info.apiVersion = VK_API_VERSION_1_0;
        if (vkCreateInstance(&instance_info, nullptr, &instance) != VK_SUCCESS) {
            return result;
        }
    }
    const auto vkEnumeratePhysicalDevices =
        load<PFN_vkEnumeratePhysicalDevices>(instance, "vkEnumeratePhysicalDevices");
    const auto vkGetPhysicalDeviceProperties =
        load<PFN_vkGetPhysicalDeviceProperties>(instance, "vkGetPhysicalDeviceProperties");
    const auto vkGetPhysicalDeviceProperties2 =
        load<PFN_vkGetPhysicalDeviceProperties2>(instance, "vkGetPhysicalDeviceProperties2");
    const auto vkDestroyInstance = load<PFN_vkDestroyInstance>(instance, "vkDestroyInstance");
    if (vkGetPhysicalDeviceProperties2 == nullptr) have_device_uuid = false;

    uint32_t device_count = 0;
    vkEnumeratePhysicalDevices(instance, &device_count, nullptr);
    if (device_count > 0) {
        std::vector<VkPhysicalDevice> devices(device_count);
        vkEnumeratePhysicalDevices(instance, &device_count, devices.data());

        // One entry per GPU: Flatpak's runtime reads each ICD twice, which listed
        // every device in pairs. Keyed by UUID, since two identical cards differ
        // only there; vendor, device, driver and name otherwise.
        std::vector<std::array<uint8_t, VK_UUID_SIZE>> seen_uuids;
        std::vector<std::string> seen_keys;
        for (uint32_t i = 0; i < device_count; ++i) {
            VkPhysicalDeviceProperties props{};
            vkGetPhysicalDeviceProperties(devices[i], &props);

            bool duplicate = false;
            std::string uuid_hex;
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

    vkDestroyInstance(instance, nullptr);
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
