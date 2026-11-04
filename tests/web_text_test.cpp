// What a web page or a text box hands Stud: who gets the session, and what a
// payload or a caret really says.
#undef NDEBUG
#include <cassert>
#include <string>

#include "stud/json_field.h"
#include "stud/stud_paths.h"
#include "stud/text_editor.h"
#include "stud/web_origin.h"

using stud::jni_bridge::json_escaped;
using stud::jni_bridge::json_string_field;
using stud::jni_bridge::json_true_field;
using stud::jni_bridge::utf16_offset;
using stud::web::is_roblox_https_url;

int main() {
    assert(is_roblox_https_url("https://www.roblox.com/my/messages"));
    assert(is_roblox_https_url("https://roblox.com"));
    assert(is_roblox_https_url("https://blog.roblox.com:443/a?b=1"));
    assert(is_roblox_https_url("https://WWW.Roblox.com/"));
    assert(!is_roblox_https_url("http://www.roblox.com/"));
    assert(!is_roblox_https_url("https://example.com/"));
    assert(!is_roblox_https_url("https://roblox.com.example.com/"));
    assert(!is_roblox_https_url("https://notroblox.com/"));
    assert(!is_roblox_https_url("https://roblox.com@example.com/"));
    assert(!is_roblox_https_url("https://example.com\\.roblox.com/"));
    assert(!is_roblox_https_url("https://example.com/.roblox.com"));
    assert(!is_roblox_https_url("https://example.com?.roblox.com"));
    assert(!is_roblox_https_url("https://example.com#.roblox.com"));
    assert(!is_roblox_https_url("https://exa%6dple.roblox.com/"));
    assert(!is_roblox_https_url("file:///etc/passwd"));
    assert(!is_roblox_https_url(""));

    assert(json_string_field(R"({"url":"https://a/?x=1&y=2"})", "url") ==
           "https://a/?x=1&y=2");
    assert(json_string_field(R"({"url" : "a\/b\"c"})", "url") == "a/b\"c");
    assert(json_string_field(R"({"name":"ç😀"})", "name") ==
           "\xC3\xA7\xF0\x9F\x98\x80");
    assert(json_string_field(R"({"url":null,"other":"x"})", "url").empty());
    assert(json_string_field(R"({"a":"url","url":"real"})", "url") == "real");
    assert(json_string_field(R"({"a":"b"})", "").empty());
    assert(json_true_field(R"({"on": true})", "on"));
    assert(json_true_field(R"({"on":true})", "on"));
    assert(!json_true_field(R"({"on":false})", "on"));
    assert(!json_true_field(R"({"a":"on"})", "on"));
    assert(json_escaped("a\"b\\c\n") == "a\\\"b\\\\c\\u000a");
    assert(json_string_field("{\"u\":\"" + json_escaped("x\ny\"") + "\"}", "u") == "x\ny\"");

    assert(stud::paths::valid_secret_name("roblosecurity"));
    assert(stud::paths::valid_secret_name("safe-storage_2"));
    assert(!stud::paths::valid_secret_name(""));
    assert(!stud::paths::valid_secret_name(".."));
    assert(!stud::paths::valid_secret_name("../../x"));
    assert(!stud::paths::valid_secret_name("a/b"));
    assert(!stud::paths::valid_secret_name("a\nb"));

    assert(utf16_offset("abc", 2) == 2);
    assert(utf16_offset("\xC3\xA7z", 2) == 1);          // two bytes, one unit
    assert(utf16_offset("\xF0\x9F\x98\x80z", 4) == 2);  // four bytes, two units
    assert(utf16_offset("ab", 9) == 2);
    return 0;
}
