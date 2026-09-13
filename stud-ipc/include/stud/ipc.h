#pragma once

#include <stdexcept>
#include <string>

// Real IPC between Stud's two processes (M8, locked decision: "Separate
// process from the game runtime, IPC between them"). Scope for this
// increment: the one-shot launch handoff -- the UI process (which owns
// the real login flow and the real session cookie, see login_window.h/
// credential_store.h) needs to hand that cookie, and a couple of
// related config values, to the runtime process at the moment it's
// spawned.
//
// Plain POSIX Unix domain sockets, no Qt dependency on either side --
// the runtime process doesn't link Qt at all (locked architecture), so
// this can't be QLocalSocket-based without giving runtime a Qt
// dependency it doesn't otherwise need. A real, ephemeral,
// $XDG_RUNTIME_DIR-based socket path (same convention Wayland's own
// socket uses) rather than a well-known filesystem path, so it's
// automatically user-scoped and cleaned up with the session.
//
// Deliberately one-shot, not a general bidirectional protocol: this
// increment's real, concrete need is "hand the cookie over once, at
// launch." A persistent channel for later lifecycle messages (window
// focus, Discord RPC, crash notification -- M10 territory) is a real,
// separate future increment, not built ahead of a concrete need for it.

namespace stud::ipc {

struct LaunchPayload {
    std::string session_cookie;
    std::string base_url = "https://www.roblox.com";
    std::string api_base_url = "https://apis.roblox.com";
    // Present when the UI process was invoked via a real
    // roblox-player:/roblox: launch URI (see ui/src/launch_uri.h) --
    // empty otherwise (e.g. a normal, non-magnet-link launch). The real
    // per-game auth ticket and join-info URL, once M9 can actually use
    // them to join a specific game rather than just boot the engine.
    std::string game_info;
    std::string place_launcher_url;
    // Real fields parsed out of place_launcher_url's own query string
    // (see ui/src/launch_uri.h's own doc comment) -- map directly onto
    // real StartGameParams fields (start_game_params.h, traced through the app's own code).
    // Zero/empty if there was no place_launcher_url or it didn't carry
    // that field.
    long long deep_link_place_id = 0;
    std::string deep_link_join_attempt_id;
    long long deep_link_referred_by_player_id = 0;
    std::string deep_link_join_attempt_origin;
    std::string deep_link_game_instance_id;
    // Real ClientSettings content (see jni-bridge/include/stud/
    // client_settings_bridge.h's own doc comment): a real device's Java
    // side fetches this over HTTP itself and hands the already-fetched
    // body to nativeInitClientSettings() -- Stud has no Java layer, so
    // the UI process (a real glibc process already making other HTTP-
    // adjacent calls, e.g. the login flow) fetches it here, once, before
    // the runtime process starts, and hands it over via this same
    // one-shot payload rather than standing up a whole separate IPC
    // protocol for one fixed-URL GET. Empty/zero if the fetch failed --
    // the runtime process treats that as "skip client settings," not a
    // hard failure.
    // Whether the desktop is in dark mode, read by the UI process --
    // the one with a real desktop connection -- and handed over here.
    //
    // The engine asks the platform for this through the real system-theme
    // protocol (see system_theme_bridge.h) and uses it to pick the app's
    // theme and the theme of the Roblox pages it opens in a web view.
    // Stud answered "error, no context" before, so the app fell back to
    // light whatever the desktop was set to.
    bool system_dark_mode = false;

    std::string client_settings_body;
    long client_settings_http_status = 0;

    // Real, currently-authenticated user identity (Roblox's own public
    // `users.roblox.com/v1/users/authenticated` endpoint, given a valid
    // .ROBLOSECURITY cookie) -- fetched once, here, by the UI process,
    // same pattern as client_settings_body above. Investigated this
    // session as the likely real fix for a native crash traced
    // to UserController::didLogin() dereferencing a null singleton:
    // Stud's own NativeUserJavaInterface stub previously always
    // returned a placeholder userId of 0/empty username regardless of
    // whatever real cookie was supplied, which real native code is very
    // plausibly reading as "not logged in" and never constructing
    // UserController as a result. Zero/empty if the fetch failed or no
    // cookie was available -- the runtime process degrades the same way
    // client_settings_body's own absence already does, not a hard
    // failure.
    long long authenticated_user_id = 0;
    std::string authenticated_username;
    std::string authenticated_display_name;

    // Real, raw response body from GET-ing place_launcher_url above (with
    // the real session cookie) -- Roblox's own public, real,
    // community-documented PlaceLauncher.ashx?request=RequestGame
    // endpoint, the same one third-party Roblox launchers use to turn a
    // deep-link's opaque join ticket into real server-join info (place,
    // access ticket, server address). Deliberately NOT parsed into
    // individual fields yet: this project has no live-captured real
    // response to ground-truth the current exact JSON schema against
    // (Roblox has changed it before), so this is evidence-gathering --
    // real, honest raw text, logged and carried through so a real
    // response can actually be inspected -- not a guessed field mapping
    // into StartGameParams that could silently be wrong. Empty if there
    // was no place_launcher_url, the fetch failed, or no cookie was
    // available, same honest-degradation pattern as client_settings_body.
    std::string place_launcher_response;
};

class IpcError : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

// $XDG_RUNTIME_DIR/stud/launch.sock, or $TMPDIR-adjacent fallback if
// XDG_RUNTIME_DIR isn't set (matches how every other Stud path helper
// in this codebase degrades, e.g. stud::config::default_config_path()).
std::string default_socket_path();

// Server side (UI process). Binds and listens on `socket_path` (creating
// its parent directory if needed), waits up to `timeout_ms` for exactly
// one client to connect, sends `payload` as JSON, then closes both the
// accepted connection and the listening socket. Throws IpcError if the
// socket can't be bound, or if no client connects within the timeout
// (the caller -- the UI process, right after spawning the runtime
// process -- can then report a clear "runtime process failed to start"
// error instead of hanging forever).
void serve_launch_payload_once(const std::string& socket_path, const LaunchPayload& payload,
                                int timeout_ms = 10000);

// Client side (runtime process). Connects to `socket_path`, reads the
// JSON payload, parses it. Throws IpcError on any failure (no socket,
// connection refused, malformed payload).
LaunchPayload receive_launch_payload(const std::string& socket_path);

}  // namespace stud::ipc
