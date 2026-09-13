#pragma once

#include <fake-jni/fake-jni.h>

#include <memory>
#include <string>

#include "stud/device_params.h"
#include "stud/game_activity_stubs.h"
#include "stud/platform_params.h"

// com.roblox.engine.jni.autovalue.InitParams, reimplemented as a real
// FakeJni object -- same approach as PlatformParams/DeviceParams.
//
// Field layout is ground-truth, traced directly from MainGameActivity's
// setInitParamsForEngine method in the real Roblox APK: it builds
// this object via InitParams.Builder (an AutoValue-generated builder) and
// hands the finished object to the native method
// nativeAppBridgeSetInitParams(InitParams). The real app's Builder-pattern
// construction API is Java-side ergonomics that libroblox.so's native
// getters never see -- native code only ever receives the finished object,
// so this reimplementation skips the builder ceremony and constructs the
// final shape directly, same simplification already made for
// PlatformParams/DeviceParams.
//
// See the engineering notes, milestone M4.
//
// Real bug found and fixed via a real run against the real libroblox.so
// (the engineering notes, the "InitParams.platformParams reads null" chain):
// AutoValue-generated classes expose their properties as zero-arg GETTER
// METHODS (`platformParams()`, `baseURL()`, ...), never as plain public
// fields -- confirmed directly, not guessed, via a real jnivm diagnostic
// showing Roblox's own compiled code calling
// `GetMethodID(InitParams, "platformParams", "()Lcom/roblox/engine/jni/
// model/PlatformParams;")` and getting back "not found," for EVERY one
// of this class's properties, not just this one. jnivm's own field-vs-
// method registration is determined purely by whether the registered
// pointer is a data-member pointer or a real member-FUNCTION pointer
// (checked directly in jnivm::Function<>'s template specializations) --
// wrapping a data member in `FakeJni::Field<>` (the previous version of
// this class) or even `FakeJni::Function<>` both resolve to a FIELD
// registration either way, so real getter methods are required, not
// just a different wrapper around the same data member. Underlying
// storage renamed with a trailing underscore specifically to free up
// the un-suffixed name for the real accessor method (C++ doesn't allow
// a member function and a member variable with the same name).
namespace stud::jni_bridge {

class InitParams : public FakeJni::JObject {
public:
    DEFINE_CLASS_NAME("com/roblox/engine/jni/autovalue/InitParams")

    std::shared_ptr<PlatformParams> platformParams_;
    std::shared_ptr<DeviceParams> deviceParams_;
    std::shared_ptr<FakeJni::JString> baseURL_;
    std::shared_ptr<FakeJni::JString> userAgent_;
    FakeJni::JBoolean isTablet_ = false;
    FakeJni::JBoolean isPotato_ = false;
    FakeJni::JBoolean isVrDevice_ = false;
    // Real, confirmed fields this class was missing entirely until
    // now (real com.roblox.engine.jni.autovalue.InitParams has 9
    // abstract getters; this class only ever implemented 7) -- an
    // AutoValue-generated class's abstract getters are ALL required to
    // be set via the builder before build() succeeds on a real device,
    // so real native code unconditionally expects both to be callable.
    // Missing method registrations here would produce the exact same
    // real "class is null"/null jmethodID failure pattern already
    // documented throughout this whole project. buildVariant's real
    // value ("googleProdRelease") is confirmed -- it's the literal
    // Kotlin metadata module-name tag on every source file
    // in this build ("NativeShell_googleProdRelease"), not guessed.
    // vrContext is a real android.app.Activity reference -- Stud is
    // never a VR device (isVrDevice_ always false), so any real,
    // non-null Activity-shaped object satisfies real code that's
    // expected to branch away before actually using it for VR-specific
    // work, same reasoning already used for StartAppParams/
    // StartGameParams's own vrContext field elsewhere in this project.
    std::shared_ptr<FakeJni::JString> buildVariant_ =
        std::make_shared<FakeJni::JString>("googleProdRelease");
    std::shared_ptr<ActivityStub> vrContext_ = std::make_shared<ActivityStub>();

    std::shared_ptr<PlatformParams> platformParams() { return platformParams_; }
    std::shared_ptr<DeviceParams> deviceParams() { return deviceParams_; }
    std::shared_ptr<FakeJni::JString> baseURL() { return baseURL_; }
    std::shared_ptr<FakeJni::JString> userAgent() { return userAgent_; }
    FakeJni::JBoolean isTablet() { return isTablet_; }
    FakeJni::JBoolean isPotato() { return isPotato_; }
    FakeJni::JBoolean isVrDevice() { return isVrDevice_; }
    std::shared_ptr<FakeJni::JString> buildVariant() { return buildVariant_; }
    std::shared_ptr<ActivityStub> vrContext() { return vrContext_; }
};

// Builds the InitParams Stud hands to nativeAppBridgeSetInitParams.
// isTablet/isPotato/isVrDevice are always false for Stud's desktop target
// (matches the locked "System properties + JNI device-class stubs" spoof
// scope -- see the engineering notes).
std::shared_ptr<InitParams> build_desktop_init_params(std::shared_ptr<PlatformParams> platform_params,
                                                        std::shared_ptr<DeviceParams> device_params,
                                                        const std::string& base_url,
                                                        const std::string& user_agent);

}  // namespace stud::jni_bridge
