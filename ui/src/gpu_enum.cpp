#include "gpu_enum.h"

// The loader is opened at run time, so Settings works without Vulkan and lists no GPUs.
#define VK_NO_PROTOTYPES
#include <vulkan/vulkan.h>

#include <dlfcn.h>

#include <algorithm>
#include <array>
#include <cstdio>
#include <cstring>
#include <fstream>
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

// The model from the PCI ID database, its [marketing name] when it has one; empty
// without a database. Flatpak sees the host's under /run/host.
std::string pci_model(uint32_t vendor, uint32_t device) {
    for (const char* path : {"/usr/share/hwdata/pci.ids", "/usr/share/misc/pci.ids", "/usr/share/pci.ids",
                             "/run/host/usr/share/hwdata/pci.ids"}) {
        std::ifstream file(path);
        if (!file) continue;
        char vendor_hex[5], device_hex[6];
        std::snprintf(vendor_hex, sizeof vendor_hex, "%04x", vendor);
        std::snprintf(device_hex, sizeof device_hex, "\t%04x", device);
        bool in_vendor = false;
        for (std::string line; std::getline(file, line);) {
            if (line.empty() || line[0] == '#') continue;
            if (line[0] != '\t') {
                if (in_vendor) break;
                in_vendor = line.rfind(vendor_hex, 0) == 0;
            } else if (in_vendor && line.rfind(device_hex, 0) == 0) {
                std::string model = line.substr(7);
                const size_t open = model.find('['), close = model.rfind(']');
                if (open != std::string::npos && close != std::string::npos && open < close) {
                    model = model.substr(open + 1, close - open - 1);
                }
                return model;
            }
        }
        return {};
    }
    return {};
}

// Without Vulkan (OpenGL only), the kernel's GPUs: the one that didn't boot the
// display is the offload GPU on a hybrid laptop.
std::vector<GpuInfo> read_drm_gpus() {
    std::vector<GpuInfo> result;
    const auto read = [](const std::string& path) {
        std::ifstream file(path);
        std::string value;
        file >> value;
        return value;
    };
    for (uint32_t card = 0;; ++card) {
        const std::string device = "/sys/class/drm/card" + std::to_string(card) + "/device/";
        const std::string vendor = read(device + "vendor");
        if (vendor.empty()) break;
        const uint32_t vendor_id = std::stoul(vendor, nullptr, 16);
        const char* maker = vendor_id == kVendorNvidia ? "NVIDIA" : vendor_id == 0x1002 ? "AMD"
                          : vendor_id == 0x8086 ? "Intel" : "GPU";
        const uint32_t device_id = std::stoul(read(device + "device"), nullptr, 16);
        std::string model = pci_model(vendor_id, device_id);
        if (model.empty()) {
            char id[8];
            std::snprintf(id, sizeof id, "%04x", device_id);
            model = std::string("GPU ") + id;
        }
        result.push_back(GpuInfo{card, std::string(maker) + " " + model, vendor_id,
                                 read(device + "boot_vga") == "0", std::string()});
    }
    return result;
}

}  // namespace

std::vector<GpuInfo> enumerate_gpus() {
    static const std::vector<GpuInfo> gpus = [] {
        std::vector<GpuInfo> found = read_gpus();
        return found.empty() ? read_drm_gpus() : found;
    }();
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
