#include "deep_link_handoff.h"

#include <cstdio>

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
