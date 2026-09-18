#include "stud/webview_cookies.h"

#include <functional>
#include <map>
#include <mutex>

namespace stud::jni_bridge {

namespace {

std::function<void(const std::string&)>& session_cookie_sink() {
    static std::function<void(const std::string&)> f;
    return f;
}

std::function<void(const std::string&)>& cookie_cleared_sink() {
    static std::function<void(const std::string&)> sink;
    return sink;
}

std::function<void(const std::string&)>& account_list_cookie_sink() {
    static std::function<void(const std::string&)> f;
    return f;
}

std::mutex& current_mutex() {
    static std::mutex m;
    return m;
}

// name -> value, newest wins. Only the two cookies that carry session
// identity; everything else the engine sets is not Stud's business.
std::map<std::string, std::string>& current() {
    static std::map<std::string, std::string> m;
    return m;
}

std::string cookie_name(const std::string& header) {
    const auto eq = header.find('=');
    return eq == std::string::npos ? header : header.substr(0, eq);
}

// The value between "name=" and the first attribute separator.
std::string cookie_value(const std::string& header) {
    const auto eq = header.find('=');
    if (eq == std::string::npos) return {};
    const auto end = header.find(';', eq + 1);
    return header.substr(eq + 1, end == std::string::npos ? std::string::npos : end - (eq + 1));
}

// A deleted or emptied cookie. Storing one of these would overwrite a
// good session with nothing.
bool is_cleared(const std::string& value) {
    return value.empty() || value == "\"\"" || value == "deleted";
}

}  // namespace

void set_session_cookie_sink(std::function<void(const std::string&)> sink) {
    session_cookie_sink() = std::move(sink);
}

void set_cookie_cleared_sink(std::function<void(const std::string&)> sink) {
    cookie_cleared_sink() = std::move(sink);
}

void set_account_list_cookie_sink(std::function<void(const std::string&)> sink) {
    account_list_cookie_sink() = std::move(sink);
}

void seed_current_session_cookie(const std::string& name, const std::string& value) {
    if (name.empty() || is_cleared(value)) return;
    std::lock_guard<std::mutex> lock(current_mutex());
    current()[name] = value;
}

void note_engine_cookies(const std::string& /*url*/, const std::vector<std::string>& headers) {
    for (const auto& header : headers) {
        const std::string name = cookie_name(header);
        if (name != ".ROBLOSECURITY" && name != "rbxas") continue;
        const std::string value = cookie_value(header);
        // A cleared cookie is the engine saying this session is over --
        // a logout. Skipping it (which is what used to happen here) left
        // the stored copy behind, so the next launch came up holding a
        // credential the server had already revoked.
        //
        // Dropping the in-memory copy matters as much as the stored one:
        // current_session_cookies() is what a web-view panel is opened
        // with, and a panel must not be handed a dead session.
        if (is_cleared(value)) {
            {
                std::lock_guard<std::mutex> lock(current_mutex());
                current().erase(name);
            }
            if (cookie_cleared_sink()) cookie_cleared_sink()(name);
            continue;
        }
        // A real .ROBLOSECURITY is long; a short one is a placeholder,
        // not a session.
        if (name == ".ROBLOSECURITY" && value.size() < 32) continue;

        {
            std::lock_guard<std::mutex> lock(current_mutex());
            current()[name] = value;
        }

        // Persist it. Stud's login happens inside the real Roblox app,
        // so this callback is the only place a new session ever appears
        // and `rbxas` is what lets EVERY signed-in account come back
        // next launch rather than just the last active one.
        const auto& sink = name == "rbxas" ? account_list_cookie_sink() : session_cookie_sink();
        if (sink) sink(value);
    }
}

std::vector<std::string> current_session_cookies() {
    std::lock_guard<std::mutex> lock(current_mutex());
    std::vector<std::string> headers;
    headers.reserve(current().size());
    for (const auto& [name, value] : current()) {
        headers.push_back(name + "=" + value + "; Domain=.roblox.com; Path=/; Secure");
    }
    return headers;
}

std::string cookie_names(const std::vector<std::string>& headers) {
    std::string names;
    for (const auto& header : headers) {
        if (header.empty()) continue;
        if (!names.empty()) names += ", ";
        names += cookie_name(header);
    }
    return names;
}

}  // namespace stud::jni_bridge
