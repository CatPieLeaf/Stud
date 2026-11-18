#pragma once

#include <QJsonObject>
#include <QNetworkAccessManager>
#include <QObject>
#include <QHash>
#include <QDateTime>
#include <QFileSystemWatcher>
#include <QString>

#include <cstdint>
#include <functional>

#include "discord_rpc.h"

namespace stud::ui {

// Empty when not in a game, or in one nobody else can join.
QString current_server_link();

// Reads the client's output: the joined game and server, and BloxstrapRPC messages
// (SetRichPresence, SetLaunchData). Feeds Discord and the region notification;
// nothing is sent unless its setting is on.
class GameActivity : public QObject {
    Q_OBJECT

public:
    struct Options {
        bool discord = false;
        bool join_button = false;
        bool region = false;
        // Servers are only recorded with their games.
        bool history_games = true;
        bool history_servers = true;
        bool studio = false;
        // Studio stops copying its log to stdout once started and opens place files
        // in a second Studio, so its presence follows the log files instead.
        QString studio_logs;
    };
    explicit GameActivity(const Options& options, QObject* parent = nullptr);
    ~GameActivity() override;

    void line(const QByteArray& text);

    // A plain search, cheap enough to call on every line from the reader thread.
    static bool wants(const QByteArray& line);

    // Records the game in progress, if any.
    void left();

private:
    enum class Server { Public, Private, Reserved };
    void studio_line(const QString& text);
    void follow_studio_logs();
    void read_studio_log(const QString& path);
    void look_up(const QString& place);

    void joined(const QString& place, const QString& job);
    void server(const QString& address);
    void rich_presence(const QJsonObject& data);
    void publish();
    QString server_link() const;
    void fetch(const QString& url, std::function<void(const QJsonObject&)> done);
    void asset_image(qint64 asset, std::function<void(const QString&)> done);

    Options options_;
    bool studio_editing_ = false;
    // -1 start page, 0 a place, 2 a script; -2 before Studio says.
    int studio_doc_ = -2;
    int64_t studio_since_ = 0;
    QFileSystemWatcher studio_watcher_;
    QDateTime studio_started_;
    QHash<QString, std::pair<qint64, QByteArray>> studio_read_;
    QNetworkAccessManager net_;
    // Bumped on every join and leave, so a late lookup for a game already left is dropped.
    quint64 generation_ = 0;
    QString location_;

    QString place_;
    QString job_;
    QString name_;
    QString creator_;
    QString icon_;
    int64_t joined_at_ = 0;
    Server server_ = Server::Public;
    bool teleport_reserved_ = false;
    QString launch_data_;

    DiscordActivity game_;
    bool game_details_ = false;
    bool game_state_ = false;
    bool game_start_ = false;
    bool game_end_ = false;
};

}  // namespace stud::ui
