// M8 test: real roblox-player:/roblox: launch-URI parsing
// (ui/src/launch_uri.h). Format and field casing verified live this
// session against multiple real Play-button clicks (both a direct
// browser default-handler launch and Firefox's portal-activated
// "choose installed app" flow) -- these are real key names, and the
// synthetic values below match the real shape confirmed live.

#include "launch_uri.h"

#include <cstdio>
#include <cstdlib>

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
    // A normal, non-launch argument (e.g. no CLI args at all, or some
    // unrelated string) -- correctly recognized as "not a launch URI".
    check(!stud::ui::parse_launch_uri("").has_value(), "an empty string is not a launch URI");
    check(!stud::ui::parse_launch_uri("/usr/bin/stud-ui").has_value(),
          "a plain file path is not mistaken for a launch URI");
    check(!stud::ui::parse_launch_uri("https://www.roblox.com/games/123").has_value(),
          "an ordinary https URL is not mistaken for a launch URI");

    // Real, well-known format.
    std::string uri =
        "roblox-player://1"
        "+launchmode:play"
        "+gameinfo:AUTH_TICKET_VALUE_HERE"
        "+launchtime:1735689600000"
        "+placelauncherurl:https%3A%2F%2Fwww.roblox.com%2FGame%2FPlaceLauncher.ashx%3Frequest%3DRequestGame%26browserTrackerId%3D123456789%26placeId%3D1818%26isPlayTogetherGame%3DFalse%26referredByPlayerId%3D42%26joinAttemptId%3Df923ce81-874a-4ce8-af0d-bc365d3a2153%26joinAttemptOrigin%3DPlayButton"
        "+browsertrackerid:123456789"
        "+robloxLocale:en_us"
        "+gameLocale:en_us"
        "+channel:";

    auto parsed = stud::ui::parse_launch_uri(uri);
    check(parsed.has_value(), "a real-shaped roblox-player: URI is recognized");
    check(parsed->launch_mode == "play", "launchmode parses correctly");
    check(parsed->game_info == "AUTH_TICKET_VALUE_HERE", "gameinfo (the real auth ticket) parses correctly");
    check(parsed->place_launcher_url ==
              "https://www.roblox.com/Game/PlaceLauncher.ashx?request=RequestGame&browserTrackerId="
              "123456789&placeId=1818&isPlayTogetherGame=False&referredByPlayerId=42&joinAttemptId="
              "f923ce81-874a-4ce8-af0d-bc365d3a2153&joinAttemptOrigin=PlayButton",
          "placelauncherurl is correctly percent-decoded, including the URL it itself contains");
    check(parsed->launch_time == "1735689600000", "launchtime parses correctly");
    check(parsed->browser_tracker_id == "123456789", "browsertrackerid parses correctly");
    check(parsed->roblox_locale == "en_us", "robloxLocale parses correctly despite mixed case in the key");
    check(parsed->game_locale == "en_us", "gameLocale parses correctly despite mixed case in the key");
    check(parsed->channel.empty(), "an empty value (channel:) parses as an empty string, not an error");
    check(parsed->place_id == 1818, "placeId is parsed out of place_launcher_url's own query string");
    check(parsed->join_attempt_id == "f923ce81-874a-4ce8-af0d-bc365d3a2153",
          "joinAttemptId is parsed out of place_launcher_url's own query string");
    check(parsed->referred_by_player_id == 42,
          "referredByPlayerId is parsed out of place_launcher_url's own query string");
    check(parsed->join_attempt_origin == "PlayButton",
          "joinAttemptOrigin is parsed out of place_launcher_url's own query string");

    // No placelauncherurl at all (e.g. a bare, non-game launch) -- the
    // deep-link-derived fields stay at their honest zero/empty defaults
    // rather than erroring.
    auto parsed_bare = stud::ui::parse_launch_uri("roblox-player://1+launchmode:app");
    check(parsed_bare.has_value() && parsed_bare->place_id == 0 &&
              parsed_bare->join_attempt_id.empty() && parsed_bare->referred_by_player_id == 0 &&
              parsed_bare->join_attempt_origin.empty(),
          "deep-link fields default to zero/empty when there's no placelauncherurl at all");

    // The "roblox:" scheme (also confirmed real, distinct from
    // "roblox-player:") is accepted too.
    auto parsed_alt = stud::ui::parse_launch_uri("roblox://1+launchmode:app");
    check(parsed_alt.has_value(), "the roblox: scheme (not just roblox-player:) is recognized");
    check(parsed_alt->launch_mode == "app", "roblox: scheme payload parses correctly too");

    // A URI without the "//" prefix still parses -- tolerant of format
    // variation given the real format isn't independently re-verified
    // here.
    auto parsed_no_slashes = stud::ui::parse_launch_uri("roblox-player:1+launchmode:play+gameinfo:XYZ");
    check(parsed_no_slashes.has_value() && parsed_no_slashes->launch_mode == "play" &&
              parsed_no_slashes->game_info == "XYZ",
          "a URI missing the // prefix still parses correctly");

    // The format the website actually sends, verbatim from a Chrome
    // report: an ordinary URL with an ordinary query string, nothing
    // "+"-separated anywhere in it. Reading this as the other format
    // left place_id at 0, which made Stud treat a browser click as a
    // bare launch and open the home screen.
    auto parsed_web = stud::ui::parse_launch_uri(
        "roblox://experiences/start?placeId=10518166490"
        "&gameInstanceId=37ecc77f-db64-47e9-8c14-d3d1dcb71e3"
        "&joinAttemptId=bcf04c5a-6826-4a26-9173-c93a5d8412a8"
        "&joinAttemptOrigin=PlayButton&referredByPlayerId=0");
    check(parsed_web.has_value(), "the website's own roblox:// link parses");
    check(parsed_web->place_id == 10518166490LL, "placeId is read from the query string");
    check(parsed_web->game_instance_id == "37ecc77f-db64-47e9-8c14-d3d1dcb71e3",
          "gameInstanceId names the server to join");
    check(parsed_web->join_attempt_id == "bcf04c5a-6826-4a26-9173-c93a5d8412a8",
          "joinAttemptId is read");
    check(parsed_web->join_attempt_origin == "PlayButton", "joinAttemptOrigin is read");
    check(parsed_web->referred_by_player_id == 0, "referredByPlayerId is read");
    check(parsed_web->launch_mode == "play", "a link naming a place is a request to play it");

    // A place link with no server named is still a join -- of any server
    // for that place, which is what the site sends from an experience
    // page.
    auto parsed_place_only =
        stud::ui::parse_launch_uri("roblox://experiences/start?placeId=606849621");
    check(parsed_place_only.has_value() && parsed_place_only->place_id == 606849621LL &&
              parsed_place_only->game_instance_id.empty(),
          "a place-only link parses, with no server named");

    // Neither format may be read as the other: the launcher protocol's
    // own links still parse exactly as before.
    auto parsed_legacy_still =
        stud::ui::parse_launch_uri("roblox-player://1+launchmode:play+gameinfo:TICKET");
    check(parsed_legacy_still.has_value() && parsed_legacy_still->launch_mode == "play" &&
              parsed_legacy_still->game_info == "TICKET" && parsed_legacy_still->place_id == 0,
          "the launcher protocol's own format still parses");

    std::printf("all launch-uri checks passed\n");
    return 0;
}
