#pragma once

#include <cstddef>
#include <string>
#include <string_view>

namespace stud::ui {

// Truncates a client log line at any ticket value: teleports can log the signed join
// URL, and nothing after it is safe to keep.
inline std::string scrub_client_log_line(std::string_view line) {
    static constexpr std::string_view kWord = "icket";  // ticket, Ticket
    for (size_t at = line.find(kWord); at != std::string_view::npos;
         at = line.find(kWord, at + 1)) {
        if (at == 0 || (line[at - 1] != 't' && line[at - 1] != 'T')) continue;
        // Matches ticket", escaped quotes, spaced colons and %22ticket%22.
        size_t after = at + kWord.size();
        for (;;) {
            if (after < line.size() &&
                (line[after] == '"' || line[after] == '\\' || line[after] == ' ')) {
                ++after;
            } else if (line.substr(after, 3) == "%22") {
                after += 3;
            } else {
                break;
            }
        }
        const std::string_view rest = line.substr(after);
        size_t value = std::string_view::npos;
        if (rest.rfind("=", 0) == 0 || rest.rfind(":", 0) == 0) value = after + 1;
        for (const std::string_view encoded : {"%3d", "%3D", "%3a", "%3A"}) {
            if (rest.rfind(encoded, 0) == 0) value = after + encoded.size();
        }
        if (value == std::string_view::npos) continue;
        return std::string(line.substr(0, value)) + "<removed>";
    }
    return std::string(line);
}

}  // namespace stud::ui
