#include "history.h"

#include "stud/stud_paths.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSaveFile>

#include <algorithm>

namespace stud::ui::history {

namespace {

QString path() { return QString::fromStdString(stud::paths::data_dir()) + QStringLiteral("/history.json"); }

void save(const std::vector<Entry>& entries) {
    QJsonArray array;
    for (const Entry& e : entries) {
        QJsonArray servers;
        for (const Server& s : e.servers) {
            servers.append(QJsonObject{{"job", s.job},
                                       {"location", s.location},
                                       {"joined", qint64(s.joined)},
                                       {"seconds", qint64(s.seconds)}});
        }
        array.append(QJsonObject{{"servers", servers},
                                 {"place", e.place},
                                 {"name", e.name},
                                 {"icon", e.icon},
                                 {"lastJob", e.last_job},
                                 {"lastPlayed", qint64(e.last_played)},
                                 {"secondsPlayed", qint64(e.seconds_played)}});
    }
    QDir().mkpath(QFileInfo(path()).absolutePath());
    QSaveFile out(path());
    if (out.open(QIODevice::WriteOnly)) {
        out.write(QJsonDocument(array).toJson(QJsonDocument::Compact));
        out.commit();
    }
}

}  // namespace

std::vector<Entry> load() {
    std::vector<Entry> entries;
    QFile in(path());
    if (!in.open(QIODevice::ReadOnly)) return entries;
    for (const QJsonValue& v : QJsonDocument::fromJson(in.readAll()).array()) {
        const QJsonObject o = v.toObject();
        Entry e{o["place"].toString(), o["name"].toString(), o["icon"].toString(),
                o["lastJob"].toString(), o["lastPlayed"].toInteger(),
                o["secondsPlayed"].toInteger(), {}};
        for (const QJsonValue& s : o["servers"].toArray()) {
            const QJsonObject so = s.toObject();
            e.servers.push_back({so["job"].toString(), so["location"].toString(),
                                 so["joined"].toInteger(), so["seconds"].toInteger()});
        }
        entries.push_back(std::move(e));
    }
    std::sort(entries.begin(), entries.end(),
              [](const Entry& a, const Entry& b) { return a.last_played > b.last_played; });
    return entries;
}

void record(const QString& place, const QString& name, const QString& icon, const QString& job,
            int64_t start, int64_t end, const QString& location, bool keep_server) {
    if (place.isEmpty() || end <= start) return;
    std::vector<Entry> entries = load();
    auto it = std::find_if(entries.begin(), entries.end(), [&](const Entry& e) { return e.place == place; });
    if (it == entries.end()) {
        entries.push_back({place, {}, {}, {}, 0, 0, {}});
        it = entries.end() - 1;
    }
    if (!name.isEmpty()) it->name = name;
    if (!icon.isEmpty()) it->icon = icon;
    it->last_job = job;
    it->last_played = end;
    it->seconds_played += end - start;
    if (keep_server && !job.isEmpty()) {
        it->servers.insert(it->servers.begin(), Server{job, location, start, end - start});
    }
    save(entries);
}

void clear() { QFile::remove(path()); }

QString duration(int64_t seconds) {
    if (seconds >= 3600) {
        return QStringLiteral("%1 h %2 min").arg(seconds / 3600).arg((seconds % 3600) / 60);
    }
    if (seconds >= 60) return QStringLiteral("%1 min").arg(seconds / 60);
    return QStringLiteral("%1 s").arg(seconds);
}

}  // namespace stud::ui::history
