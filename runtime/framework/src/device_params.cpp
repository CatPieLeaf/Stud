#include "stud/device_params.h"

#include <climits>
#include <fstream>
#include <sstream>
#include <sys/stat.h>

namespace stud::jni_bridge {

BEGIN_NATIVE_DESCRIPTOR(DeviceParams)
{ FakeJni::Field<&DeviceParams::osVersion>{}, "osVersion" },
{ FakeJni::Field<&DeviceParams::deviceName>{}, "deviceName" },
{ FakeJni::Field<&DeviceParams::appVersion>{}, "appVersion" },
{ FakeJni::Field<&DeviceParams::country>{}, "country" },
{ FakeJni::Field<&DeviceParams::manufacturer>{}, "manufacturer" },
{ FakeJni::Field<&DeviceParams::deviceTotalMemoryMB>{}, "deviceTotalMemoryMB" },
{ FakeJni::Field<&DeviceParams::displayResolution>{}, "displayResolution" },
{ FakeJni::Field<&DeviceParams::displayPhysicalWidthPixels>{}, "displayPhysicalWidthPixels" },
{ FakeJni::Field<&DeviceParams::displayPhysicalHeightPixels>{}, "displayPhysicalHeightPixels" },
{ FakeJni::Field<&DeviceParams::networkType>{}, "networkType" },
{ FakeJni::Field<&DeviceParams::isChrome>{}, "isChrome" },
{ FakeJni::Field<&DeviceParams::appBuildVariant>{}, "appBuildVariant" },
{ FakeJni::Field<&DeviceParams::cpu64Bit>{}, "cpu64Bit" },
{ FakeJni::Field<&DeviceParams::memoryClass>{}, "memoryClass" },
{ FakeJni::Field<&DeviceParams::largeMemoryClass>{}, "largeMemoryClass" },
{ FakeJni::Field<&DeviceParams::isLowRamDevice>{}, "isLowRamDevice" },
{ FakeJni::Field<&DeviceParams::lowMemoryKillerBackgroundAppThreshold>{}, "lowMemoryKillerBackgroundAppThreshold" },
{ FakeJni::Field<&DeviceParams::lowMemoryKillerForegroundAppThreshold>{}, "lowMemoryKillerForegroundAppThreshold" },
{ FakeJni::Field<&DeviceParams::deviceSku>{}, "deviceSku" },
{ FakeJni::Field<&DeviceParams::socModel>{}, "socModel" },
{ FakeJni::Field<&DeviceParams::testDeviceName>{}, "testDeviceName" },
END_NATIVE_DESCRIPTOR

namespace {

// What the app's own network-type helper (rh.a2.g in the app) reports, for
// the connection this machine is actually using.
//
// Its answers are "" with no active network, "WiFi" for Wi-Fi, a mobile
// generation ("2G".."5G") for cellular, and "Other" for anything else --
// Ethernet included. This used to be "WIFI", a spelling the app never
// produces, sent whatever the machine was on.
//
// The active network is the one carrying the default route: the lowest
// metric 0.0.0.0/0 entry in /proc/net/route (the sandbox's /proc shows
// the host's network namespace unless Stud runs offline). Wi-Fi is the
// kernel's own answer, a `wireless` directory under the interface in the
// real /sys the sandbox binds read-only. There is no cellular modem to
// ask about on a desktop, so a mobile generation is never claimed.
std::string active_network_type() {
    std::ifstream routes("/proc/net/route");
    std::string line;
    std::getline(routes, line);  // header
    std::string best_iface;
    long best_metric = LONG_MAX;
    while (std::getline(routes, line)) {
        std::istringstream fields(line);
        std::string iface, destination, gateway, flags, refcnt, use;
        long metric = 0;
        if (!(fields >> iface >> destination >> gateway >> flags >> refcnt >> use >> metric)) {
            continue;
        }
        // RTF_UP is 0x1; a route that is not up carries nothing.
        const unsigned long route_flags = std::stoul(flags, nullptr, 16);
        if (destination != "00000000" || (route_flags & 0x1u) == 0) continue;
        if (metric < best_metric) {
            best_metric = metric;
            best_iface = iface;
        }
    }
    if (best_iface.empty()) return "";
    struct stat st {};
    const std::string wireless = "/sys/class/net/" + best_iface + "/wireless";
    return ::stat(wireless.c_str(), &st) == 0 ? "WiFi" : "Other";
}

}  // namespace

std::shared_ptr<DeviceParams> build_desktop_device_params(const std::string& os_version,
                                                            const std::string& device_name,
                                                            const std::string& app_version,
                                                            const std::string& display_resolution,
                                                            int display_width_px, int display_height_px,
                                                            int total_memory_mb) {
    auto params = std::make_shared<DeviceParams>();
    params->osVersion = std::make_shared<FakeJni::JString>(os_version);
    params->deviceName = std::make_shared<FakeJni::JString>(device_name);
    params->appVersion = std::make_shared<FakeJni::JString>(app_version);
    params->country = std::make_shared<FakeJni::JString>("");
    params->manufacturer = std::make_shared<FakeJni::JString>("Stud");
    params->deviceTotalMemoryMB = total_memory_mb;
    params->displayResolution = std::make_shared<FakeJni::JString>(display_resolution);
    params->displayPhysicalWidthPixels = display_width_px;
    params->displayPhysicalHeightPixels = display_height_px;
    params->networkType = std::make_shared<FakeJni::JString>(active_network_type());
    // Matches the ChromeOS the User-Agent already reports. On a device
    // this comes from hasSystemFeature("org.chromium.arc.device_management")
    // the same real ARC check, and it was left false here, so the
    // agent and the params described different machines.
    params->isChrome = true;
    params->appBuildVariant = std::make_shared<FakeJni::JString>("release");
    params->cpu64Bit = true;
    params->memoryClass = 512;
    params->largeMemoryClass = 512;
    params->isLowRamDevice = false;
    params->lowMemoryKillerBackgroundAppThreshold = 0;
    params->lowMemoryKillerForegroundAppThreshold = 0;
    params->deviceSku = std::make_shared<FakeJni::JString>("unknown");
    params->socModel = std::make_shared<FakeJni::JString>("unknown");
    return params;
}

BEGIN_NATIVE_DESCRIPTOR(DeviceStaticParamsJava)
{ FakeJni::Field<&DeviceStaticParamsJava::appBuildVariant>{}, "appBuildVariant" },
{ FakeJni::Field<&DeviceStaticParamsJava::appVersion>{}, "appVersion" },
{ FakeJni::Field<&DeviceStaticParamsJava::cpu64Bit>{}, "cpu64Bit" },
{ FakeJni::Field<&DeviceStaticParamsJava::deviceName>{}, "deviceName" },
{ FakeJni::Field<&DeviceStaticParamsJava::deviceSku>{}, "deviceSku" },
{ FakeJni::Field<&DeviceStaticParamsJava::manufacturer>{}, "manufacturer" },
{ FakeJni::Field<&DeviceStaticParamsJava::osVersion>{}, "osVersion" },
{ FakeJni::Field<&DeviceStaticParamsJava::socModel>{}, "socModel" },
END_NATIVE_DESCRIPTOR

std::shared_ptr<DeviceStaticParamsJava> build_desktop_device_static_params(
    const std::shared_ptr<DeviceParams>& device_params) {
    auto params = std::make_shared<DeviceStaticParamsJava>();
    params->appBuildVariant = device_params->appBuildVariant;
    params->appVersion = device_params->appVersion;
    params->cpu64Bit = device_params->cpu64Bit;
    params->deviceName = device_params->deviceName;
    params->deviceSku = device_params->deviceSku;
    params->manufacturer = device_params->manufacturer;
    params->osVersion = device_params->osVersion;
    params->socModel = device_params->socModel;
    return params;
}

}  // namespace stud::jni_bridge
