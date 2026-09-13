#pragma once

#include <optional>
#include <string>

// Real Roblox launch-URI parsing (M8) -- the "magnet link" mechanism:
// clicking Play on roblox.com generates a `roblox-player:` URI (real
// schemes confirmed via reference/sober-oss's own analysis notes,
// ARCHITECTURE.md: "Discord join link handling (x scheme handler
// roblox: and roblox-player:)" -- architecture read only, no code
// copied, per this project's standing clean-room rule), the browser
// hands it to whichever app is registered as the handler (see
// packaging/stud.desktop), and that app launches straight into the
// specific game the link names -- the same mechanism qBittorrent uses
// for magnet: links, not a Stud-specific invention.
//
// Wire format: publicly documented, long-established convention (used
// by numerous real, independent third-party Roblox launcher projects,
// not proprietary or hidden) -- `roblox-player://1+key:value+key:value...`,
// percent-encoded values. UPDATE, real and confirmed: verified live
// against multiple real Play-button clicks this session (both via a
// real browser and Firefox's portal-activated handler) -- format and
// field casing exactly as documented, no surprises.
//
// UPDATE 2, real: place_launcher_url's own query string carries several
// fields in cleartext (placeId, joinAttemptId, referredByPlayerId,
// joinAttemptOrigin) that map directly onto real StartGameParams fields
// (jni-bridge/include/stud/start_game_params.h, ground-truth-traced via
// the app's own code) -- confirmed the real, in-app engine resolves a real join from
// exactly these fields for a plain roblox://experiences/start deep
// link (no gameinfo ticket at all, real Sober captures this session).
// Broken out into their own fields below rather than left for a caller
// to re-parse place_launcher_url's query string itself.

namespace stud::ui {

struct LaunchUri {
    std::string launch_mode;          // e.g. "play", "app"
    std::string game_info;            // the real, opaque join ticket
    std::string place_launcher_url;   // real PlaceLauncher.ashx URL (see its own real-404 caveat, ui/src/main.cpp's fetch_place_launcher_info())
    std::string launch_time;
    std::string browser_tracker_id;
    std::string roblox_locale;
    std::string game_locale;
    std::string channel;

    // Real fields parsed out of place_launcher_url's own query string
    // (see UPDATE 2 above) -- zero/empty if place_launcher_url was
    // absent or didn't carry that particular field.
    long long place_id = 0;
    std::string join_attempt_id;
    long long referred_by_player_id = 0;
    std::string join_attempt_origin;
    // The specific server instance a link points at, when it names one
    // (`gameInstanceId` in the website's own links). Empty means "any
    // server for this place", which is what a plain place link asks for.
    std::string game_instance_id;
};

// Returns std::nullopt if `uri` doesn't start with a recognized scheme
// ("roblox-player:" or "roblox:") -- not an error, callers use this to
// distinguish "I was launched with a game link" from "I was launched
// normally" (e.g. from a desktop icon, with no arguments).
std::optional<LaunchUri> parse_launch_uri(const std::string& uri);

}  // namespace stud::ui
