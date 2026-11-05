#include "stud/discord_rpc.h"

#include <fcntl.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

#include <cerrno>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace stud::render_host {

namespace {

// Discord's IPC framing: a little-endian opcode and payload length, then
// the payload. Opcode 0 is the handshake, 1 an ordinary frame.
constexpr uint32_t kOpHandshake = 0;
constexpr uint32_t kOpFrame = 1;

int g_fd = -1;
std::string g_application_id;
std::mutex g_mutex;
int64_t g_nonce = 0;
// What should be showing, so a Discord that appears later gets it too.
GamePresence g_presence;
bool g_has_presence = false;

std::string runtime_dir() {
    if (const char* xdg = std::getenv("XDG_RUNTIME_DIR")) return xdg;
    return "/tmp";
}

constexpr uint32_t kOpClose = 2;
std::string g_inbox;

// Reads whatever Discord has answered so far, without waiting. Every
// handshake and SET_ACTIVITY gets a reply; never reading them let them
// pile up for the whole session, and hid the one that matters: Discord
// refuses a bad handshake with a CLOSE frame, which is said out loud
// here. False when Discord has closed the connection.
bool drain_replies() {
    char buf[4096];
    for (;;) {
        const ssize_t n = ::recv(g_fd, buf, sizeof(buf), MSG_DONTWAIT);
        if (n > 0) {
            g_inbox.append(buf, static_cast<size_t>(n));
            continue;
        }
        if (n == 0) return false;
        if (errno == EINTR) continue;
        if (errno == EAGAIN || errno == EWOULDBLOCK) break;
        return false;
    }
    while (g_inbox.size() >= 8) {
        uint32_t header[2];
        std::memcpy(header, g_inbox.data(), sizeof(header));
        if (g_inbox.size() - 8 < header[1]) break;
        if (header[0] == kOpClose) {
            std::printf("stud-render-host: Discord closed rich presence: %s\n",
                        g_inbox.substr(8, header[1]).c_str());
            std::fflush(stdout);
            return false;
        }
        g_inbox.erase(0, 8 + static_cast<size_t>(header[1]));
    }
    return true;
}

// One write of the whole frame, never waiting: the socket is
// non-blocking, so a Discord that has stopped reading costs this update
// rather than the engine thread that asked for it.
bool send_frame(uint32_t opcode, const std::string& payload) {
    if (g_fd < 0 || !drain_replies()) return false;
    uint32_t header[2] = {opcode, static_cast<uint32_t>(payload.size())};
    std::string frame(reinterpret_cast<const char*>(header), sizeof(header));
    frame += payload;
    const ssize_t n = ::send(g_fd, frame.data(), frame.size(), MSG_NOSIGNAL);
    return n == static_cast<ssize_t>(frame.size());
}

// Minimal JSON string escaping. Presence text is a game name and a
// creator name, both of which really can contain quotes and backslashes.
std::string escape(const std::string& s) {
    std::string out;
    out.reserve(s.size() + 8);
    for (const char c : s) {
        switch (c) {
            case '"': out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            case '\t': out += "\\t"; break;
            default:
                if (static_cast<unsigned char>(c) < 0x20) {
                    char buf[8];
                    std::snprintf(buf, sizeof(buf), "\\u%04x", c);
                    out += buf;
                } else {
                    out += c;
                }
        }
    }
    return out;
}

void close_connection() {
    if (g_fd >= 0) {
        ::close(g_fd);
        g_fd = -1;
    }
    g_inbox.clear();
}

// Discord listens on discord-ipc-0 through discord-ipc-9; a client tries
// each in turn, because several Discord builds can run at once.
//
// A sandboxed Discord puts its socket in its own directory rather than
// at the top of the runtime dir, so those are tried too: the Flatpak
// builds under app/<id>/ and the Snap under snap.discord/. Only the top
// level used to be tried, so presence never connected to either.
bool connect_locked() {
    if (g_fd >= 0) return true;
    static const char* const kDirs[] = {"", "/app/com.discordapp.Discord",
                                        "/app/dev.vencord.Vesktop", "/snap.discord"};
    std::vector<std::string> paths;
    for (const char* dir : kDirs) {
        for (int i = 0; i < 10; ++i) {
            paths.push_back(runtime_dir() + dir + "/discord-ipc-" + std::to_string(i));
        }
    }
    for (const std::string& path : paths) {
        const int fd = ::socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
        if (fd < 0) return false;
        sockaddr_un addr{};
        addr.sun_family = AF_UNIX;
        std::snprintf(addr.sun_path, sizeof(addr.sun_path), "%s", path.c_str());
        if (::connect(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) == 0) {
            ::fcntl(fd, F_SETFL, ::fcntl(fd, F_GETFL) | O_NONBLOCK);
            g_fd = fd;
            const std::string handshake =
                "{\"v\":1,\"client_id\":\"" + escape(g_application_id) + "\"}";
            if (!send_frame(kOpHandshake, handshake)) {
                close_connection();
                return false;
            }
            std::printf("stud-render-host: Discord rich presence connected: %s\n", path.c_str());
            std::fflush(stdout);
            return true;
        }
        ::close(fd);
    }
    return false;
}

void send_presence_locked() {
    const GamePresence& presence = g_presence;
    std::string activity = "{";
    if (presence.universe_name.empty()) {
        // Not in an experience. The presence stays, showing Stud itself
        // rather than vanishing between games.
        activity += "\"details\":\"In the Roblox app\"";
        activity += ",\"assets\":{\"large_image\":\"stud\",\"large_text\":\"Stud\"}";
    } else {
        activity += "\"details\":\"" + escape(presence.universe_name) + "\"";
        if (!presence.creator_name.empty()) {
            activity += ",\"state\":\"by " + escape(presence.creator_name) + "\"";
        }
        activity += ",\"assets\":{";
        // Discord accepts an http(s) URL as an asset key, which is what
        // makes a per-game thumbnail possible without uploading anything
        // to the application beforehand.
        activity += "\"large_image\":\"" +
                    escape(presence.thumbnail_url.empty() ? "stud" : presence.thumbnail_url) + "\"";
        activity += ",\"large_text\":\"" + escape(presence.universe_name) + "\"";
        activity += ",\"small_image\":\"stud\",\"small_text\":\"Stud\"}";
        if (presence.started_at > 0) {
            activity += ",\"timestamps\":{\"start\":" + std::to_string(presence.started_at) + "}";
        }
        if (!presence.join_url.empty()) {
            activity += ",\"buttons\":[{\"label\":\"Join server\",\"url\":\"" +
                        escape(presence.join_url) + "\"}]";
        }
    }
    activity += "}";

    const std::string payload = "{\"cmd\":\"SET_ACTIVITY\",\"args\":{\"pid\":" +
                                 std::to_string(::getpid()) + ",\"activity\":" + activity +
                                 "},\"nonce\":\"" + std::to_string(++g_nonce) + "\"}";
    if (!send_frame(kOpFrame, payload)) {
        // Discord went away. Drop the connection so keep_connected()
        // reconnects rather than writing into a dead socket forever.
        close_connection();
    }
}

// Discord can open after Stud, and restart under it. Equibop and Vesktop do
// this the most: their RPC server lives and dies with the app and replaces
// its socket each time. Retrying only on a presence change left someone who
// stays in one game with nothing, so this watches the connection and shows
// the current presence on every new one.
void keep_connected() {
    constexpr auto kRetryInterval = std::chrono::seconds(5);
    for (;;) {
        std::this_thread::sleep_for(kRetryInterval);
        std::lock_guard<std::mutex> lock(g_mutex);
        if (g_application_id.empty()) return;
        if (g_fd >= 0 && drain_replies()) continue;
        close_connection();
        if (connect_locked() && g_has_presence) send_presence_locked();
    }
}

}  // namespace

void discord_rpc_start(const std::string& application_id) {
    std::lock_guard<std::mutex> lock(g_mutex);
    g_application_id = application_id;
    if (g_application_id.empty()) return;
    // Discord not running is an ordinary condition, not an error.
    connect_locked();
    std::thread(keep_connected).detach();
}

void discord_rpc_set_game(const GamePresence& presence) {
    std::lock_guard<std::mutex> lock(g_mutex);
    if (g_application_id.empty()) return;
    g_presence = presence;
    g_has_presence = true;
    if (connect_locked()) send_presence_locked();
}

void discord_rpc_stop() {
    std::lock_guard<std::mutex> lock(g_mutex);
    g_application_id.clear();
    if (g_fd < 0) return;
    const std::string payload = "{\"cmd\":\"SET_ACTIVITY\",\"args\":{\"pid\":" +
                                 std::to_string(::getpid()) + "},\"nonce\":\"" +
                                 std::to_string(++g_nonce) + "\"}";
    send_frame(kOpFrame, payload);
    close_connection();
}

}  // namespace stud::render_host
