#pragma once

#include <cstddef>
#include <string>
#include <string_view>

namespace stud::runtime {

// An engine log line, fit for the session log. The log exists to be exported
// and sent to someone, and on a teleport the engine logs its whole join URL
// with the signed ticket in it. The line stops at the value: what follows a
// ticket in a join URL is no better, and a name in the ticket can hold a
// space, so there is no safe place to pick the line up again.
//
// Stud's own readers (note_engine_log_line) take the raw line; only what is
// printed goes through here.
inline std::string scrub_engine_log_line(std::string_view line) {
    static constexpr std::string_view kWord = "icket";  // ticket, Ticket
    for (size_t at = line.find(kWord); at != std::string_view::npos;
         at = line.find(kWord, at + 1)) {
        if (at == 0 || (line[at - 1] != 't' && line[at - 1] != 'T')) continue;
        size_t after = at + kWord.size();
        if (after < line.size() && line[after] == '"') ++after;  // "ticket":
        const std::string_view rest = line.substr(after);
        size_t value = std::string_view::npos;
        if (rest.rfind("=", 0) == 0 || rest.rfind(":", 0) == 0) value = after + 1;
        if (rest.rfind("%3d", 0) == 0 || rest.rfind("%3D", 0) == 0) value = after + 3;
        if (value == std::string_view::npos) continue;
        return std::string(line.substr(0, value)) + "<removed>";
    }
    return std::string(line);
}

}  // namespace stud::runtime
