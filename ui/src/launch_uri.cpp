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

// Case-insensitive: documented key casing varies (robloxLocale vs robloxlocale).
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
    // Unknown keys are ignored.
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

    // The "//" before the payload is optional.
    if (rest.size() >= 2 && rest.substr(0, 2) == "//") {
        rest = rest.substr(2);
    }

    LaunchUri result;

    // Two formats: the launcher protocol (roblox-player://1+launchmode:play+...,
    // "+"-separated key:value pairs), and the website's ordinary URL with a query
    // (roblox://experiences/start?placeId=...), which Stud's own Discord join
    // button also builds.
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
        result.link_code = take("linkCode");
        if (result.link_code.empty()) result.link_code = take("privateServerLinkCode");
        result.access_code = take("accessCode");
        const std::string follow = take("userId");
        if (!follow.empty()) result.user_id = std::strtoll(follow.c_str(), nullptr, 10);
        result.join_attempt_id = take("joinAttemptId");
        result.join_attempt_origin = take("joinAttemptOrigin");
        result.browser_tracker_id = take("browserTrackerId");
        result.roblox_locale = take("robloxLocale");
        result.game_locale = take("gameLocale");
        result.launch_time = take("launchTime");
        // A link naming a place is a request to play it.
        if (result.place_id != 0 || result.user_id != 0) result.launch_mode = "play";
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
        // Tokens without a colon, like the leading version "1", are skipped.

        if (next_plus == std::string_view::npos) {
            break;
        }
        pos = next_plus + 1;
    }

    // Join fields from place_launcher_url's query string (see launch_uri.h).
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
        // PlaceLauncher's names: RequestPrivateGame has linkCode or accessCode,
        // RequestFollowUser has userId, and RequestGameJob has gameId.
        if (query.hasQueryItem("linkCode")) {
            result.link_code = query.queryItemValue("linkCode").toStdString();
        }
        if (query.hasQueryItem("accessCode")) {
            result.access_code = query.queryItemValue("accessCode").toStdString();
        }
        if (query.hasQueryItem("userId")) {
            result.user_id = query.queryItemValue("userId").toLongLong();
        }
        if (query.hasQueryItem("gameId") && result.game_instance_id.empty()) {
            result.game_instance_id = query.queryItemValue("gameId").toStdString();
        }
    }

    return result;
}

}  // namespace stud::ui
