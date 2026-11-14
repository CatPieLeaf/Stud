#pragma once

#include <QString>

#include <cstdint>
#include <vector>

// The games played through Stud, one entry a place: its name and icon, the
// last server, when it was last played and the time played in all. Kept in
// ~/.local/share/stud/history.json.
namespace stud::ui::history {

struct Entry {
    QString place;
    QString name;
    QString icon;  // the game icon's URL
    QString last_job;
    int64_t last_played = 0;   // Unix seconds
    int64_t seconds_played = 0;
};

// Most recently played first.
std::vector<Entry> load();

// Adds a session of `place` from `start` to `end` (Unix seconds).
void record(const QString& place, const QString& name, const QString& icon, const QString& job,
            int64_t start, int64_t end);

// Every session's time added up.
int64_t total_seconds();

// "3 h 12 min", "12 min", "40 s".
QString duration(int64_t seconds);

}  // namespace stud::ui::history
