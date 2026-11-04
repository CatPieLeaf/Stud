#pragma once

#include <cstdint>
#include <optional>
#include <string>

#include "stud/json_field.h"

// What a sign-in page and the app say to each other.
//
// The page reports through the hybrid bridge with Navigation.navigateToFeature
// commands, each carrying a callbackID, and a host is expected to answer it in
// the page. The check itself is started by the engine: it sends the app-bridge
// notification OPEN_CAPTCHA_VIEW and leaves showing the page to the platform.
// Both are protocol, read from how mocktail (Apache-2.0) and Cordial handle
// them.
namespace stud::jni_bridge {

inline bool hybrid_text_is_plain(const std::string& text) {
    for (const char c : text) {
        const auto byte = static_cast<unsigned char>(c);
        if (byte < 0x20 || byte == 0x7f) return false;
    }
    return true;
}

struct NavigationCommand {
    std::string feature;
    std::string callback_id;  // empty when the page sent none
};

// A navigateToFeature command, or nothing for any other command and for
// anything that does not read cleanly: these bytes come from a web page.
inline std::optional<NavigationCommand> parse_navigation_command(const std::string& command) {
    constexpr size_t kMaxIdentifier = 128;
    constexpr size_t kMaxCallbackId = 1024;
    if (json_string_field(command, "moduleID") != "Navigation" ||
        json_string_field(command, "functionName") != "navigateToFeature") {
        return std::nullopt;
    }
    NavigationCommand parsed;
    parsed.feature = json_string_field(command, "feature");
    parsed.callback_id = json_string_field(command, "callbackID");
    if (parsed.feature.empty() || parsed.feature.size() > kMaxIdentifier ||
        !hybrid_text_is_plain(parsed.feature) || parsed.callback_id.size() > kMaxCallbackId ||
        !hybrid_text_is_plain(parsed.callback_id)) {
        return std::nullopt;
    }
    return parsed;
}

// The answer to a command, true only if the page had its callback to call. The
// id goes in as a JSON string, so nothing a page chose can leave the literal.
inline std::string native_callback_script(const std::string& callback_id) {
    return "(() => { try { const b = window.Roblox && window.Roblox.Hybrid && "
           "window.Roblox.Hybrid.Bridge; if (b && typeof b.nativeCallback === \"function\") { "
           "b.nativeCallback(\"" + json_escaped(callback_id) +
           "\", true, {}); return true; } } catch (_) {} return false; })();";
}

inline std::string percent_encoded(const std::string& value) {
    static const char kHex[] = "0123456789ABCDEF";
    std::string out;
    for (const char c : value) {
        const auto byte = static_cast<unsigned char>(c);
        const bool plain = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                           (c >= '0' && c <= '9') || c == '-' || c == '_' || c == '.' || c == '~';
        if (plain) {
            out.push_back(c);
        } else {
            out.push_back('%');
            out.push_back(kHex[byte >> 4]);
            out.push_back(kHex[byte & 0xF]);
        }
    }
    return out;
}

// The page for an OPEN_CAPTCHA_VIEW payload, empty when it cannot be built.
// hybrid-return-token=1 is what makes the page report through the bridge.
inline std::string captcha_view_url(const std::string& payload) {
    constexpr size_t kMaxCredential = 4096;
    const std::string type = json_string_field(payload, "captchaType");
    if (type.empty()) return {};
    if (type != "login") {
        return "https://www.roblox.com/captcha/app/signup?hybrid-return-token=1";
    }
    std::string credential_type = json_string_field(payload, "cvalueType");
    if (credential_type == "phone_number") credential_type = "phonenumber";
    if (credential_type != "username" && credential_type != "email" &&
        credential_type != "phonenumber") {
        return {};
    }
    const std::string credential = json_string_field(payload, "cvalue");
    if (credential.empty() || credential.size() > kMaxCredential ||
        !hybrid_text_is_plain(credential)) {
        return {};
    }
    return "https://www.roblox.com/captcha/app/login?credentialsType=" + credential_type +
           "&credentialsValue=" + percent_encoded(credential) + "&hybrid-return-token=1";
}

}  // namespace stud::jni_bridge
