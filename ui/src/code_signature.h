#pragma once

#include <QString>

// Checks Roblox's macOS client as macOS would: the executable's code directory signed
// by Roblox's Developer ID under Apple's pinned root, every page hashing as recorded,
// every other file matching _CodeSignature/CodeResources, and nested code the same way.

namespace stud::ui::codesign {

inline constexpr const char* kRobloxTeam = "2CFABCH843";

// Empty when `app` is Roblox's as signed, with code directory `identifier`; else what failed.
QString verify_app(const QString& app, const QString& identifier);

}  // namespace stud::ui::codesign
