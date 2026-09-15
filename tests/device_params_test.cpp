// M4 test: same approach as jni_bridge_test.cpp, build a DeviceParams and
// an InitParams (wrapping it plus a PlatformParams) and read every field
// back through genuine JNI accessors, not direct C++ member access.
// See the engineering notes, milestone M4.

#include "stud/device_params.h"
#include "stud/init_params.h"
#include "stud/platform_params.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace {

void check(bool condition, const char* what) {
    if (!condition) {
        std::fprintf(stderr, "FAILED: %s\n", what);
        std::exit(1);
    }
    std::printf("ok: %s\n", what);
}

std::string get_string_field(FakeJni::Env& env, jobject obj, jclass cls, const char* name) {
    jobject field = env.GetObjectField(obj, env.GetFieldID(cls, name, "Ljava/lang/String;"));
    if (field == nullptr) return "";
    return env.GetStringUTFChars(reinterpret_cast<jstring>(field), nullptr);
}

// Real correction (the engineering notes, "InitParams.platformParams reads
// null" chain): InitParams's own properties are registered as real
// zero-arg METHODS now, matching AutoValue's actual accessor convention
// confirmed directly against the real libroblox.so, checked.
// DeviceParams's own fields (checked separately below, nested inside a
// method-returned InitParams.deviceParams()) are still genuine FIELDS,
// unaffected by this, only InitParams itself needed correcting.
std::string call_string_method(FakeJni::Env& env, jobject obj, jclass cls, const char* name) {
    jobject result = env.CallObjectMethod(
        obj, env.GetMethodID(cls, name, "()Ljava/lang/String;"));
    if (result == nullptr) return "";
    return env.GetStringUTFChars(reinterpret_cast<jstring>(result), nullptr);
}

}  // namespace

int main() {
    FakeJni::Jvm jvm;
    jvm.registerClass<stud::jni_bridge::PlatformParams>();
    jvm.registerClass<stud::jni_bridge::DeviceParams>();
    jvm.registerClass<stud::jni_bridge::InitParams>();
    jvm.attachLibrary("");
    FakeJni::LocalFrame frame(jvm);
    auto& env = frame.getJniEnv();

    auto device = stud::jni_bridge::build_desktop_device_params("34", "Stud", "2.733.988", "1920x1080",
                                                                  1920, 1080, 16384);
    jobject dref = env.createLocalReference(device);
    jclass dcls = env.GetObjectClass(dref);

    check(get_string_field(env, dref, dcls, "osVersion") == "34", "DeviceParams.osVersion round-trips");
    check(get_string_field(env, dref, dcls, "manufacturer") == "Stud",
          "DeviceParams.manufacturer round-trips");
    check(env.GetIntField(dref, env.GetFieldID(dcls, "deviceTotalMemoryMB", "I")) == 16384,
          "DeviceParams.deviceTotalMemoryMB round-trips");
    check(env.GetBooleanField(dref, env.GetFieldID(dcls, "cpu64Bit", "Z")) == JNI_TRUE,
          "DeviceParams.cpu64Bit is true");
    check(env.GetBooleanField(dref, env.GetFieldID(dcls, "isLowRamDevice", "Z")) == JNI_FALSE,
          "DeviceParams.isLowRamDevice is false");
    check(env.GetLongField(dref, env.GetFieldID(dcls, "lowMemoryKillerBackgroundAppThreshold", "J")) == 0,
          "DeviceParams.lowMemoryKillerBackgroundAppThreshold defaults to 0 (no Android LMK on Linux)");

    auto platform = stud::jni_bridge::build_desktop_platform_params("/data/stud/assets");
    auto init = stud::jni_bridge::build_desktop_init_params(platform, device, "https://www.roblox.com",
                                                              "Stud/0.1");
    jobject iref = env.createLocalReference(init);
    jclass icls = env.GetObjectClass(iref);

    check(call_string_method(env, iref, icls, "baseURL") == "https://www.roblox.com",
          "InitParams.baseURL() round-trips");
    check(call_string_method(env, iref, icls, "userAgent") == "Stud/0.1",
          "InitParams.userAgent() round-trips");
    check(env.CallBooleanMethod(iref, env.GetMethodID(icls, "isTablet", "()Z")) == JNI_FALSE,
          "InitParams.isTablet() is false (desktop spoof)");
    check(env.CallBooleanMethod(iref, env.GetMethodID(icls, "isPotato", "()Z")) == JNI_FALSE,
          "InitParams.isPotato() is false");
    check(env.CallBooleanMethod(iref, env.GetMethodID(icls, "isVrDevice", "()Z")) == JNI_FALSE,
          "InitParams.isVrDevice() is false");

    jobject nested_device_ref = env.CallObjectMethod(
        iref, env.GetMethodID(icls, "deviceParams", "()Lcom/roblox/engine/jni/model/DeviceParams;"));
    check(nested_device_ref != nullptr, "InitParams.deviceParams() is non-null");
    jclass nested_device_cls = env.GetObjectClass(nested_device_ref);
    check(get_string_field(env, nested_device_ref, nested_device_cls, "osVersion") == "34",
          "InitParams.deviceParams() is the same DeviceParams object (nested access works)");

    std::printf("all device/init-params checks passed\n");
    return 0;
}
