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
    // No touchscreen, with a real mouse and keyboard: what a desktop-class
    // Android device reports (`android.hardware.type.pc`, the source of both
    // flags). The in-frame cursor doesn't depend on these; it follows from
    // SOURCE_MOUSE input events. isKeyboardDevice does matter: with it false,
    // the engine ignores delivered keys. Do not flip these to mimic a phone.
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
    // See build_desktop_platform_params().
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
