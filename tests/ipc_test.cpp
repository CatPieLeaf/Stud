// M8 test: stud-ipc's real Unix-domain-socket launch handoff (see
// stud-ipc/include/stud/ipc.h). Real loopback: a server thread and a
// client, communicating over an actual bound-and-listening socket, not
// mocked.

#include "stud/ipc.h"

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <thread>

namespace {

void check(bool condition, const char* what) {
    if (!condition) {
        std::fprintf(stderr, "FAILED: %s\n", what);
        std::exit(1);
    }
    std::printf("ok: %s\n", what);
}

}  // namespace

int main(int argc, char** argv) {
    if (argc != 2) {
        std::fprintf(stderr, "usage: %s <scratch-dir>\n", argv[0]);
        return 1;
    }
    std::string socket_path = std::string(argv[1]) + "/launch.sock";

    // No server listening yet, connect fails clearly.
    bool threw = false;
    try {
        stud::ipc::receive_launch_payload(socket_path);
    } catch (const stud::ipc::IpcError&) {
        threw = true;
    }
    check(threw, "connecting with no server listening throws IpcError instead of hanging");

    // Real end-to-end handoff: a real server thread accepts a real
    // client connection and sends a real payload.
    stud::ipc::LaunchPayload sent;
    sent.session_cookie = "_|WARNING:-DO-NOT-SHARE-THIS-fake-test-cookie_";
    sent.base_url = "https://www.roblox.com";
    sent.api_base_url = "https://apis.roblox.com";
    sent.game_info = "FAKE_AUTH_TICKET_VALUE";
    sent.client_settings_body = R"({"applicationSettings":{"FakeFlag":"true"}})";
    sent.client_settings_http_status = 200;
    sent.deep_link_place_id = 1818;
    sent.deep_link_join_attempt_id = "f923ce81-874a-4ce8-af0d-bc365d3a2153";
    sent.deep_link_referred_by_player_id = 42;
    sent.deep_link_join_attempt_origin = "PlayButton";

    std::thread server_thread(
        [&]() { stud::ipc::serve_launch_payload_once(socket_path, sent, /*timeout_ms=*/5000); });

    // Real race, resolved the honest way: the client may attempt to
    // connect before the server has bound+listened yet, retry briefly
    // instead of an arbitrary fixed sleep guess.
    stud::ipc::LaunchPayload received;
    bool connected = false;
    for (int attempt = 0; attempt < 50 && !connected; ++attempt) {
        try {
            received = stud::ipc::receive_launch_payload(socket_path);
            connected = true;
        } catch (const stud::ipc::IpcError&) {
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
        }
    }
    server_thread.join();

    check(connected, "client eventually connects to the real server");
    check(received.session_cookie == sent.session_cookie,
          "session cookie round-trips exactly over the real socket");
    check(received.base_url == sent.base_url, "base URL round-trips exactly");
    check(received.api_base_url == sent.api_base_url, "API base URL round-trips exactly");
    check(received.game_info == sent.game_info,
          "gameInfo (the real per-launch auth ticket) round-trips exactly");
    check(received.client_settings_body == sent.client_settings_body,
          "clientSettingsBody (the UI process's own pre-fetched ClientSettings content) round-trips "
          "exactly");
    check(received.client_settings_http_status == sent.client_settings_http_status,
          "clientSettingsHttpStatus round-trips exactly");
    check(received.deep_link_place_id == sent.deep_link_place_id,
          "deepLinkPlaceId round-trips exactly");
    check(received.deep_link_join_attempt_id == sent.deep_link_join_attempt_id,
          "deepLinkJoinAttemptId round-trips exactly");
    check(received.deep_link_referred_by_player_id == sent.deep_link_referred_by_player_id,
          "deepLinkReferredByPlayerId round-trips exactly");
    check(received.deep_link_join_attempt_origin == sent.deep_link_join_attempt_origin,
          "deepLinkJoinAttemptOrigin round-trips exactly");

    // Bytes that are not UTF-8: a settings body is whatever the network sent,
    // and this runs on a thread nothing else would catch a throw on.
    stud::ipc::LaunchPayload invalid = sent;
    invalid.client_settings_body = "before \xff after";
    std::thread invalid_server(
        [&]() { stud::ipc::serve_launch_payload_once(socket_path, invalid, /*timeout_ms=*/5000); });
    stud::ipc::LaunchPayload repaired;
    connected = false;
    for (int attempt = 0; attempt < 50 && !connected; ++attempt) {
        try {
            repaired = stud::ipc::receive_launch_payload(socket_path);
            connected = true;
        } catch (const stud::ipc::IpcError&) {
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
        }
    }
    invalid_server.join();
    check(connected && repaired.session_cookie == sent.session_cookie,
          "a payload with invalid UTF-8 still arrives");

    // Timeout path: a server with no client ever connecting throws
    // IpcError instead of hanging forever.
    bool timed_out = false;
    auto start = std::chrono::steady_clock::now();
    try {
        stud::ipc::serve_launch_payload_once(socket_path, sent, /*timeout_ms=*/300);
    } catch (const stud::ipc::IpcError&) {
        timed_out = true;
    }
    auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() -
                                                                          start)
                       .count();
    check(timed_out, "serve_launch_payload_once throws IpcError when no client ever connects");
    check(elapsed >= 300 && elapsed < 5000,
          "the timeout is real, not instant and not hanging (took ~300ms as configured)");

    std::printf("all ipc checks passed\n");
    return 0;
}
