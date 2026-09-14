#pragma once

#include <cstdint>
#include <string>

// Discord rich presence, spoken directly over Discord's local IPC socket.
//
// The protocol is small enough that a library would be more dependency
// than help: connect to $XDG_RUNTIME_DIR/discord-ipc-<n>, send a
// handshake naming the application id, then frames of JSON. Each frame is
// a little-endian opcode and length followed by the payload.
//
// This lives in render-host because the connection has to last exactly as
// long as the session does, and render-host is the process that does.
// Process A exits right after launching; Process B is sandboxed and
// cannot see the socket at all.

namespace stud::render_host {

// Stud's own Discord application. Compiled in rather than configurable:
// it identifies Stud itself, so it is no more a user setting than the
// application's name is, and a wrong value here produces a presence that
// silently claims to be something else.
inline constexpr const char* kDiscordApplicationId = "1547140974948520026";

// Opens the connection, if rich presence is enabled and an application id
// is configured. Safe to call when Discord is not running; it fails
// quietly and every later call is a no-op, because Discord being closed is
// an ordinary condition rather than an error.
void discord_rpc_start(const std::string& application_id);

// What the user is doing, as the presence should show it.
struct GamePresence {
    // Empty means "not in an experience": the presence falls back to
    // Stud's own identity rather than being cleared, so it still reads as
    // Roblox rather than disappearing between games.
    std::string universe_name;
    std::string creator_name;
    // A thumbnail URL Discord fetches itself. Discord accepts an http(s)
    // URL as an asset key, which is what makes a per-game image possible
    // without uploading anything to the application.
    std::string thumbnail_url;
    // roblox://experiences/start?placeId=...&gameInstanceId=..., shown as
    // a button when the user asked for it.
    std::string join_url;
    // Seconds since the epoch, for the elapsed-time counter.
    int64_t started_at = 0;
};

// Replaces the presence. Called on a join, on a leave, and when the
// server changes within one session.
void discord_rpc_set_game(const GamePresence& presence);

// Clears the presence and closes the connection.
void discord_rpc_stop();

}  // namespace stud::render_host
