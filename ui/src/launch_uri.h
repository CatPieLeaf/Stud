#pragma once

#include <optional>
#include <string>

// Roblox launch links (roblox-player: and roblox:) that the browser hands to Stud.
// roblox-player:1+key:value+..., values percent-encoded. The join fields in
// place_launcher_url's query string are parsed out into the fields below.

namespace stud::ui {

struct LaunchUri {
    std::string launch_mode;  // e.g. "play", "app"
    std::string game_info;  // the opaque join ticket
    // Only its query string is read. It is never requested: anyone can write the
    // link, and the request would carry the session cookie to that host.
    std::string place_launcher_url;
    std::string launch_time;
    std::string browser_tracker_id;
    std::string roblox_locale;
    std::string game_locale;
    std::string channel;

    // From place_launcher_url's query string; zero or empty when absent.
    long long place_id = 0;
    std::string join_attempt_id;
    long long referred_by_player_id = 0;
    std::string join_attempt_origin;
    // gameInstanceId; empty means any server of the place.
    std::string game_instance_id;
    // A private server's share-link or access code: credentials, never logged.
    std::string link_code;
    std::string access_code;
    // A follow link names a user instead of a place.
    long long user_id = 0;
};

// nullopt when `uri` isn't a roblox-player: or roblox: link, i.e. a plain launch.
std::optional<LaunchUri> parse_launch_uri(const std::string& uri);

}  // namespace stud::ui
