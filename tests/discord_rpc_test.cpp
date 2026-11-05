// A Discord that shows up after Stud, or restarts under it, must still get
// the presence, with nothing changing on Stud's side.

#include <poll.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

#include <cstdlib>
#include <filesystem>
#include <string>

#include "check.h"
#include "stud/discord_rpc.h"

namespace {

// Accepts one client and returns everything it sends until the presence
// arrives, or an empty string if it never does.
std::string serve_once(const std::string& path) {
    ::unlink(path.c_str());
    const int server = ::socket(AF_UNIX, SOCK_STREAM, 0);
    sockaddr_un addr{};
    addr.sun_family = AF_UNIX;
    std::snprintf(addr.sun_path, sizeof(addr.sun_path), "%s", path.c_str());
    check(::bind(server, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) == 0, "fake Discord binds");
    ::listen(server, 1);

    constexpr int kWaitMs = 15000;
    std::string received;
    pollfd pfd{server, POLLIN, 0};
    if (::poll(&pfd, 1, kWaitMs) == 1) {
        const int client = ::accept(server, nullptr, nullptr);
        pollfd cfd{client, POLLIN, 0};
        char buf[4096];
        while (received.find("SET_ACTIVITY") == std::string::npos && ::poll(&cfd, 1, kWaitMs) == 1) {
            const ssize_t n = ::recv(client, buf, sizeof(buf), 0);
            if (n <= 0) break;
            received.append(buf, static_cast<size_t>(n));
        }
        ::close(client);
    }
    ::close(server);
    ::unlink(path.c_str());
    return received;
}

}  // namespace

int main() {
    using namespace stud::render_host;
    // Under /tmp, not the build tree: a socket path has to fit sun_path.
    char dir_name[] = "/tmp/stud-rpc-XXXXXX";
    check(::mkdtemp(dir_name) != nullptr, "scratch folder");
    const std::filesystem::path dir = dir_name;
    ::setenv("XDG_RUNTIME_DIR", dir_name, 1);
    const std::string socket_path = (dir / "discord-ipc-0").string();

    discord_rpc_start("123");
    GamePresence game;
    game.universe_name = "Late Discord";
    game.started_at = 1793000000;
    discord_rpc_set_game(game);

    const std::string first = serve_once(socket_path);
    check(first.find("\"client_id\":\"123\"") != std::string::npos, "a late Discord gets the handshake");
    check(first.find("Late Discord") != std::string::npos, "a late Discord gets the presence");

    const std::string second = serve_once(socket_path);
    check(second.find("Late Discord") != std::string::npos, "a restarted Discord gets it again");
    check(second.find("\"start\":1793000000") != std::string::npos, "the elapsed time carries on");

    discord_rpc_stop();
    std::filesystem::remove_all(dir);
    return 0;
}
