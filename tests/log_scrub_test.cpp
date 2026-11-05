// What of the engine's log may reach the session log.

#include "log_scrub.h"

#include <cstdio>
#include <cstdlib>
#include <string>

namespace {

void check(bool condition, const char* what) {
    if (!condition) {
        std::fprintf(stderr, "FAILED: %s\n", what);
        std::exit(1);
    }
    std::printf("ok: %s\n", what);
}

}  // namespace

int main() {
    using stud::runtime::scrub_engine_log_line;
    const std::string join = R"(doTeleport: joinScriptUrl https://x/Join.ashx?ticketVersion=2&ticket={"UserId"%3a1}&a=b c)";
    const std::string scrubbed = scrub_engine_log_line(join);
    check(scrubbed == "doTeleport: joinScriptUrl https://x/Join.ashx?ticketVersion=2&ticket=<removed>",
          "a join URL stops at the ticket, and ticketVersion is not a ticket");
    check(scrub_engine_log_line("authenticationTicket=abc") == "authenticationTicket=<removed>",
          "any capitalisation");
    check(scrub_engine_log_line(R"({"ticket":"abc"})") == R"({"ticket":<removed>)", "JSON");
    check(scrub_engine_log_line("u?ticket%3Dabc") == "u?ticket%3D<removed>", "URL-encoded");
    check(scrub_engine_log_line("a ticket was issued") == "a ticket was issued",
          "the word alone stays");
    check(scrub_engine_log_line("sticker=1") == "sticker=1", "a similar word stays");
    return 0;
}
