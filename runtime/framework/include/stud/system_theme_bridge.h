#pragma once

// The real system-theme protocol
// (`com.roblox.universalapp.systemtheme.SystemThemeProtocol`), which Stud
// has never answered -- so the Lua app was told nothing about the desktop's
// light/dark setting and every web-view panel opened in the site's light
// theme regardless.
//
// Mechanism, confirmed against the app's own code from the real class rather than guessed:
//
//   * The app publishes a **setTheme** message on the MessageBus carrying
//     one integer under a key the engine names itself
//     (`JNISystemThemeProtocol.getSystemThemeParamKey()`). The values are
//     the real enum: ERROR 0, LIGHT 1, DARK 2, SYSTEM_LIGHT 3,
//     SYSTEM_DARK 4.
//   * The platform answers by setting a **cookie** on the Roblox site:
//     `RBXThemeOverride=light|dark; Path=/`. That cookie is the whole
//     mechanism behind "the website is in dark mode" -- it is not a
//     rendering trick, it is the site's own theme, which is why this is
//     the only way to get a genuinely dark panel rather than an inverted
//     one. It also sets `RBXHideThemeSetting=True; Path=/my/account`, so
//     the account page stops offering a theme picker that the app is
//     already driving.
//   * The platform publishes a **themeUpdated** message whenever the
//     system theme changes, and the app then reads it back through
//     `SystemThemeProtocol.getSystemTheme()` -- the static the engine
//     calls into Java, which on a real device reads
//     `Configuration.uiMode & UI_MODE_NIGHT_MASK`.
//
// Stud supplies the desktop's own setting for that last part (see
// set_system_dark_mode), so "dark" means the user's actual desktop, not a
// value invented here.

#include <functional>
#include <string>

#include <fake-jni/fake-jni.h>

#include "stud/linker.h"

namespace stud::jni_bridge {

// Tells this process whether the desktop is in dark mode. Read by the UI
// process, which is the one with a real desktop connection, and handed
// over with the rest of the launch payload.
void set_system_dark_mode(bool dark);
bool system_dark_mode();

// The real enum value `SystemThemeProtocol.getSystemTheme()` should
// return for the current desktop: SYSTEM_DARK(4) or SYSTEM_LIGHT(3).
int system_theme_value();

// Subscribes to the app's setTheme message and publishes the initial
// themeUpdated, so the app asks for the system theme at all. `on_theme`
// receives the real theme name ("light" or "dark") whenever the app
// settles on one -- that is what the web view needs for its cookie.
bool run_system_theme_bootstrap(FakeJni::Jvm& jvm, const stud::linker::LoadedLibrary& lib,
                                std::function<void(const std::string& theme_name)> on_theme);

// Publishes themeUpdated. Called once at bring-up, and again if the
// desktop setting changes while Stud is running.
void publish_system_theme_updated(FakeJni::Jvm& jvm, const stud::linker::LoadedLibrary& lib);

// The theme the app last settled on, or empty if it has not said. The web
// view sends this as `RBXThemeOverride` so a panel opens in the same theme
// as the app around it.
std::string current_theme_name();

}  // namespace stud::jni_bridge
