#pragma once

#include <fake-jni/fake-jni.h>

#include <memory>
#include <string>

#include "stud/game_activity_stubs.h"
#include "stud/platform_params.h"

// com.roblox.engine.jni.autovalue.StartAppParams, reimplemented as a real
// FakeJni object -- same approach as InitParams/PlatformParams/DeviceParams.
// Field layout is ground-truth, traced directly from the app's own code
// of the class itself (10 real AutoValue getter methods -- see
// the engineering notes, the "V2 app-bridge API" entry). Real getter methods,
// not plain fields -- same already-confirmed reason as InitParams (AutoValue
// classes expose properties as zero-arg methods, and jnivm's own field-vs-
// method registration is determined purely by whether the wrapped pointer
// is a data member or a real member function).

namespace stud::jni_bridge {

class StartAppParams : public FakeJni::JObject {
public:
    DEFINE_CLASS_NAME("com/roblox/engine/jni/autovalue/StartAppParams")

    std::shared_ptr<FakeJni::JString> appStarterPlace_;
    std::shared_ptr<FakeJni::JString> appStarterScript_;
    FakeJni::JLong appUserId_ = 0;
    FakeJni::JBoolean isUnder13_ = false;
    FakeJni::JInt membershipType_ = 0;
    std::shared_ptr<PlatformParams> platformParams_;
    std::shared_ptr<FakeJni::JString> selectedTheme_;
    std::shared_ptr<SurfaceStub> surface_;
    std::shared_ptr<FakeJni::JString> username_;
    std::shared_ptr<ActivityStub> vrContext_;

    std::shared_ptr<FakeJni::JString> appStarterPlace() { return appStarterPlace_; }
    std::shared_ptr<FakeJni::JString> appStarterScript() { return appStarterScript_; }
    FakeJni::JLong appUserId() { return appUserId_; }
    FakeJni::JBoolean isUnder13() { return isUnder13_; }
    FakeJni::JInt membershipType() { return membershipType_; }
    std::shared_ptr<PlatformParams> platformParams() { return platformParams_; }
    std::shared_ptr<FakeJni::JString> selectedTheme() { return selectedTheme_; }
    std::shared_ptr<SurfaceStub> surface() { return surface_; }
    std::shared_ptr<FakeJni::JString> username() { return username_; }
    std::shared_ptr<ActivityStub> vrContext() { return vrContext_; }
};

// Builds a real, honest StartAppParams: empty strings/zeros for anything
// Stud has no real source for yet (matches every other "honest
// placeholder, not fabricated content" builder in this project), the
// same platform_params object the caller already built, and a real
// ActivityStub. `surface` MUST be the same real Surface GameActivity's
// own lifecycle already created (see GameActivityLifecycleResult::
// surface's doc comment) -- reused here, not freshly constructed, to
// avoid a second, real, independently-mapped window.
std::shared_ptr<StartAppParams> build_desktop_start_app_params(
    std::shared_ptr<PlatformParams> platform_params, std::shared_ptr<SurfaceStub> surface);

}  // namespace stud::jni_bridge
