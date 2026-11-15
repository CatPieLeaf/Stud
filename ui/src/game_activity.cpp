#include "game_activity.h"

#include "history.h"
#include "notifications.h"

#include <QDateTime>
#include <QJsonArray>
#include <QJsonDocument>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QRegularExpression>
#include <QTimer>
#include <QUrl>

#include <cstdio>

namespace stud::ui {

namespace {

int64_t now_seconds() { return QDateTime::currentSecsSinceEpoch(); }

QString flag_of(const QString& country_code) {
    // A flag emoji is the country's two letters as regional indicator
    // symbols, U+1F1E6 being 'A', so any ISO code becomes one.
    QString flag;
    if (country_code.size() != 2) return flag;
    for (const QChar c : country_code.toUpper()) {
        if (c < u'A' || c > u'Z') return {};
        flag.append(QChar::fromUcs4(0x1F1E6 + (c.unicode() - u'A')));
    }
    return flag;
}

}  // namespace

GameActivity::GameActivity(const Options& options, QObject* parent)
    : QObject(parent), options_(options) {
    studio_since_ = now_seconds();
    if (options_.discord) {
        discord_rpc_start(kRobloxDiscordApplicationId);
        publish();
    }
}

GameActivity::~GameActivity() {
    if (options_.discord) discord_rpc_stop();
}

bool GameActivity::wants(const QByteArray& line) {
    static const char* const kMarks[] = {
        "! Joining game", "UDMUX Address", "doTeleport: joinScriptUrl",
        "Report game_join_loadtime:", "[BloxstrapRPC] ", "Client:Disconnect",
        "[FLog::PlaceManager]",
    };
    for (const char* mark : kMarks) {
        if (line.contains(mark)) return true;
    }
    return false;
}

void GameActivity::line(const QByteArray& raw) {
    const QString text = QString::fromUtf8(raw);
    if (options_.studio) {
        studio_line(text);
        return;
    }
    // "! Joining game '<job id>' place <place id> at <address>"
    static const QRegularExpression joining(
        QStringLiteral(R"(! Joining game '([0-9a-fA-F-]+)' place (\d+))"));
    // "UDMUX Address = <public address>, Port = ..."
    static const QRegularExpression udmux(QStringLiteral(R"(UDMUX Address = ([0-9.]+))"));
    // A teleport's join type: 4 and 6 are the private-game ones, which is
    // what a reserved server is (the line also holds the join ticket, which
    // is not kept).
    static const QRegularExpression join_type(QStringLiteral(R"(JoinTypeId"%3a(\d+)%2c)"));
    // The page the join came from: a private server's invite or its page.
    static const QRegularExpression referral(QStringLiteral(R"(referral_page:([^,]+))"));
    if (const auto m = joining.match(text); m.hasMatch()) {
        joined(m.captured(2), m.captured(1));
    } else if (const auto m = udmux.match(text); m.hasMatch()) {
        server(m.captured(1));
    } else if (text.contains(QStringLiteral("UgcExperienceController: doTeleport: joinScriptUrl"))) {
        const int type = join_type.match(text).captured(1).toInt();
        teleport_reserved_ = type == 4 || type == 6;
    } else if (text.contains(QStringLiteral("[FLog::GameJoinLoadTime] Report game_join_loadtime:"))) {
        const QString page = referral.match(text).captured(1);
        if (page.contains(QStringLiteral("RequestPrivateGame")) ||
            page.contains(QStringLiteral("GameDetailPageJSHybridEvent"))) {
            server_ = Server::Private;
            publish();
        }
    } else if (text.startsWith(QStringLiteral("[BloxstrapRPC] "))) {
        // The client prints each one twice, once in its log format and once
        // bare; the bare one is taken.
        const QJsonObject message =
            QJsonDocument::fromJson(raw.mid(qsizetype(sizeof("[BloxstrapRPC] ")) - 1)).object();
        const QString command = message.value(QStringLiteral("command")).toString();
        if (command == QStringLiteral("SetRichPresence")) {
            rich_presence(message.value(QStringLiteral("data")).toObject());
        } else if (command == QStringLiteral("SetLaunchData") && !place_.isEmpty()) {
            const QString data = message.value(QStringLiteral("data")).toString();
            if (data.size() <= 200) {
                launch_data_ = data;
                publish();
            }
        }
    } else if (text.contains(QStringLiteral("[FLog::SingleSurfaceApp] leaveUGCGameInternal"))) {
        // Back to the app. Not Client:Disconnect: a teleport (a lobby's
        // reserved server, another place, another game) disconnects too, and
        // the old connection can finish closing after the new join's line,
        // which ended the game just joined. A teleport logs its own
        // "Joining game", which replaces the place instead.
        left();
    }
}

void GameActivity::joined(const QString& place, const QString& job) {
    if (place == place_ && job == job_) return;
    // A teleport (to a lobby, another place, another game) ends the server
    // it leaves as leaving would: kept in the history, its location dropped.
    if (!place_.isEmpty() && options_.history_games) {
        history::record(place_, name_, icon_, job_, joined_at_, now_seconds(), location_,
                        options_.history_servers);
    }
    location_.clear();
    ++generation_;
    place_ = place;
    job_ = job;
    name_.clear();
    creator_.clear();
    icon_.clear();
    joined_at_ = now_seconds();
    server_ = teleport_reserved_ ? Server::Reserved : Server::Public;
    teleport_reserved_ = false;
    launch_data_.clear();
    game_ = {};
    game_details_ = game_state_ = game_start_ = game_end_ = false;
    std::printf("stud: joined place %s\n", qPrintable(place));
    publish();
    if (!options_.discord) return;

    // The place's universe, then its name, creator and icon.
    const quint64 generation = generation_;
    fetch(QStringLiteral("https://apis.roblox.com/universes/v1/places/%1/universe").arg(place),
          [this, generation](const QJsonObject& json) {
              const qint64 universe = json.value(QStringLiteral("universeId")).toInteger();
              if (generation != generation_ || universe <= 0) return;
              fetch(QStringLiteral("https://games.roblox.com/v1/games?universeIds=%1").arg(universe),
                    [this, generation](const QJsonObject& games) {
                        if (generation != generation_) return;
                        const QJsonObject game =
                            games.value(QStringLiteral("data")).toArray().at(0).toObject();
                        name_ = game.value(QStringLiteral("name")).toString();
                        creator_ = game.value(QStringLiteral("creator"))
                                       .toObject()
                                       .value(QStringLiteral("name"))
                                       .toString();
                        publish();
                    });
              fetch(QStringLiteral("https://thumbnails.roblox.com/v1/games/icons?universeIds=%1"
                                   "&size=512x512&format=Png&isCircular=false")
                        .arg(universe),
                    [this, generation](const QJsonObject& icons) {
                        if (generation != generation_) return;
                        icon_ = icons.value(QStringLiteral("data"))
                                    .toArray()
                                    .at(0)
                                    .toObject()
                                    .value(QStringLiteral("imageUrl"))
                                    .toString();
                        publish();
                    });
          });
}

void GameActivity::left() {
    if (place_.isEmpty()) return;
    if (options_.history_games) {
        history::record(place_, name_, icon_, job_, joined_at_, now_seconds(), location_,
                        options_.history_servers);
    }
    location_.clear();
    ++generation_;
    place_.clear();
    job_.clear();
    name_.clear();
    creator_.clear();
    icon_.clear();
    launch_data_.clear();
    server_ = Server::Public;
    game_ = {};
    game_details_ = game_state_ = game_start_ = game_end_ = false;
    publish();
}

void GameActivity::server(const QString& address) {
    // Looked up for the notification, or for the server history.
    const bool for_history = options_.history_games && options_.history_servers;
    if (!options_.region && !for_history) return;
    // Over TLS, and only ever the game server's public address, never the
    // player's.
    const quint64 generation = generation_;
    fetch(QStringLiteral("https://ipwho.is/%1?fields=success,country,country_code,city").arg(address),
          [this, generation](const QJsonObject& json) {
              if (!json.value(QStringLiteral("success")).toBool()) return;
              const QString country = json.value(QStringLiteral("country")).toString();
              const QString city = json.value(QStringLiteral("city")).toString();
              const QString where = city.isEmpty() ? country : city + QStringLiteral(", ") + country;
              if (generation == generation_) location_ = where;
              if (!options_.region) return;
              const QString flag = flag_of(json.value(QStringLiteral("country_code")).toString());
              send_notification(QStringLiteral("Server region"),
                                flag.isEmpty() ? where : flag + QStringLiteral(" ") + where,
                                QString(), {/*transient=*/true, /*suppress_sound=*/true});
          });
}

// BloxstrapRPC's SetRichPresence, as Froststrap and Bloxstrap read it:
// details and state (at most 128 characters; "<reset>" goes back to Stud's),
// timeStart and timeEnd (Unix seconds; 0 clears one), and smallImage and
// largeImage ({assetId, hoverText, clear, reset}). An absent key leaves what
// is showing.
void GameActivity::rich_presence(const QJsonObject& data) {
    if (!options_.discord || place_.isEmpty()) return;
    const auto text = [&data](const char* key, std::string& out, bool& set) {
        const QJsonValue value = data.value(QLatin1String(key));
        if (!value.isString() || value.toString().size() > 128) return;
        const QString s = value.toString();
        set = s != QStringLiteral("<reset>");
        out = set ? s.toStdString() : std::string();
    };
    text("details", game_.details, game_details_);
    text("state", game_.state, game_state_);
    const auto time = [&data](const char* key, int64_t& out, bool& set) {
        const QJsonValue value = data.value(QLatin1String(key));
        if (!value.isDouble()) return;
        out = value.toInteger();
        set = true;  // 0 is set too: it clears the time
    };
    time("timeStart", game_.start, game_start_);
    time("timeEnd", game_.end, game_end_);
    const quint64 generation = generation_;
    const auto image = [&, generation](const char* key, std::string DiscordActivity::*url,
                                       std::string DiscordActivity::*hover) {
        const QJsonObject spec = data.value(QLatin1String(key)).toObject();
        if (spec.isEmpty()) return;
        if (spec.value(QStringLiteral("clear")).toBool() || spec.value(QStringLiteral("reset")).toBool()) {
            game_.*url = std::string();
            game_.*hover = std::string();
            return;
        }
        if (spec.contains(QStringLiteral("hoverText"))) {
            game_.*hover = spec.value(QStringLiteral("hoverText")).toString().toStdString();
        }
        const qint64 asset = spec.value(QStringLiteral("assetId")).toInteger();
        if (asset > 0) {
            asset_image(asset, [this, generation, url](const QString& image_url) {
                if (generation != generation_) return;
                game_.*url = image_url.toStdString();
                publish();
            });
        }
    };
    image("largeImage", &DiscordActivity::large_image, &DiscordActivity::large_text);
    image("smallImage", &DiscordActivity::small_image, &DiscordActivity::small_text);
    publish();
}

namespace {
QString g_server_link;
}  // namespace

QString current_server_link() { return g_server_link; }

// The link to this server, where anyone can join it: a public server, or a
// reserved one the game has given launch data for. Never a private server's.
QString GameActivity::server_link() const {
    if (options_.studio || place_.isEmpty() || job_.isEmpty()) return {};
    const bool joinable =
        server_ == Server::Public || (server_ == Server::Reserved && !launch_data_.isEmpty());
    if (!joinable) return {};
    QString url = QStringLiteral("https://www.roblox.com/games/start?placeId=%1&gameInstanceId=%2")
                      .arg(place_, job_);
    if (!launch_data_.isEmpty()) {
        url += QStringLiteral("&launchData=") + QString::fromLatin1(QUrl::toPercentEncoding(launch_data_));
    }
    return url;
}

void GameActivity::publish() {
    g_server_link = server_link();
    if (!options_.discord) return;
    DiscordActivity a;
    if (options_.studio) {
        a.details = studio_editing_ ? "Editing a place" : "In Roblox Studio";
        a.state = "Roblox Studio";
        a.start = studio_since_;
        a.status_display = 2;
        discord_rpc_set(a);
        return;
    }
    if (place_.isEmpty()) {
        a.details = "In the Roblox app";
        discord_rpc_set(a);
        return;
    }
    a.details = game_details_ ? game_.details : (name_.isEmpty() ? "In a game" : name_.toStdString());
    a.state = game_state_ ? game_.state
                          : (creator_.isEmpty() ? std::string() : "by " + creator_.toStdString());
    a.large_image = !game_.large_image.empty() ? game_.large_image : icon_.toStdString();
    a.large_text = !game_.large_text.empty() ? game_.large_text : name_.toStdString();
    a.small_image = game_.small_image;
    a.small_text = game_.small_text;
    // The game's name as the short status, in place of "Playing ROBLOX".
    a.status_display = 2;
    a.start = game_start_ ? game_.start : joined_at_;
    a.end = game_end_ ? game_.end : 0;
    if (options_.join_button && !g_server_link.isEmpty()) {
        a.buttons.emplace_back("Join server", g_server_link.toStdString());
    }
    a.buttons.emplace_back("See game page",
                           QStringLiteral("https://www.roblox.com/games/%1").arg(place_).toStdString());
    discord_rpc_set(a);
}

// Studio: a place being edited, or Studio open on its start page.
void GameActivity::studio_line(const QString& text) {
    const bool opened = text.contains(QStringLiteral("[FLog::PlaceManager] Start to open place"));
    const bool closed = text.contains(QStringLiteral("PlaceManager::closeCurrentPlayDoc"));
    if (!opened && !closed) return;
    studio_editing_ = opened;
    studio_since_ = now_seconds();
    publish();
}

void GameActivity::fetch(const QString& url, std::function<void(const QJsonObject&)> done) {
    QNetworkRequest request{QUrl(url)};
    request.setHeader(QNetworkRequest::UserAgentHeader, QStringLiteral("Stud/" STUD_VERSION));
    request.setTransferTimeout(10000);
    QNetworkReply* reply = net_.get(request);
    connect(reply, &QNetworkReply::finished, this, [reply, done = std::move(done)]() {
        reply->deleteLater();
        if (reply->error() != QNetworkReply::NoError) return;
        done(QJsonDocument::fromJson(reply->readAll()).object());
    });
}

void GameActivity::asset_image(qint64 asset, std::function<void(const QString&)> done) {
    fetch(QStringLiteral("https://thumbnails.roblox.com/v1/assets?assetIds=%1&size=512x512&format=Png")
              .arg(asset),
          [done = std::move(done)](const QJsonObject& json) {
              const QString url = json.value(QStringLiteral("data"))
                                      .toArray()
                                      .at(0)
                                      .toObject()
                                      .value(QStringLiteral("imageUrl"))
                                      .toString();
              if (!url.isEmpty()) done(url);
          });
}

}  // namespace stud::ui
