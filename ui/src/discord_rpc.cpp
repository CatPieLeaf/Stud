#include "discord_rpc.h"

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

namespace stud::ui {

namespace {

// Framing: little-endian opcode and length, then the payload. 0 handshake, 1 frame.
constexpr uint32_t kOpHandshake = 0;
constexpr uint32_t kOpFrame = 1;

int g_fd = -1;
std::string g_application_id;
std::mutex g_mutex;
int64_t g_nonce = 0;
// Kept so a Discord that starts later gets it too.
DiscordActivity g_presence;
bool g_has_presence = false;

std::string runtime_dir() {
    if (const char* xdg = std::getenv("XDG_RUNTIME_DIR")) return xdg;
    return "/tmp";
}

constexpr uint32_t kOpClose = 2;
std::string g_inbox;

// Reads pending replies without waiting, so they don't pile up, and reports the
// CLOSE frame Discord sends for a bad handshake. False when Discord closed.
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
            std::printf("stud: Discord closed rich presence: %s\n",
                        g_inbox.substr(8, header[1]).c_str());
            std::fflush(stdout);
            return false;
        }
        g_inbox.erase(0, 8 + static_cast<size_t>(header[1]));
    }
    return true;
}

// Non-blocking, so a Discord that stopped reading costs this update, not the thread.
bool send_frame(uint32_t opcode, const std::string& payload) {
    if (g_fd < 0 || !drain_replies()) return false;
    uint32_t header[2] = {opcode, static_cast<uint32_t>(payload.size())};
    std::string frame(reinterpret_cast<const char*>(header), sizeof(header));
    frame += payload;
    const ssize_t n = ::send(g_fd, frame.data(), frame.size(), MSG_NOSIGNAL);
    return n == static_cast<ssize_t>(frame.size());
}

// Game and creator names can contain quotes and backslashes.
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

// discord-ipc-0 to 9, since several Discord builds can run at once; also inside
// the Flatpak (app/<id>/) and Snap (snap.discord/) directories.
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
            std::printf("stud: Discord rich presence connected: %s\n", path.c_str());
            std::fflush(stdout);
            return true;
        }
        ::close(fd);
    }
    return false;
}

void send_presence_locked() {
    const DiscordActivity& a = g_presence;
    std::string activity = "{";
    const auto field = [&activity](const char* key, const std::string& value) {
        if (value.empty()) return;
        if (activity.size() > 1) activity += ",";
        activity += "\"" + std::string(key) + "\":\"" + escape(value) + "\"";
    };
    field("details", a.details);
    field("state", a.state);
    std::string assets;
    const auto asset = [&assets](const char* key, const std::string& value) {
        if (value.empty()) return;
        if (!assets.empty()) assets += ",";
        assets += "\"" + std::string(key) + "\":\"" + escape(value) + "\"";
    };
    asset("large_image", a.large_image);
    asset("large_text", a.large_text);
    asset("small_image", a.small_image);
    asset("small_text", a.small_text);
    if (!assets.empty()) activity += std::string(activity.size() > 1 ? "," : "") + "\"assets\":{" + assets + "}";
    if (a.start > 0 || a.end > 0) {
        std::string timestamps;
        if (a.start > 0) timestamps += "\"start\":" + std::to_string(a.start);
        if (a.end > 0) timestamps += std::string(timestamps.empty() ? "" : ",") + "\"end\":" + std::to_string(a.end);
        activity += std::string(activity.size() > 1 ? "," : "") + "\"timestamps\":{" + timestamps + "}";
    }
    if (a.status_display >= 0) {
        activity += std::string(activity.size() > 1 ? "," : "") + "\"status_display_type\":" +
                    std::to_string(a.status_display);
    }
    if (!a.buttons.empty()) {
        std::string buttons;
        for (size_t i = 0; i < a.buttons.size() && i < 2; ++i) {
            if (i > 0) buttons += ",";
            buttons += "{\"label\":\"" + escape(a.buttons[i].first) + "\",\"url\":\"" +
                       escape(a.buttons[i].second) + "\"}";
        }
        activity += std::string(activity.size() > 1 ? "," : "") + "\"buttons\":[" + buttons + "]";
    }
    activity += "}";

    const std::string payload = "{\"cmd\":\"SET_ACTIVITY\",\"args\":{\"pid\":" +
                                 std::to_string(::getpid()) + ",\"activity\":" + activity +
                                 "},\"nonce\":\"" + std::to_string(++g_nonce) + "\"}";
    if (!send_frame(kOpFrame, payload)) {
        // Discord went away; keep_connected() reconnects.
        close_connection();
    }
}

// Discord can start after Stud or restart (Equibop and Vesktop replace their socket
// each time), so the connection is watched and the presence resent on reconnect.
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
    // Discord not running is normal.
    connect_locked();
    std::thread(keep_connected).detach();
}

void discord_rpc_set(const DiscordActivity& presence) {
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

}  // namespace stud::ui
