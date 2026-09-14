#include "deep_link_handoff.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "stud/render_host_protocol.h"

namespace stud::ui {

bool hand_deep_link_to_running_stud(const LaunchUri& link) {
    if (link.place_id == 0) return false;
    // One key=value per line: small, obvious on the wire, and trivially
    // extended without a version field. Values never contain a newline --
    // they come out of a URI's own query string.
    std::string payload;
    payload += "placeId=" + std::to_string(link.place_id) + "\n";
    if (!link.join_attempt_id.empty()) payload += "joinAttemptId=" + link.join_attempt_id + "\n";
    if (link.referred_by_player_id != 0) {
        payload += "referredBy=" + std::to_string(link.referred_by_player_id) + "\n";
    }
    if (!link.join_attempt_origin.empty()) {
        payload += "joinOrigin=" + link.join_attempt_origin + "\n";
    }
    if (!link.game_info.empty()) payload += "gameInfo=" + link.game_info + "\n";
    if (!link.game_instance_id.empty()) {
        payload += "gameInstanceId=" + link.game_instance_id + "\n";
    }
    // The token that lets the running Stud come forward.
    //
    // A compositor will not let an application raise itself -- so the
    // only thing that can bring Stud to the front is a token minted by
    // the process the user actually clicked in. The browser (or the
    // portal) hands it to THIS process in the environment, and this
    // process is about to exit without ever showing a window, so passing
    // it on is the whole of its value. Without it the game would join
    // into a window still sitting behind the browser.
    //
    // Both spellings: XDG_ACTIVATION_TOKEN is the Wayland one,
    // DESKTOP_STARTUP_ID the older X11 name that launchers still set.
    const char* token = std::getenv("XDG_ACTIVATION_TOKEN");
    if (token == nullptr || *token == '\0') token = std::getenv("DESKTOP_STARTUP_ID");
    if (token != nullptr && *token != '\0' && std::strchr(token, '\n') == nullptr) {
        payload += std::string("activationToken=") + token + "\n";
    } else {
        // Worth saying: without it the game joins into a window that
        // stays behind whatever the link was clicked in, and nothing else
        // reports why. The usual cause is a launcher that was not asked
        // for startup notification (StartupNotify in the desktop entry),
        // or a direct invocation from a terminal, which has no token to
        // give.
        std::fprintf(stderr,
                     "stud: no activation token in the environment -- the running Stud will "
                     "join the game but stay in the background\n");
    }
    const std::string& uri = payload;
    stud::render_host::Client client;
    if (!client.connect_to(stud::render_host::default_socket_path())) {
        // No render-host: either nothing is running, or it is starting up.
        // Either way this launch has nothing to hand over to.
        return false;
    }
    uint64_t a[8] = {};
    const uint64_t accepted =
        client.call(stud::render_host::CallId::DeliverDeepLink, a, uri.data(),
                    static_cast<uint32_t>(uri.size()), nullptr, 0, nullptr);
    // The URI is never printed: it carries a one-time join ticket.
    if (accepted == 0) {
        std::fprintf(stderr, "stud: the running session did not accept the link\n");
        return false;
    }
    return true;
}

}  // namespace stud::ui
