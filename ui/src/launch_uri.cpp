#include "launch_uri.h"

#include <QByteArray>
#include <QUrl>
#include <QUrlQuery>

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <string_view>

namespace stud::ui {

namespace {

std::string percent_decode(std::string_view value) {
    return QUrl::fromPercentEncoding(QByteArray(value.data(), static_cast<int>(value.size())))
        .toStdString();
}

// Case-insensitive key match -- the real format's exact key casing
// isn't confirmed against a live click (see launch_uri.h's caveat), and
// different public documentation of this format disagrees on casing
// (e.g. "robloxLocale" vs "robloxlocale"). Matching case-insensitively
// is the honest, defensive choice given that real uncertainty, not
// laziness.
bool key_equals(std::string_view key, std::string_view name) {
    if (key.size() != name.size()) {
        return false;
    }
    return std::equal(key.begin(), key.end(), name.begin(), [](char a, char b) {
        return std::tolower(static_cast<unsigned char>(a)) == std::tolower(static_cast<unsigned char>(b));
    });
}

void assign_field(LaunchUri& result, std::string_view key, std::string_view value) {
    std::string decoded = percent_decode(value);
    if (key_equals(key, "launchmode")) {
        result.launch_mode = decoded;
    } else if (key_equals(key, "gameinfo")) {
        result.game_info = decoded;
    } else if (key_equals(key, "placelauncherurl")) {
        result.place_launcher_url = decoded;
    } else if (key_equals(key, "launchtime")) {
        result.launch_time = decoded;
    } else if (key_equals(key, "browsertrackerid")) {
        result.browser_tracker_id = decoded;
    } else if (key_equals(key, "robloxlocale")) {
        result.roblox_locale = decoded;
    } else if (key_equals(key, "gamelocale")) {
        result.game_locale = decoded;
    } else if (key_equals(key, "channel")) {
        result.channel = decoded;
    }
    // Unknown keys are ignored, not an error -- the real format may
    // carry fields this struct doesn't track yet; ignoring unknowns
    // rather than failing keeps this forward-compatible.
}

}  // namespace

std::optional<LaunchUri> parse_launch_uri(const std::string& uri) {
    static constexpr std::string_view kSchemes[] = {"roblox-player:", "roblox:"};

    std::string_view rest;
    bool matched = false;
    for (auto scheme : kSchemes) {
        if (uri.size() > scheme.size() && std::string_view(uri).substr(0, scheme.size()) == scheme) {
            rest = std::string_view(uri).substr(scheme.size());
            matched = true;
            break;
        }
    }
    if (!matched) {
        return std::nullopt;
    }

    // Real format has a "//" prefix before the "1+launchmode:play+..."
    // payload -- tolerate its absence too rather than requiring an
    // exact match, since this is a best-effort parse of a
    // publicly-documented-but-not-locally-verified format.
    if (rest.size() >= 2 && rest.substr(0, 2) == "//") {
        rest = rest.substr(2);
    }

    LaunchUri result;

    // Two real formats, and Stud has to read both.
    //
    // The one below is the launcher protocol the Roblox desktop client
    // registers: `roblox-player://1+launchmode:play+placelauncherurl:...`,
    // "+"-separated key:value pairs.
    //
    // This one is what the website actually sends today, and what a
    // browser handed over in the report that found this:
    //   roblox://experiences/start?placeId=10518166490&gameInstanceId=...
    //     &joinAttemptId=...&joinAttemptOrigin=PlayButton
    // An ordinary URL with an ordinary query string. Nothing in the loop
    // below can read it -- there is no "+" and no "key:value" -- so every
    // field stayed at its default, place_id stayed 0, and Stud treated a
    // browser click as a bare launch and opened the home screen. Stud
    // even builds a link of this shape itself, for the Discord join
    // button, and could not have read its own.
    if (rest.find('?') != std::string_view::npos) {
        const QUrl url(QString::fromUtf8(uri.data(), static_cast<int>(uri.size())));
        const QUrlQuery query(url);
        auto take = [&query](const char* name) {
            return query.hasQueryItem(QString::fromLatin1(name))
                        ? query.queryItemValue(QString::fromLatin1(name), QUrl::FullyDecoded)
                              .toStdString()
                        : std::string();
        };
        const std::string place = take("placeId");
        if (!place.empty()) result.place_id = std::strtoll(place.c_str(), nullptr, 10);
        const std::string referrer = take("referredByPlayerId");
        if (!referrer.empty()) {
            result.referred_by_player_id = std::strtoll(referrer.c_str(), nullptr, 10);
        }
        result.game_instance_id = take("gameInstanceId");
        result.join_attempt_id = take("joinAttemptId");
        result.join_attempt_origin = take("joinAttemptOrigin");
        result.browser_tracker_id = take("browserTrackerId");
        result.roblox_locale = take("robloxLocale");
        result.game_locale = take("gameLocale");
        result.launch_time = take("launchTime");
        // A link that names a place is a request to play it. The
        // "+"-separated format says so in a launchmode field; this one
        // says it in its path (`experiences/start`), and the distinction
        // has no other consumer, so it is recorded the same way.
        if (result.place_id != 0) result.launch_mode = "play";
        return result;
    }

    size_t pos = 0;
    while (pos <= rest.size()) {
        size_t next_plus = rest.find('+', pos);
        std::string_view segment = (next_plus == std::string_view::npos)
                                        ? rest.substr(pos)
                                        : rest.substr(pos, next_plus - pos);

        size_t colon = segment.find(':');
        if (colon != std::string_view::npos) {
            assign_field(result, segment.substr(0, colon), segment.substr(colon + 1));
        }
        // Segments without a colon (e.g. the leading "1" protocol
        // version marker) are silently skipped -- not every "+"-
        // separated token is a key:value pair.

        if (next_plus == std::string_view::npos) {
            break;
        }
        pos = next_plus + 1;
    }

    // See launch_uri.h's own UPDATE 2 doc comment: real join-relevant
    // fields sitting in cleartext in place_launcher_url's own query
    // string. Best-effort -- an unparseable or field-less URL just
    // leaves these at their honest zero/empty defaults, same treatment
    // as every other field here.
    if (!result.place_launcher_url.empty()) {
        QUrlQuery query(QUrl(QString::fromStdString(result.place_launcher_url)));
        if (query.hasQueryItem("placeId")) {
            result.place_id = query.queryItemValue("placeId").toLongLong();
        }
        if (query.hasQueryItem("joinAttemptId")) {
            result.join_attempt_id = query.queryItemValue("joinAttemptId").toStdString();
        }
        if (query.hasQueryItem("referredByPlayerId")) {
            result.referred_by_player_id = query.queryItemValue("referredByPlayerId").toLongLong();
        }
        if (query.hasQueryItem("joinAttemptOrigin")) {
            result.join_attempt_origin = query.queryItemValue("joinAttemptOrigin").toStdString();
        }
    }

    return result;
}

}  // namespace stud::ui
