#include "stud/init_params.h"

namespace stud::jni_bridge {

// Real, version-agnostic fix (the engineering notes): registers InitParams's
// properties as real METHODS, matching AutoValue's actual zero-arg
// getter convention (confirmed directly against the real libroblox.so,
// not guessed); see init_params.h's doc comment for the full trace.
BEGIN_NATIVE_DESCRIPTOR(InitParams)
{ FakeJni::Function<&InitParams::platformParams>{}, "platformParams" },
{ FakeJni::Function<&InitParams::deviceParams>{}, "deviceParams" },
{ FakeJni::Function<&InitParams::baseURL>{}, "baseURL" },
{ FakeJni::Function<&InitParams::userAgent>{}, "userAgent" },
{ FakeJni::Function<&InitParams::isTablet>{}, "isTablet" },
{ FakeJni::Function<&InitParams::isPotato>{}, "isPotato" },
{ FakeJni::Function<&InitParams::isVrDevice>{}, "isVrDevice" },
{ FakeJni::Function<&InitParams::buildVariant>{}, "buildVariant" },
{ FakeJni::Function<&InitParams::vrContext>{}, "vrContext" },
END_NATIVE_DESCRIPTOR

std::shared_ptr<InitParams> build_desktop_init_params(std::shared_ptr<PlatformParams> platform_params,
                                                        std::shared_ptr<DeviceParams> device_params,
                                                        const std::string& base_url,
                                                        const std::string& user_agent) {
    auto params = std::make_shared<InitParams>();
    params->platformParams_ = std::move(platform_params);
    params->deviceParams_ = std::move(device_params);
    params->baseURL_ = std::make_shared<FakeJni::JString>(base_url);
    params->userAgent_ = std::make_shared<FakeJni::JString>(user_agent);
    // Tablet, matching the form factor the User-Agent reports (see
    // build_real_user_agent()). The real app derives both from the same
    // flag, the app's own User-Agent builder's h() calls a device a Phone only when isTablet
    // is false, so reporting Tablet here and Phone there would be
    // internally inconsistent. Stud is a large-display, mouse-and-
    // keyboard, no-touchscreen device, which is the tablet/desktop-class
    // side of that split, not the phone side.
    params->isTablet_ = true;
    params->isPotato_ = false;
    params->isVrDevice_ = false;
    return params;
}

}  // namespace stud::jni_bridge
