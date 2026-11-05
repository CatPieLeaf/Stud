#pragma once

#include <optional>
#include <string>

namespace stud::web {

// Signing in happens on Roblox's own website, in Stud's own viewer: the app's
// login screen depends on the Roblox version, the website does not. The viewer
// is told by this word in place of a user agent, runs as a plain browser, and
// reports the session it ends up with on stdout as "session <secret> <value>".
inline constexpr char kLoginPageUrl[] = "https://www.roblox.com/login";
inline constexpr char kSignUpPageUrl[] = "https://www.roblox.com/CreateAccount";
inline constexpr char kLoginRequest[] = "login";
// The same, ready and hidden, waiting for "load <url>" on its stdin: starting a
// viewer takes long enough to see, so it is started before it is needed.
inline constexpr char kLoginPreparedRequest[] = "login-hidden";
inline constexpr char kLoginSessionPrefix[] = "session ";

struct LoginField {
    std::string secret;
    std::string value;
};

// One reported field, after the prefix. Only these two names exist, so a viewer
// can never choose what else gets written to Stud's storage.
inline std::optional<LoginField> parse_login_field(const std::string& field) {
    const auto space = field.find(' ');
    if (space == std::string::npos || space + 1 == field.size()) return std::nullopt;
    LoginField parsed{field.substr(0, space), field.substr(space + 1)};
    if (parsed.secret != "roblosecurity" && parsed.secret != "rbxas") return std::nullopt;
    return parsed;
}

// Whether a URL is served by Roblox itself, over TLS. This is what stands
// between the signed-in session and any other site, so anything that does
// not read cleanly as a plain host is a no: the authority ends where a
// browser ends it, backslash included, and the host is what follows any
// userinfo, which is how https://roblox.com@example.com reaches example.com.
inline bool is_roblox_https_url(const std::string& url) {
    const std::string scheme = "https://";
    if (url.rfind(scheme, 0) != 0) return false;
    const auto end = url.find_first_of("/\\?#", scheme.size());
    std::string host = url.substr(scheme.size(),
                                  end == std::string::npos ? end : end - scheme.size());
    if (const auto at = host.rfind('@'); at != std::string::npos) host.erase(0, at + 1);
    if (const auto port = host.find(':'); port != std::string::npos) host.resize(port);
    for (char& c : host) {
        if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
        const bool plain = (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-' || c == '.';
        if (!plain) return false;
    }
    const std::string domain = "roblox.com";
    if (host == domain) return true;
    const std::string dotted = "." + domain;
    return host.size() > dotted.size() &&
           host.compare(host.size() - dotted.size(), dotted.size(), dotted) == 0;
}

}  // namespace stud::web
