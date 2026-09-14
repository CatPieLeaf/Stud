#include "update_check.h"

#include <QJsonDocument>
#include <QJsonObject>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QPointer>
#include <QStringList>
#include <QTimer>

namespace stud::ui {
namespace {

// GitHub's own API for "what is the newest release". It answers with the
// release marked latest, which is what a user clicking through would
// land on -- a pre-release does not count as an update.
constexpr const char* kLatestApi = "https://api.github.com/repos/CatPieLeaf/Stud/releases/latest";

bool g_started = false;
bool g_update_available = false;
QString g_latest;

// Compares two dotted versions numerically rather than as text, because
// "1.10.0" is newer than "1.9.0" and a string comparison says otherwise.
// Anything unparseable compares as not-newer: a malformed tag must never
// nag a user who is already up to date.
bool is_newer(const QString& candidate, const QString& current) {
    const QStringList a = candidate.split('.');
    const QStringList b = current.split('.');
    for (int i = 0; i < qMax(a.size(), b.size()); ++i) {
        bool a_ok = false;
        bool b_ok = false;
        const int lhs = i < a.size() ? a.at(i).toInt(&a_ok) : 0;
        const int rhs = i < b.size() ? b.at(i).toInt(&b_ok) : 0;
        if (i < a.size() && !a_ok) return false;
        if (i < b.size() && !b_ok) return false;
        if (lhs != rhs) return lhs > rhs;
    }
    return false;
}

}  // namespace

UpdateCheck::UpdateCheck(QObject* parent) : QObject(parent) {}

UpdateCheck* UpdateCheck::instance() {
    static UpdateCheck check;
    return &check;
}

QString UpdateCheck::releasesUrl() {
    return QStringLiteral("https://github.com/CatPieLeaf/Stud/releases/latest");
}

bool UpdateCheck::updateAvailable() { return g_update_available; }

QString UpdateCheck::latestVersion() { return g_latest; }

void UpdateCheck::start() {
    if (g_started) return;
    g_started = true;

    // Owned by the singleton, so it outlives this call and dies with the
    // application rather than leaking a manager per check.
    auto* manager = new QNetworkAccessManager(instance());
    QNetworkRequest request((QUrl(QString::fromLatin1(kLatestApi))));
    request.setRawHeader("Accept", "application/vnd.github+json");
    // GitHub asks for a User-Agent and answers 403 without one. Naming
    // the application and version is what it wants, and is no more than
    // the request already reveals.
    request.setRawHeader("User-Agent", "Stud/" STUD_VERSION);
    request.setTransferTimeout(8000);

    QNetworkReply* reply = manager->get(request);
    QObject::connect(reply, &QNetworkReply::finished, instance(), [reply]() {
        reply->deleteLater();
        if (reply->error() != QNetworkReply::NoError) return;  // silence, see the header

        const QJsonDocument doc = QJsonDocument::fromJson(reply->readAll());
        if (!doc.isObject()) return;
        // "v1.1.0" -> "1.1.0": the tag carries the v, the version does not.
        QString tag = doc.object().value(QStringLiteral("tag_name")).toString();
        if (tag.startsWith('v', Qt::CaseInsensitive)) tag.remove(0, 1);
        if (tag.isEmpty()) return;

        if (!is_newer(tag, QStringLiteral(STUD_VERSION))) return;
        g_update_available = true;
        g_latest = tag;
        Q_EMIT instance()->updateFound(tag);
    });
}

}  // namespace stud::ui
