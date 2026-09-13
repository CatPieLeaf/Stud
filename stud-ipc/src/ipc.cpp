#include "stud/ipc.h"

#include <nlohmann/json.hpp>

#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <poll.h>
#include <unistd.h>

#include <cerrno>
#include <cstdlib>
#include <cstring>

namespace stud::ipc {

namespace {

std::string parent_directory(const std::string& path) {
    size_t pos = path.find_last_of('/');
    return pos == std::string::npos ? std::string() : path.substr(0, pos);
}

void make_directories(const std::string& path) {
    std::string current;
    for (char c : path) {
        if (c == '/' && !current.empty()) {
            ::mkdir(current.c_str(), 0700);
        }
        current += c;
    }
    if (!current.empty()) {
        ::mkdir(current.c_str(), 0700);
    }
}

nlohmann::json to_json(const LaunchPayload& payload) {
    return {
        {"sessionCookie", payload.session_cookie},
        {"systemDarkMode", payload.system_dark_mode},
        {"baseUrl", payload.base_url},
        {"apiBaseUrl", payload.api_base_url},
        {"gameInfo", payload.game_info},
        {"placeLauncherUrl", payload.place_launcher_url},
        {"clientSettingsBody", payload.client_settings_body},
        {"clientSettingsHttpStatus", payload.client_settings_http_status},
        {"authenticatedUserId", payload.authenticated_user_id},
        {"authenticatedUsername", payload.authenticated_username},
        {"authenticatedDisplayName", payload.authenticated_display_name},
        {"placeLauncherResponse", payload.place_launcher_response},
        {"deepLinkPlaceId", payload.deep_link_place_id},
        {"deepLinkJoinAttemptId", payload.deep_link_join_attempt_id},
        {"deepLinkReferredByPlayerId", payload.deep_link_referred_by_player_id},
        {"deepLinkJoinAttemptOrigin", payload.deep_link_join_attempt_origin},
        {"deepLinkGameInstanceId", payload.deep_link_game_instance_id},
    };
}

LaunchPayload from_json(const nlohmann::json& doc) {
    LaunchPayload payload;
    if (doc.contains("sessionCookie") && doc.at("sessionCookie").is_string()) {
        payload.session_cookie = doc.at("sessionCookie").get<std::string>();
        if (doc.contains("systemDarkMode")) {
            payload.system_dark_mode = doc.at("systemDarkMode").get<bool>();
        }
    }
    if (doc.contains("baseUrl") && doc.at("baseUrl").is_string()) {
        payload.base_url = doc.at("baseUrl").get<std::string>();
    }
    if (doc.contains("apiBaseUrl") && doc.at("apiBaseUrl").is_string()) {
        payload.api_base_url = doc.at("apiBaseUrl").get<std::string>();
    }
    if (doc.contains("gameInfo") && doc.at("gameInfo").is_string()) {
        payload.game_info = doc.at("gameInfo").get<std::string>();
    }
    if (doc.contains("placeLauncherUrl") && doc.at("placeLauncherUrl").is_string()) {
        payload.place_launcher_url = doc.at("placeLauncherUrl").get<std::string>();
    }
    if (doc.contains("clientSettingsBody") && doc.at("clientSettingsBody").is_string()) {
        payload.client_settings_body = doc.at("clientSettingsBody").get<std::string>();
    }
    if (doc.contains("clientSettingsHttpStatus") && doc.at("clientSettingsHttpStatus").is_number()) {
        payload.client_settings_http_status = doc.at("clientSettingsHttpStatus").get<long>();
    }
    if (doc.contains("authenticatedUserId") && doc.at("authenticatedUserId").is_number()) {
        payload.authenticated_user_id = doc.at("authenticatedUserId").get<long long>();
    }
    if (doc.contains("authenticatedUsername") && doc.at("authenticatedUsername").is_string()) {
        payload.authenticated_username = doc.at("authenticatedUsername").get<std::string>();
    }
    if (doc.contains("authenticatedDisplayName") && doc.at("authenticatedDisplayName").is_string()) {
        payload.authenticated_display_name = doc.at("authenticatedDisplayName").get<std::string>();
    }
    if (doc.contains("placeLauncherResponse") && doc.at("placeLauncherResponse").is_string()) {
        payload.place_launcher_response = doc.at("placeLauncherResponse").get<std::string>();
    }
    if (doc.contains("deepLinkPlaceId") && doc.at("deepLinkPlaceId").is_number()) {
        payload.deep_link_place_id = doc.at("deepLinkPlaceId").get<long long>();
    }
    if (doc.contains("deepLinkJoinAttemptId") && doc.at("deepLinkJoinAttemptId").is_string()) {
        payload.deep_link_join_attempt_id = doc.at("deepLinkJoinAttemptId").get<std::string>();
    }
    if (doc.contains("deepLinkReferredByPlayerId") && doc.at("deepLinkReferredByPlayerId").is_number()) {
        payload.deep_link_referred_by_player_id = doc.at("deepLinkReferredByPlayerId").get<long long>();
    }
    if (doc.contains("deepLinkGameInstanceId") && doc.at("deepLinkGameInstanceId").is_string()) {
        payload.deep_link_game_instance_id = doc.at("deepLinkGameInstanceId").get<std::string>();
    }
    if (doc.contains("deepLinkJoinAttemptOrigin") && doc.at("deepLinkJoinAttemptOrigin").is_string()) {
        payload.deep_link_join_attempt_origin = doc.at("deepLinkJoinAttemptOrigin").get<std::string>();
    }
    return payload;
}

// Fills `addr` for a real AF_UNIX socket at `path`. Throws IpcError if
// `path` is too long for sockaddr_un's fixed-size buffer (a real,
// standard Unix domain socket constraint, ~108 bytes on Linux) --
// caught here explicitly rather than silently truncating a path.
sockaddr_un make_sockaddr(const std::string& path) {
    sockaddr_un addr{};
    addr.sun_family = AF_UNIX;
    if (path.size() >= sizeof(addr.sun_path)) {
        throw IpcError("stud::ipc: socket path too long: " + path);
    }
    std::memcpy(addr.sun_path, path.c_str(), path.size() + 1);
    return addr;
}

}  // namespace

std::string default_socket_path() {
    if (const char* xdg_runtime_dir = std::getenv("XDG_RUNTIME_DIR")) {
        return std::string(xdg_runtime_dir) + "/stud/launch.sock";
    }
    return "/tmp/stud-" + std::to_string(::getuid()) + "/launch.sock";
}

void serve_launch_payload_once(const std::string& socket_path, const LaunchPayload& payload,
                                int timeout_ms) {
    make_directories(parent_directory(socket_path));
    // A stale socket file from a previous, uncleanly-terminated run
    // would make bind() fail with EADDRINUSE -- remove it first. Not an
    // error if it doesn't exist.
    ::unlink(socket_path.c_str());

    int listen_fd = ::socket(AF_UNIX, SOCK_STREAM, 0);
    if (listen_fd < 0) {
        throw IpcError(std::string("stud::ipc: socket() failed: ") + std::strerror(errno));
    }

    sockaddr_un addr = make_sockaddr(socket_path);
    if (::bind(listen_fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) {
        std::string error = std::strerror(errno);
        ::close(listen_fd);
        throw IpcError("stud::ipc: bind(" + socket_path + ") failed: " + error);
    }
    // Owner-only -- this socket carries a real session cookie.
    ::chmod(socket_path.c_str(), 0600);

    if (::listen(listen_fd, 1) != 0) {
        std::string error = std::strerror(errno);
        ::close(listen_fd);
        ::unlink(socket_path.c_str());
        throw IpcError("stud::ipc: listen() failed: " + error);
    }

    pollfd pfd{listen_fd, POLLIN, 0};
    int poll_result = ::poll(&pfd, 1, timeout_ms);
    if (poll_result <= 0) {
        ::close(listen_fd);
        ::unlink(socket_path.c_str());
        throw IpcError("stud::ipc: no client connected within " + std::to_string(timeout_ms) +
                        "ms (runtime process failed to start or connect)");
    }

    int conn_fd = ::accept(listen_fd, nullptr, nullptr);
    ::close(listen_fd);
    ::unlink(socket_path.c_str());
    if (conn_fd < 0) {
        throw IpcError(std::string("stud::ipc: accept() failed: ") + std::strerror(errno));
    }

    std::string body = to_json(payload).dump();
    size_t total_written = 0;
    while (total_written < body.size()) {
        ssize_t written = ::write(conn_fd, body.data() + total_written, body.size() - total_written);
        if (written <= 0) {
            ::close(conn_fd);
            throw IpcError(std::string("stud::ipc: write() failed: ") + std::strerror(errno));
        }
        total_written += static_cast<size_t>(written);
    }
    // Shutdown, not just close -- signals real EOF to the client's
    // blocking read loop below rather than leaving it to time out.
    ::shutdown(conn_fd, SHUT_WR);
    ::close(conn_fd);
}

LaunchPayload receive_launch_payload(const std::string& socket_path) {
    int fd = ::socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) {
        throw IpcError(std::string("stud::ipc: socket() failed: ") + std::strerror(errno));
    }

    sockaddr_un addr = make_sockaddr(socket_path);
    if (::connect(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) {
        std::string error = std::strerror(errno);
        ::close(fd);
        throw IpcError("stud::ipc: connect(" + socket_path + ") failed: " + error);
    }

    std::string body;
    char buf[4096];
    ssize_t bytes_read;
    while ((bytes_read = ::read(fd, buf, sizeof(buf))) > 0) {
        body.append(buf, static_cast<size_t>(bytes_read));
    }
    ::close(fd);

    if (bytes_read < 0) {
        throw IpcError(std::string("stud::ipc: read() failed: ") + std::strerror(errno));
    }

    nlohmann::json doc;
    try {
        doc = nlohmann::json::parse(body);
    } catch (const nlohmann::json::parse_error& e) {
        throw IpcError(std::string("stud::ipc: malformed launch payload: ") + e.what());
    }
    if (!doc.is_object()) {
        throw IpcError("stud::ipc: launch payload must be a JSON object");
    }
    return from_json(doc);
}

}  // namespace stud::ipc
