#pragma once

#include <cstdint>
#include <string>
#include <utility>
#include <vector>

// Rich Presence over Discord's local IPC socket: a handshake and JSON frames, so no
// library. Presence appears whenever Discord is running.

namespace stud::ui {

// Roblox's own Discord application, so the presence reads as Roblox.
inline constexpr const char* kRobloxDiscordApplicationId = "363445589247131668";

// Empty fields are left out; images may be http(s) URLs instead of asset keys.
struct DiscordActivity {
    std::string details;
    std::string state;
    std::string large_image;
    std::string large_text;
    std::string small_image;
    std::string small_text;
    int64_t start = 0;  // Unix seconds; 0 for none
    int64_t end = 0;
    // What "Playing ..." names: 0 application, 1 state, 2 details; -1 Discord decides.
    int status_display = -1;
    // Discord allows two.
    std::vector<std::pair<std::string, std::string>> buttons;  // label, URL
};

void discord_rpc_start(const std::string& application_id);
void discord_rpc_set(const DiscordActivity& activity);
void discord_rpc_stop();

}  // namespace stud::ui
