#pragma once

#include <cstdint>
#include <string>

// Discord Rich Presence over Discord's local IPC socket. No library: the
// protocol is a handshake and JSON frames. Discord not running is an ordinary
// condition; presence appears when it starts, and comes back when it restarts.

namespace stud::ui {

// Roblox's own Discord application ("ROBLOX"), as Discord lists it among its
// detectable games, so the presence reads as Roblox.
inline constexpr const char* kRobloxDiscordApplicationId = "363445589247131668";

// What the profile shows. Empty fields are left out; an image is an http(s)
// URL, which Discord accepts in place of an uploaded asset key.
struct DiscordActivity {
    std::string details;
    std::string state;
    std::string large_image;
    std::string large_text;
    std::string small_image;
    std::string small_text;
    int64_t start = 0;  // Unix seconds; 0 for none
    int64_t end = 0;
    std::string button_label;
    std::string button_url;
};

void discord_rpc_start(const std::string& application_id);
void discord_rpc_set(const DiscordActivity& activity);
// Clears the presence and disconnects.
void discord_rpc_stop();

}  // namespace stud::ui
