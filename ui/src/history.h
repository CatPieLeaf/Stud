#pragma once

#include <QString>

#include <cstdint>
#include <vector>

// One entry per place played through Stud, in ~/.local/share/stud/history.json.
namespace stud::ui::history {

// `location` is "" when unknown.
struct Server {
    QString job;
    QString location;
    int64_t joined = 0;  // Unix seconds
    int64_t seconds = 0;
};

struct Entry {
    QString place;
    QString name;
    QString icon;  // URL
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

void clear();

// "3 h 12 min", "12 min", "40 s".
QString duration(int64_t seconds);

}  // namespace stud::ui::history
