// Which launch arguments are Roblox links (ui/src/launch_uri.h).

#include "launch_uri.h"
#include "check.h"

#include <cstdio>

int main() {
    using stud::ui::is_roblox_link;
    check(!is_roblox_link(""), "an empty string is not a link");
    check(!is_roblox_link("/usr/bin/stud-ui"), "a file path is not a link");
    check(!is_roblox_link("https://www.roblox.com/games/123"), "an https URL is not a link");
    check(!is_roblox_link("roblox:"), "a bare scheme with nothing after it is not a link");
    check(!is_roblox_link("roblox-studio:1+launchmode:edit"), "a Studio link is not a Player link");
    check(is_roblox_link("roblox-player://1+launchmode:play+gameinfo:TICKET"), "the launcher protocol is a link");
    check(is_roblox_link("roblox-player:1+launchmode:app"), "the slashes are optional");
    check(is_roblox_link("roblox://experiences/start?placeId=606849621"), "the website's links are links");
    std::printf("all launch-link checks passed\n");
    return 0;
}
