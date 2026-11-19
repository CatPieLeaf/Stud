#include "gpu_enum.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>

namespace stud::ui {

namespace {

std::string read_word(const std::string& path) {
    std::ifstream file(path);
    std::string value;
    file >> value;
    return value;
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

// The kernel's GPUs. The one that didn't boot the display is the offload GPU on a
// hybrid laptop; the PCI address identifies a card when card numbers change.
std::vector<GpuInfo> read_gpus() {
    std::vector<GpuInfo> result;
    // Every cardN, not counting from 0: a card0 handed over by the boot framebuffer
    // leaves the GPUs at card1 and up.
    std::vector<uint32_t> cards;
    std::error_code listed;
    for (const auto& entry : std::filesystem::directory_iterator("/sys/class/drm", listed)) {
        const std::string name = entry.path().filename().string();
        if (name.size() > 4 && name.rfind("card", 0) == 0 &&
            name.find_first_not_of("0123456789", 4) == std::string::npos) {
            cards.push_back(std::stoul(name.substr(4)));
        }
    }
    std::sort(cards.begin(), cards.end());
    for (const uint32_t card : cards) {
        const std::string device = "/sys/class/drm/card" + std::to_string(card) + "/device";
        const std::string vendor = read_word(device + "/vendor");
        if (vendor.empty()) continue;
        const uint32_t vendor_id = std::stoul(vendor, nullptr, 16);
        const char* maker = vendor_id == kVendorNvidia ? "NVIDIA" : vendor_id == 0x1002 ? "AMD"
                          : vendor_id == 0x8086 ? "Intel" : "GPU";
        const uint32_t device_id = std::stoul(read_word(device + "/device"), nullptr, 16);
        std::string model = pci_model(vendor_id, device_id);
        if (model.empty()) {
            char id[8];
            std::snprintf(id, sizeof id, "%04x", device_id);
            model = std::string("GPU ") + id;
        }
        std::error_code error;
        const std::string slot = std::filesystem::canonical(device, error).filename().string();
        result.push_back(GpuInfo{card, std::string(maker) + " " + model, vendor_id,
                                 read_word(device + "/boot_vga") == "0", "pci-" + slot});
    }
    return result;
}

}  // namespace

std::vector<GpuInfo> enumerate_gpus() {
    static const std::vector<GpuInfo> gpus = read_gpus();
    return gpus;
}

bool vulkan_available() {
    if (std::getenv("VK_DRIVER_FILES") != nullptr || std::getenv("VK_ICD_FILENAMES") != nullptr) return true;
    std::vector<std::string> dirs = {"/etc/vulkan/icd.d", "/usr/share/vulkan/icd.d", "/run/host/usr/share/vulkan/icd.d"};
    // Flatpak's GL extensions add theirs through XDG_DATA_DIRS.
    if (const char* data = std::getenv("XDG_DATA_DIRS")) {
        std::string list = data;
        for (size_t at = 0; at <= list.size();) {
            const size_t end = std::min(list.find(':', at), list.size());
            if (end > at) dirs.push_back(list.substr(at, end - at) + "/vulkan/icd.d");
            at = end + 1;
        }
    }
    for (const std::string& dir : dirs) {
        std::error_code error;
        for (const auto& entry : std::filesystem::directory_iterator(dir, error)) {
            if (entry.path().extension() == ".json") return true;
        }
    }
    return false;
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
        // Saved from the Vulkan list, whose order differs from the kernel's.
        if (selection.device_uuid.rfind("pci-", 0) != 0) return default_gpu_index();
    }
    // A number no GPU has (a removed card, or a choice saved while the list was empty).
    for (const auto& gpu : enumerate_gpus()) {
        if (gpu.device_index == selection.device_index) return gpu.device_index;
    }
    return default_gpu_index();
}

}  // namespace stud::ui
