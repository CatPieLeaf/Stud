#include "stud/discord_rpc.h"

#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

#include <cstdio>
#include <cstdlib>
#include <mutex>
#include <string>

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

std::string runtime_dir() {
    if (const char* xdg = std::getenv("XDG_RUNTIME_DIR")) return xdg;
    return "/tmp";
}

bool send_frame(uint32_t opcode, const std::string& payload) {
    if (g_fd < 0) return false;
    uint32_t header[2] = {opcode, static_cast<uint32_t>(payload.size())};
    if (::send(g_fd, header, sizeof(header), MSG_NOSIGNAL) != sizeof(header)) return false;
    const ssize_t n = ::send(g_fd, payload.data(), payload.size(), MSG_NOSIGNAL);
    return n == static_cast<ssize_t>(payload.size());
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
}

// Discord listens on discord-ipc-0 through discord-ipc-9; a client tries
// each in turn, because several Discord builds can run at once.
bool connect_locked() {
    if (g_fd >= 0) return true;
    for (int i = 0; i < 10; ++i) {
        const std::string path = runtime_dir() + "/discord-ipc-" + std::to_string(i);
        const int fd = ::socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
        if (fd < 0) return false;
        sockaddr_un addr{};
        addr.sun_family = AF_UNIX;
        std::snprintf(addr.sun_path, sizeof(addr.sun_path), "%s", path.c_str());
        if (::connect(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) == 0) {
            g_fd = fd;
            const std::string handshake =
                "{\"v\":1,\"client_id\":\"" + escape(g_application_id) + "\"}";
            if (!send_frame(kOpHandshake, handshake)) {
                close_connection();
                return false;
            }
            std::printf("stud-render-host: Discord rich presence connected\n");
            std::fflush(stdout);
            return true;
        }
        ::close(fd);
    }
    return false;
}

}  // namespace

void discord_rpc_start(const std::string& application_id) {
    std::lock_guard<std::mutex> lock(g_mutex);
    g_application_id = application_id;
    if (g_application_id.empty()) return;
    // Discord not running is an ordinary condition, not an error: the
    // connection is retried the next time the presence changes.
    connect_locked();
}

void discord_rpc_set_game(const GamePresence& presence) {
    std::lock_guard<std::mutex> lock(g_mutex);
    if (g_application_id.empty()) return;
    if (!connect_locked()) return;

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
        // Discord went away. Drop the connection so the next update
        // reconnects rather than writing into a dead socket forever.
        close_connection();
    }
}

void discord_rpc_stop() {
    std::lock_guard<std::mutex> lock(g_mutex);
    if (g_fd < 0) return;
    const std::string payload = "{\"cmd\":\"SET_ACTIVITY\",\"args\":{\"pid\":" +
                                 std::to_string(::getpid()) + "},\"nonce\":\"" +
                                 std::to_string(++g_nonce) + "\"}";
    send_frame(kOpFrame, payload);
    close_connection();
}

}  // namespace stud::render_host
