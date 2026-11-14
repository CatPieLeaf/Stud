#pragma once

#include <QJsonObject>
#include <QNetworkAccessManager>
#include <QObject>
#include <QString>

#include <cstdint>
#include <functional>

#include "discord_rpc.h"

namespace stud::ui {

// What the player is doing, read from the client's own output: the game it
// joins and the server it reaches, and what a game sends through BloxstrapRPC
// (SetRichPresence, SetLaunchData). Feeds Discord and the server region
// notification. Nothing is sent anywhere unless its setting is on.
class GameActivity : public QObject {
    Q_OBJECT

public:
    GameActivity(bool discord, bool join_button, bool region, QObject* parent = nullptr);
    ~GameActivity() override;

    // One line of the client's output, as it printed it.
    void line(const QByteArray& text);

    // Whether `line` is one line() reads: a plain search, cheap enough for
    // the thread that drains the client's output to ask about every line.
    static bool wants(const QByteArray& line);

private:
    enum class Server { Public, Private, Reserved };

    void joined(const QString& place, const QString& job);
    void left();
    void server(const QString& address);
    void rich_presence(const QJsonObject& data);
    void publish();
    // GETs `url` and hands the JSON object back, or nothing on failure.
    void fetch(const QString& url, std::function<void(const QJsonObject&)> done);
    // The image URL of a Roblox asset, through the thumbnails API.
    void asset_image(qint64 asset, std::function<void(const QString&)> done);

    bool discord_;
    bool join_button_;
    bool region_;
    QNetworkAccessManager net_;
    // Bumped on every join and leave, so a lookup answering late for a game
    // already left does not show it.
    quint64 generation_ = 0;

    QString place_;
    QString job_;
    QString name_;
    QString creator_;
    QString icon_;
    int64_t joined_at_ = 0;
    Server server_ = Server::Public;
    // The teleport just started goes to a reserved server.
    bool teleport_reserved_ = false;
    // What the game gave SetLaunchData, carried in the join link.
    QString launch_data_;

    // What the game itself set through SetRichPresence, over the defaults.
    DiscordActivity game_;
    bool game_details_ = false;
    bool game_state_ = false;
    bool game_start_ = false;
    bool game_end_ = false;
};

}  // namespace stud::ui
