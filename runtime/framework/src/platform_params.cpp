#include "stud/platform_params.h"

namespace stud::jni_bridge {

BEGIN_NATIVE_DESCRIPTOR(PlatformParams)
{ FakeJni::Field<&PlatformParams::assetFolderPath>{}, "assetFolderPath" },
{ FakeJni::Field<&PlatformParams::isTouchDevice>{}, "isTouchDevice" },
{ FakeJni::Field<&PlatformParams::isMouseDevice>{}, "isMouseDevice" },
{ FakeJni::Field<&PlatformParams::isKeyboardDevice>{}, "isKeyboardDevice" },
{ FakeJni::Field<&PlatformParams::dpiScale>{}, "dpiScale" },
{ FakeJni::Field<&PlatformParams::viewportWidthMm>{}, "viewportWidthMm" },
{ FakeJni::Field<&PlatformParams::viewportHeightMm>{}, "viewportHeightMm" },
END_NATIVE_DESCRIPTOR

std::shared_ptr<PlatformParams> build_desktop_platform_params(const std::string& asset_folder_path,
                                                                float dpi_scale, int viewport_width_mm,
                                                                int viewport_height_mm) {
    auto params = std::make_shared<PlatformParams>();
    params->assetFolderPath = std::make_shared<FakeJni::JString>(asset_folder_path);
    // Real Android values, read straight off the app's own builder
    // (the app's own device-params builder):
    //   isTouchDevice = hasSystemFeature("android.hardware.touchscreen")
    //   isMouseDevice = hasSystemFeature("android.hardware.type.pc")
    //   isKeyboardDevice = hasSystemFeature("android.hardware.type.pc")
    // On a real phone or tablet that is touch=true, mouse=false,
    // keyboard=false, and such a device with an OTG mouse attached still
    // shows Roblox's own in-frame cursor, so the cursor is NOT gated on
    // these flags; it follows from real SOURCE_MOUSE input events.
    //
    // Stud used to claim the exact opposite (touch=false, mouse=true), i.e.
    // "I am a PC". That is a real behavioural difference, not a cosmetic
    // one: on a PC the engine delegates the cursor to the host window
    // system instead of drawing one itself, and Stud's surface has no
    // system pointer (the real app's own SurfaceView asks Android for
    // PointerIcon TYPE_NULL), so nothing was drawn at all. Report what a
    // real Android device reports.
    // Stud runs on a real Linux desktop: it genuinely has a real mouse and a
    // real keyboard, and no touchscreen. That is exactly what a desktop-class
    // Android device (`hasSystemFeature("android.hardware.type.pc")`, the
    // real source of these two flags in the app's own device-params builder) reports, so this
    // is the honest description of the machine rather than a spoof.
    //
    // Live-caught regression this pass: these were briefly flipped to a
    // phone's values (touch=true, mouse/keyboard=false) while chasing the
    // missing cursor. It did not affect the cursor, but it DID break
    // keyboard input, because `isKeyboardDevice` is what makes the engine
    // treat a keyboard as present at all; with it false, keys are delivered
    // and then ignored. Do not flip these to mimic a phone again.
    params->isTouchDevice = false;
    params->isMouseDevice = true;
    params->isKeyboardDevice = true;
    params->dpiScale = dpi_scale;
    params->viewportWidthMm = viewport_width_mm;
    params->viewportHeightMm = viewport_height_mm;
    return params;
}

BEGIN_NATIVE_DESCRIPTOR(PlatformParamsWithLuaFlags)
{ FakeJni::Field<&PlatformParamsWithLuaFlags::isLuaHomePageEnabled>{}, "isLuaHomePageEnabled" },
{ FakeJni::Field<&PlatformParamsWithLuaFlags::isLuaGamesPageEnabled>{}, "isLuaGamesPageEnabled" },
{ FakeJni::Field<&PlatformParamsWithLuaFlags::isLuaChatEnabled>{}, "isLuaChatEnabled" },
{ FakeJni::Field<&PlatformParamsWithLuaFlags::isTablet>{}, "isTablet" },
END_NATIVE_DESCRIPTOR

std::shared_ptr<PlatformParamsWithLuaFlags> build_desktop_platform_params_with_lua_flags(
    const std::string& asset_folder_path, float dpi_scale, int viewport_width_mm,
    int viewport_height_mm) {
    auto params = std::make_shared<PlatformParamsWithLuaFlags>();
    params->assetFolderPath = std::make_shared<FakeJni::JString>(asset_folder_path);
    // Real Android values, read straight off the app's own builder
    // (the app's own device-params builder):
    //   isTouchDevice = hasSystemFeature("android.hardware.touchscreen")
    //   isMouseDevice = hasSystemFeature("android.hardware.type.pc")
    //   isKeyboardDevice = hasSystemFeature("android.hardware.type.pc")
    // On a real phone or tablet that is touch=true, mouse=false,
    // keyboard=false, and such a device with an OTG mouse attached still
    // shows Roblox's own in-frame cursor, so the cursor is NOT gated on
    // these flags; it follows from real SOURCE_MOUSE input events.
    //
    // Stud used to claim the exact opposite (touch=false, mouse=true), i.e.
    // "I am a PC". That is a real behavioural difference, not a cosmetic
    // one: on a PC the engine delegates the cursor to the host window
    // system instead of drawing one itself, and Stud's surface has no
    // system pointer (the real app's own SurfaceView asks Android for
    // PointerIcon TYPE_NULL), so nothing was drawn at all. Report what a
    // real Android device reports.
    // Stud runs on a real Linux desktop: it genuinely has a real mouse and a
    // real keyboard, and no touchscreen. That is exactly what a desktop-class
    // Android device (`hasSystemFeature("android.hardware.type.pc")`, the
    // real source of these two flags in the app's own device-params builder) reports, so this
    // is the honest description of the machine rather than a spoof.
    //
    // Live-caught regression this pass: these were briefly flipped to a
    // phone's values (touch=true, mouse/keyboard=false) while chasing the
    // missing cursor. It did not affect the cursor, but it DID break
    // keyboard input, because `isKeyboardDevice` is what makes the engine
    // treat a keyboard as present at all; with it false, keys are delivered
    // and then ignored. Do not flip these to mimic a phone again.
    params->isTouchDevice = false;
    params->isMouseDevice = true;
    params->isKeyboardDevice = true;
    params->dpiScale = dpi_scale;
    params->viewportWidthMm = viewport_width_mm;
    params->viewportHeightMm = viewport_height_mm;
    params->isLuaHomePageEnabled = true;
    params->isLuaGamesPageEnabled = true;
    params->isLuaChatEnabled = true;
    // Mirrors InitParams' own isTablet, which this field is documented
    // to duplicate. Measured: neither value changes the app shell's home
    // layout, both were live-tested, so this is consistency between
    // the two structs, not a behaviour switch.
    params->isTablet = true;
    return params;
}

}  // namespace stud::jni_bridge
