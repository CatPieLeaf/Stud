#include "game_activity.h"

#include "notifications.h"

#include <QDateTime>
#include <QJsonArray>
#include <QJsonDocument>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QRegularExpression>
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

GameActivity::GameActivity(bool discord, bool join_button, bool region, QObject* parent)
    : QObject(parent), discord_(discord), join_button_(join_button), region_(region) {
    if (discord_) {
        discord_rpc_start(kRobloxDiscordApplicationId);
        publish();
    }
}

GameActivity::~GameActivity() {
    if (discord_) discord_rpc_stop();
}

void GameActivity::line(const QByteArray& raw) {
    const QString text = QString::fromUtf8(raw);
    // "! Joining game '<job id>' place <place id> at <address>"
    static const QRegularExpression joining(
        QStringLiteral(R"(! Joining game '([0-9a-fA-F-]+)' place (\d+))"));
    // "UDMUX Address = <public address>, Port = ..."
    static const QRegularExpression udmux(QStringLiteral(R"(UDMUX Address = ([0-9.]+))"));
    if (const auto m = joining.match(text); m.hasMatch()) {
        joined(m.captured(2), m.captured(1));
    } else if (const auto m = udmux.match(text); m.hasMatch()) {
        server(m.captured(1));
    } else if (text.startsWith(QStringLiteral("[BloxstrapRPC] "))) {
        // The client prints each one twice, once in its log format and once
        // bare; the bare one is taken.
        const QJsonObject message =
            QJsonDocument::fromJson(raw.mid(qsizetype(sizeof("[BloxstrapRPC] ")) - 1)).object();
        if (message.value(QStringLiteral("command")).toString() == QStringLiteral("SetRichPresence")) {
            rich_presence(message.value(QStringLiteral("data")).toObject());
        }
    } else if (text.contains(QStringLiteral("[DFLog::NetworkClient] Client:Disconnect"))) {
        left();
    }
}

void GameActivity::joined(const QString& place, const QString& job) {
    if (place == place_ && job == job_) return;
    ++generation_;
    place_ = place;
    job_ = job;
    name_.clear();
    creator_.clear();
    icon_.clear();
    joined_at_ = now_seconds();
    game_ = {};
    game_details_ = game_state_ = game_times_ = false;
    std::printf("stud: joined place %s\n", qPrintable(place));
    publish();
    if (!discord_) return;

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
    ++generation_;
    place_.clear();
    job_.clear();
    name_.clear();
    creator_.clear();
    icon_.clear();
    game_ = {};
    game_details_ = game_state_ = game_times_ = false;
    publish();
}

void GameActivity::server(const QString& address) {
    if (!region_) return;
    // Over TLS, and only ever the game server's public address, never the
    // player's.
    fetch(QStringLiteral("https://ipwho.is/%1?fields=success,country,country_code,city").arg(address),
          [](const QJsonObject& json) {
              if (!json.value(QStringLiteral("success")).toBool()) return;
              const QString country = json.value(QStringLiteral("country")).toString();
              const QString city = json.value(QStringLiteral("city")).toString();
              const QString where = city.isEmpty() ? country : city + QStringLiteral(", ") + country;
              const QString flag = flag_of(json.value(QStringLiteral("country_code")).toString());
              send_notification(QStringLiteral("Server region"),
                                flag.isEmpty() ? where : flag + QStringLiteral(" ") + where,
                                QString(), {/*transient=*/true, /*suppress_sound=*/true});
          });
}

// BloxstrapRPC's SetRichPresence: details, state, timeStart, timeEnd,
// smallImage and largeImage ({assetId, hoverText, clear, reset}). A key that
// is absent leaves what is showing; "<reset>" goes back to Stud's own.
void GameActivity::rich_presence(const QJsonObject& data) {
    if (!discord_ || place_.isEmpty()) return;
    const auto text = [&data](const char* key, std::string& out, bool& set) {
        const QJsonValue value = data.value(QLatin1String(key));
        if (!value.isString()) return;
        const QString s = value.toString();
        set = s != QStringLiteral("<reset>");
        out = set ? s.toStdString() : std::string();
    };
    text("details", game_.details, game_details_);
    text("state", game_.state, game_state_);
    if (data.contains(QStringLiteral("timeStart")) || data.contains(QStringLiteral("timeEnd"))) {
        game_times_ = true;
        game_.start = data.value(QStringLiteral("timeStart")).toInteger();
        game_.end = data.value(QStringLiteral("timeEnd")).toInteger();
    }
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

void GameActivity::publish() {
    if (!discord_) return;
    DiscordActivity a;
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
    if (game_times_) {
        a.start = game_.start;
        a.end = game_.end;
    } else {
        a.start = joined_at_;
    }
    if (join_button_) {
        a.button_label = "Join server";
        a.button_url = QStringLiteral("https://www.roblox.com/games/start?placeId=%1&gameInstanceId=%2")
                           .arg(place_, job_)
                           .toStdString();
    }
    discord_rpc_set(a);
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
