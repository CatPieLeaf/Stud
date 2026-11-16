#pragma once

#include <QString>

// Roblox's macOS client, checked the way macOS checks it before running it.
//
// The code directory of the bundle's executable has to carry a signature by
// Roblox's Developer ID, issued under Apple's Developer ID authority and
// Apple's root (pinned here, never taken from the signature); every page of
// every executable has to hash to what its code directory says; and every
// other file has to match the seal in _CodeSignature/CodeResources, so a file
// changed, removed or added fails. Nested code (helpers, libraries, apps
// inside the bundle) is pinned by the seal's code-directory hash and checked
// the same way.

namespace stud::ui::codesign {

// Roblox Corporation's Apple team, in its Developer ID and every code
// directory it signs.
inline constexpr const char* kRobloxTeam = "2CFABCH843";

// Empty when the bundle at `app` (a .app directory) is Roblox's as signed, its
// executable's code directory naming `identifier`; otherwise what failed.
QString verify_app(const QString& app, const QString& identifier);

}  // namespace stud::ui::codesign
