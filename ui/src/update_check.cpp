#include "update_check.h"

#include "notifications.h"

#include <QCryptographicHash>
#include <QDesktopServices>
#include <QFile>
#include <QFileInfo>
#include <QUrl>

#include <cstdio>
#include <sys/stat.h>

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

// releases/latest skips pre-releases.
constexpr const char* kLatestApi = "https://api.github.com/repos/CatPieLeaf/Stud/releases/latest";

bool g_started = false;
bool g_update_available = false;
QString g_latest;

// Numeric (1.10.0 > 1.9.0). Unparseable counts as not newer, so a bad tag never nags.
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

// The .zsync header records the whole file's SHA-1.
constexpr const char* kAppImage =
    "https://github.com/CatPieLeaf/Stud/releases/latest/download/Stud-x86_64.AppImage";
bool g_updating = false;

QNetworkRequest release_request(const QString& url) {
    QNetworkRequest request((QUrl(url)));
    request.setRawHeader("User-Agent", "Stud/" STUD_VERSION);
    // Times out a stall, not a slow transfer.
    request.setTransferTimeout(30000);
    return request;
}

void fail_update(const QString& why) {
    g_updating = false;
    std::fprintf(stderr, "stud: self-update failed: %s\n", qPrintable(why));
    send_notification(QStringLiteral("Stud could not update"), why);
}

// Downloads beside the running AppImage and renames it over once the SHA-1 matches.
void update_appimage(const QString& target) {
    if (g_updating) return;
    g_updating = true;
    auto* manager = new QNetworkAccessManager(UpdateCheck::instance());
    QNetworkReply* sums = manager->get(release_request(QString::fromLatin1(kAppImage) + ".zsync"));
    QObject::connect(sums, &QNetworkReply::finished, UpdateCheck::instance(), [sums, manager, target] {
        sums->deleteLater();
        QByteArray sha1;
        // The header ends at the first empty line.
        for (const QByteArray& line : sums->readAll().split('\n')) {
            if (line.isEmpty()) break;
            if (line.startsWith("SHA-1: ")) sha1 = line.mid(7).trimmed().toLower();
        }
        if (sums->error() != QNetworkReply::NoError || sha1.isEmpty()) {
            manager->deleteLater();
            fail_update(QStringLiteral("The release's checksum could not be read."));
            return;
        }
        const QString partial = target + QStringLiteral(".download");
        auto* out = new QFile(partial);
        if (!out->open(QIODevice::WriteOnly | QIODevice::Truncate)) {
            delete out;
            manager->deleteLater();
            fail_update(QStringLiteral("%1 is not writable.").arg(QFileInfo(target).absolutePath()));
            return;
        }
        send_notification(QStringLiteral("Updating Stud"),
                          QStringLiteral("Downloading Stud %1.").arg(UpdateCheck::latestVersion()));
        // Hashed as written, so a short write (full disk) fails the check.
        auto* hash = new QCryptographicHash(QCryptographicHash::Sha1);
        auto* write_failed = new bool(false);
        const auto take = [out, hash, write_failed](const QByteArray& chunk) {
            if (out->write(chunk) != chunk.size()) *write_failed = true;
            hash->addData(chunk);
        };
        QNetworkReply* image = manager->get(release_request(QString::fromLatin1(kAppImage)));
        QObject::connect(image, &QNetworkReply::readyRead, image, [image, take] { take(image->readAll()); });
        QObject::connect(image, &QNetworkReply::finished, UpdateCheck::instance(),
                         [image, out, hash, write_failed, take, manager, target, partial, sha1] {
            take(image->readAll());
            bool written = !*write_failed && out->flush() && image->error() == QNetworkReply::NoError;
            delete write_failed;
            out->close();
            written = written && out->error() == QFileDevice::NoError;
            const QByteArray got = hash->result().toHex();
            delete out;
            delete hash;
            image->deleteLater();
            manager->deleteLater();
            if (!written || got != sha1) {
                QFile::remove(partial);
                fail_update(written ? QStringLiteral("The download did not match the release.")
                                    : QStringLiteral("The download did not finish."));
                return;
            }
            ::chmod(QFile::encodeName(partial).constData(), 0755);
            // The running AppImage keeps its open copy; the next start gets the new one.
            if (std::rename(QFile::encodeName(partial).constData(),
                            QFile::encodeName(target).constData()) != 0) {
                QFile::remove(partial);
                fail_update(QStringLiteral("The new AppImage could not replace %1.").arg(target));
                return;
            }
            g_updating = false;
            std::printf("stud: updated the AppImage to %s\n", qPrintable(UpdateCheck::latestVersion()));
            send_notification(QStringLiteral("Stud updated"),
                              QStringLiteral("Restart Stud to use version %1.").arg(UpdateCheck::latestVersion()));
        });
    });
}

}  // namespace

UpdateCheck::UpdateCheck(QObject* parent) : QObject(parent) {}

void UpdateCheck::open() {
    const QString appimage = qEnvironmentVariable("APPIMAGE");
    if (!appimage.isEmpty() && QFileInfo(appimage).isFile()) {
        update_appimage(appimage);
        return;
    }
    QDesktopServices::openUrl(QUrl(releasesUrl()));
}

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

    auto* manager = new QNetworkAccessManager(instance());
    QNetworkRequest request((QUrl(QString::fromLatin1(kLatestApi))));
    request.setRawHeader("Accept", "application/vnd.github+json");
    // GitHub returns 403 without a User-Agent.
    request.setRawHeader("User-Agent", "Stud/" STUD_VERSION);
    request.setTransferTimeout(8000);

    QNetworkReply* reply = manager->get(request);
    QObject::connect(reply, &QNetworkReply::finished, instance(), [reply]() {
        reply->deleteLater();
        if (reply->error() != QNetworkReply::NoError) return;  // failures are silent

        const QJsonDocument doc = QJsonDocument::fromJson(reply->readAll());
        if (!doc.isObject()) return;
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
