#include "github_mirror.h"

#include "stud/settings.h"

#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>

namespace stud::ui::github {

namespace {

bool prefer_jsdelivr() {
    try {
        return stud::config::load_settings(stud::config::default_config_path()).use_jsdelivr;
    } catch (const std::exception&) {
        return stud::config::StudSettings().use_jsdelivr;
    }
}

void fetch(const QUrl& url, std::function<void(const QByteArray& body, const QString& error)> done) {
    auto* net = new QNetworkAccessManager();
    QNetworkRequest request(url);
    // GitHub's API refuses requests without one.
    request.setHeader(QNetworkRequest::UserAgentHeader, QStringLiteral("Stud/" STUD_VERSION));
    request.setTransferTimeout(30000);
    QNetworkReply* reply = net->get(request);
    QObject::connect(reply, &QNetworkReply::finished, net, [net, reply, url, done] {
        net->deleteLater();
        if (reply->error() != QNetworkReply::NoError) {
            done({}, QStringLiteral("could not download %1 from %2: %3")
                         .arg(url.fileName(), url.host(), reply->errorString()));
            return;
        }
        done(reply->readAll(), QString());
    });
}

}  // namespace

QUrl jsdelivr_copy(const QUrl& url) {
    if (url.host() != QLatin1String("raw.githubusercontent.com")) return {};
    // /owner/repository/ref/path... -> /gh/owner/repository@ref/path...
    const QStringList parts = url.path().split(QLatin1Char('/'), Qt::SkipEmptyParts);
    if (parts.size() < 4) return {};
    return QUrl(QStringLiteral("https://cdn.jsdelivr.net/gh/%1/%2@%3/%4")
                    .arg(parts.at(0), parts.at(1), parts.at(2), parts.mid(3).join(QLatin1Char('/'))));
}

void get_either(const QUrl& github, const QUrl& jsdelivr,
                std::function<void(const QByteArray& body, bool from_jsdelivr, const QString& error)> done) {
    if (jsdelivr.isEmpty()) {
        fetch(github, [done](const QByteArray& body, const QString& error) { done(body, false, error); });
        return;
    }
    const bool jsdelivr_first = prefer_jsdelivr();
    const QUrl first = jsdelivr_first ? jsdelivr : github;
    const QUrl second = jsdelivr_first ? github : jsdelivr;
    fetch(first, [second, jsdelivr_first, done](const QByteArray& body, const QString& failed) {
        if (failed.isEmpty()) return done(body, jsdelivr_first, QString());
        fetch(second, [failed, jsdelivr_first, done](const QByteArray& body, const QString& error) {
            done(body, !jsdelivr_first, error.isEmpty() ? QString() : failed + QStringLiteral("; ") + error);
        });
    });
}

void get(const QUrl& url, std::function<void(const QByteArray& body, const QString& error)> done) {
    get_either(url, jsdelivr_copy(url),
               [done](const QByteArray& body, bool, const QString& error) { done(body, error); });
}

}  // namespace stud::ui::github
