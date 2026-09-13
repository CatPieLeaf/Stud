// M4 test (first increment): builds a PlatformParams object -- Stud's
// desktop-vs-mobile spoof, ground-truth-traced from the real Roblox APK
// (see the engineering notes) -- via libjnivm's real FakeJni::Jvm/JNIEnv, then
// reads every field back through genuine JNI field accessors (not direct
// C++ member access) to prove the object is shaped exactly as
// libroblox.so's native code, using real JNI calls, would expect. Not yet
// tested against the real libroblox.so itself -- blocked on M2's
// DT_GNU_HASH gap. See the engineering notes, milestone M4.

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

}  // namespace

int main() {
    FakeJni::Jvm jvm;
    jvm.registerClass<stud::jni_bridge::PlatformParams>();
    jvm.registerClass<stud::jni_bridge::PlatformParamsWithLuaFlags>();
    jvm.attachLibrary("");
    FakeJni::LocalFrame frame(jvm);
    auto& env = frame.getJniEnv();

    auto params = stud::jni_bridge::build_desktop_platform_params("/data/stud/assets", 1.25f, 350, 200);
    jobject ref = env.createLocalReference(params);
    jclass cls = env.GetObjectClass(ref);

    check(env.GetBooleanField(ref, env.GetFieldID(cls, "isTouchDevice", "Z")) == JNI_FALSE,
          "isTouchDevice reads false via real JNI field access");
    check(env.GetBooleanField(ref, env.GetFieldID(cls, "isMouseDevice", "Z")) == JNI_TRUE,
          "isMouseDevice reads true via real JNI field access");
    check(env.GetBooleanField(ref, env.GetFieldID(cls, "isKeyboardDevice", "Z")) == JNI_TRUE,
          "isKeyboardDevice reads true via real JNI field access");

    jfloat dpi = env.GetFloatField(ref, env.GetFieldID(cls, "dpiScale", "F"));
    check(dpi > 1.249f && dpi < 1.251f, "dpiScale round-trips through JNI correctly");

    check(env.GetIntField(ref, env.GetFieldID(cls, "viewportWidthMm", "I")) == 350,
          "viewportWidthMm round-trips through JNI correctly");
    check(env.GetIntField(ref, env.GetFieldID(cls, "viewportHeightMm", "I")) == 200,
          "viewportHeightMm round-trips through JNI correctly");

    jobject path_ref =
        env.GetObjectField(ref, env.GetFieldID(cls, "assetFolderPath", "Ljava/lang/String;"));
    check(path_ref != nullptr, "assetFolderPath field is non-null");
    const char* path_chars = env.GetStringUTFChars(reinterpret_cast<jstring>(path_ref), nullptr);
    check(std::strcmp(path_chars, "/data/stud/assets") == 0,
          "assetFolderPath round-trips through JNI as the right string");

    check(stud::jni_bridge::PlatformParams::getClassName() == "com/roblox/engine/jni/model/PlatformParams",
          "class name matches the real Roblox JNI type this object stands in for");

    // PlatformParamsWithLuaFlags: the real MainGameActivity always wraps in
    // this subclass before handing PlatformParams to InitParams. Confirms
    // inherited base fields are still reachable via JNI on a subclass
    // instance, alongside the four fields it adds.
    auto lua_params = stud::jni_bridge::build_desktop_platform_params_with_lua_flags("/data/stud/assets");
    jobject lua_ref = env.createLocalReference(lua_params);
    jclass lua_cls = env.GetObjectClass(lua_ref);

    check(env.GetBooleanField(lua_ref, env.GetFieldID(lua_cls, "isMouseDevice", "Z")) == JNI_TRUE,
          "PlatformParamsWithLuaFlags inherits base PlatformParams fields, reachable via JNI");
    check(env.GetBooleanField(lua_ref, env.GetFieldID(lua_cls, "isLuaHomePageEnabled", "Z")) == JNI_TRUE,
          "isLuaHomePageEnabled is true, matching real-device behavior (hardcoded true, not spoof-dependent)");
    check(env.GetBooleanField(lua_ref, env.GetFieldID(lua_cls, "isLuaGamesPageEnabled", "Z")) == JNI_TRUE,
          "isLuaGamesPageEnabled is true");
    check(env.GetBooleanField(lua_ref, env.GetFieldID(lua_cls, "isLuaChatEnabled", "Z")) == JNI_TRUE,
          "isLuaChatEnabled is true");
    check(env.GetBooleanField(lua_ref, env.GetFieldID(lua_cls, "isTablet", "Z")) == JNI_FALSE,
          "PlatformParamsWithLuaFlags.isTablet is false (desktop spoof)");

    // shared_ptr<PlatformParamsWithLuaFlags> must upcast cleanly into
    // InitParams.platformParams (typed as shared_ptr<PlatformParams>) --
    // this is exactly how the real bootstrap wires it.
    std::shared_ptr<stud::jni_bridge::PlatformParams> upcast = lua_params;
    check(upcast != nullptr, "PlatformParamsWithLuaFlags upcasts to PlatformParams cleanly");

    std::printf("all jni-bridge checks passed\n");
    return 0;
}
