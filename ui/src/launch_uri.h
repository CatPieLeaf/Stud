#pragma once

#include <optional>
#include <string>

// Roblox launch links: clicking Play on roblox.com opens a
// `roblox-player:` link (or a `roblox:` deep link), the browser hands it to
// the registered handler, Stud, and Stud starts the client straight into
// the game the link names.
//
// Format: `roblox-player:1+key:value+key:value...`, values percent-encoded,
// the long-established public convention launchers read. place_launcher_url's
// query string carries placeId, joinAttemptId, referredByPlayerId and
// joinAttemptOrigin in the clear; they are broken out into fields below
// rather than left for a caller to re-parse.

namespace stud::ui {

struct LaunchUri {
    std::string launch_mode;          // e.g. "play", "app"
    std::string game_info;            // the real, opaque join ticket
    // Read for its query string and nothing else. It comes from a link
    // anyone can write, so it is never requested: the session cookie on
    // that request would go to whatever host the link names.
    std::string place_launcher_url;
    std::string launch_time;
    std::string browser_tracker_id;
    std::string roblox_locale;
    std::string game_locale;
    std::string channel;

    // Real fields parsed out of place_launcher_url's own query string
    // (see UPDATE 2 above), zero/empty if place_launcher_url was
    // absent or didn't carry that particular field.
    long long place_id = 0;
    std::string join_attempt_id;
    long long referred_by_player_id = 0;
    std::string join_attempt_origin;
    // The specific server instance a link points at, when it names one
    // (`gameInstanceId` in the website's own links). Empty means "any
    // server for this place", which is what a plain place link asks for.
    std::string game_instance_id;
    // A private server: its share-link code, or its access code. Both are
    // credentials to that server, never logged.
    std::string link_code;
    std::string access_code;
    // A follow link names the user to join rather than a place.
    long long user_id = 0;
};

// Returns std::nullopt if `uri` doesn't start with a recognized scheme
// ("roblox-player:" or "roblox:"), not an error, callers use this to
// distinguish "I was launched with a game link" from "I was launched
// normally" (e.g. from a desktop icon, with no arguments).
std::optional<LaunchUri> parse_launch_uri(const std::string& uri);

}  // namespace stud::ui
