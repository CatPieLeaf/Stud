#pragma once

#include <QString>

#include <cstdint>
#include <vector>

// The games played through Stud, one entry a place: its name and icon, the
// last server, when it was last played and the time played in all. Kept in
// ~/.local/share/stud/history.json.
namespace stud::ui::history {

// One server joined in a game: its job, where it is ("" when unknown), when it
// was joined and how long it was played.
struct Server {
    QString job;
    QString location;
    int64_t joined = 0;  // Unix seconds
    int64_t seconds = 0;
};

struct Entry {
    QString place;
    QString name;
    QString icon;  // the game icon's URL
    QString last_job;
    int64_t last_played = 0;   // Unix seconds
    int64_t seconds_played = 0;
    std::vector<Server> servers;  // most recent first
};

// Most recently played first.
std::vector<Entry> load();

// Adds a session of `place` from `start` to `end` (Unix seconds), and its
// server, at `location`, when `keep_server`.
void record(const QString& place, const QString& name, const QString& icon, const QString& job,
            int64_t start, int64_t end, const QString& location, bool keep_server);

// Forgets every game and server.
void clear();

// Every session's time added up.
int64_t total_seconds();

// "3 h 12 min", "12 min", "40 s".
QString duration(int64_t seconds);

}  // namespace stud::ui::history
