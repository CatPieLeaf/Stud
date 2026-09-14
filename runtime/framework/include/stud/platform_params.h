#pragma once

#include <fake-jni/fake-jni.h>

#include <memory>

// com.roblox.engine.jni.model.PlatformParams, reimplemented as a real
// FakeJni object so libroblox.so's native code can operate on it via
// genuine JNI field accessors, exactly as it would on a real device.
//
// Field layout and types are ground-truth, not guessed: traced directly
// from the real Roblox APK's app's own device-params builder, which builds this exact object from
// PackageManager.hasSystemFeature() results before handing it to the
// engine. See the engineering notes, "Desktop-vs-mobile spoof: SOLVED" section,
// for the full trace. This is a clean-room reimplementation of the *shape*
// of that object as observed from the real APK's bytecode, not copied
// from any of the app's source.
//
// See the engineering notes, milestone M4.

namespace stud::jni_bridge {

class PlatformParams : public FakeJni::JObject {
public:
    DEFINE_CLASS_NAME("com/roblox/engine/jni/model/PlatformParams")

    std::shared_ptr<FakeJni::JString> assetFolderPath;
    FakeJni::JBoolean isTouchDevice = false;
    FakeJni::JBoolean isMouseDevice = false;
    FakeJni::JBoolean isKeyboardDevice = false;
    FakeJni::JFloat dpiScale = 0.0f;
    FakeJni::JInt viewportWidthMm = 0;
    FakeJni::JInt viewportHeightMm = 0;
};

// Builds a PlatformParams reporting Stud's desktop spoof: no touchscreen,
// mouse and keyboard present (isMouseDevice/isKeyboardDevice both key off
// the same "android.hardware.type.pc" feature check on a real device; see
// the ground-truth trace above). `dpi_scale`/`viewport_width_mm`/
// `viewport_height_mm` should reflect the real host display; sensible
// desktop-monitor defaults are used if not provided.
std::shared_ptr<PlatformParams> build_desktop_platform_params(
    const std::string& asset_folder_path, float dpi_scale = 1.0f, int viewport_width_mm = 340,
    int viewport_height_mm = 190);

// The object MainGameActivity actually hands to
// InitParams.Builder.setPlatformParams() isn't a bare PlatformParams, the
// real APK wraps it in a subclass (an internal class in the
// analyzed build) adding four more fields. Ground-truth traced from that
// class's the app's own code: isLuaHomePageEnabled/isLuaGamesPageEnabled/
// isLuaChatEnabled are hardcoded `true` unconditionally on a real device
// (universal Lua app-shell feature flags, not device-type-dependent), plus
// a second, redundant isTablet (same underlying flag as InitParams'
// isTablet).
//
// Named descriptively here rather than replicating the real internal
// class name. That name isn't stable across Roblox versions
// (internal names change per build), and "user supplies
// any APK version they want" is a locked project decision, so hardcoding a
// version-specific internal name would be fragile. No evidence so far
// that libroblox.so's native code relies on that specific class name
// string (JNI field access works by field name via GetFieldID regardless
// of the declaring class's name), confirmed via extensive real
// integration testing since (the engineering notes), no class-name-related
// failure ever observed.
class PlatformParamsWithLuaFlags : public PlatformParams {
public:
    // Distinct from PlatformParams's own class name, registering two C++
    // types under the identical Java class name would collide in jnivm's
    // class registry. See the comment above for why this isn't the real
    // (version-fragile) APK class name either.
    DEFINE_CLASS_NAME("stud/jni_bridge/PlatformParamsWithLuaFlags", PlatformParams)

    FakeJni::JBoolean isLuaHomePageEnabled = true;
    FakeJni::JBoolean isLuaGamesPageEnabled = true;
    FakeJni::JBoolean isLuaChatEnabled = true;
    FakeJni::JBoolean isTablet = false;
};

// Builds the augmented PlatformParams MainGameActivity actually passes to
// InitParams, base desktop spoof plus the four Lua-flags fields, all
// matching real-device values (see PlatformParamsWithLuaFlags above).
std::shared_ptr<PlatformParamsWithLuaFlags> build_desktop_platform_params_with_lua_flags(
    const std::string& asset_folder_path, float dpi_scale = 1.0f, int viewport_width_mm = 340,
    int viewport_height_mm = 190);

}  // namespace stud::jni_bridge
