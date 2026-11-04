#pragma once

#include <string>

namespace stud::web {

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
