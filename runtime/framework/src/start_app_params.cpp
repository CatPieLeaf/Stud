#include "stud/system_theme_bridge.h"
#include "stud/start_app_params.h"

namespace stud::jni_bridge {

BEGIN_NATIVE_DESCRIPTOR(StartAppParams)
{ FakeJni::Function<&StartAppParams::appStarterPlace>{}, "appStarterPlace" },
{ FakeJni::Function<&StartAppParams::appStarterScript>{}, "appStarterScript" },
{ FakeJni::Function<&StartAppParams::appUserId>{}, "appUserId" },
{ FakeJni::Function<&StartAppParams::isUnder13>{}, "isUnder13" },
{ FakeJni::Function<&StartAppParams::membershipType>{}, "membershipType" },
{ FakeJni::Function<&StartAppParams::platformParams>{}, "platformParams" },
{ FakeJni::Function<&StartAppParams::selectedTheme>{}, "selectedTheme" },
{ FakeJni::Function<&StartAppParams::surface>{}, "surface" },
{ FakeJni::Function<&StartAppParams::username>{}, "username" },
{ FakeJni::Function<&StartAppParams::vrContext>{}, "vrContext" },
END_NATIVE_DESCRIPTOR

std::shared_ptr<StartAppParams> build_desktop_start_app_params(
    std::shared_ptr<PlatformParams> platform_params, std::shared_ptr<SurfaceStub> surface) {
    auto params = std::make_shared<StartAppParams>();
    params->appStarterPlace_ = std::make_shared<FakeJni::JString>("");
    params->appStarterScript_ = std::make_shared<FakeJni::JString>("");
    // Real authenticated user id/username, when available (see
    // NativeUserJavaInterfaceStub::getUserId()'s own doc comment for
    // why this matters: UserController::didLogin()'s own real compiled
    // body reads these fields directly off this exact object).
    params->appUserId_ = native_user_id();
    params->isUnder13_ = false;
    params->membershipType_ = 0;
    params->platformParams_ = std::move(platform_params);
    // The desktop's own setting, not a fixed "Light". The real app sends
    // whichever theme it is running in, and the engine takes this at face
    // value, so hardcoding one meant the app started light on a dark
    // desktop no matter what the system-theme protocol said afterwards.
    params->selectedTheme_ =
        std::make_shared<FakeJni::JString>(system_dark_mode() ? "Dark" : "Light");
    // User-reported bug, fixed (the engineering notes, "two windows,
    // one invisible" entry): this used to construct a brand-new
    // SurfaceStub of its own; Roblox's own ANativeWindow_fromSurface()
    // call on THAT jobject creates a genuinely separate, second real
    // Wayland window. Reuse the SAME real surface GameActivity's own
    // lifecycle already created, matching every other real V2 call
    // site's own fix for this exact class of bug.
    params->surface_ = std::move(surface);
    params->username_ = std::make_shared<FakeJni::JString>(native_username());
    params->vrContext_ = std::make_shared<ActivityStub>();
    return params;
}

}  // namespace stud::jni_bridge
