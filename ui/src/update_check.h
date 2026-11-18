#pragma once

#include <QObject>
#include <QString>

namespace stud::ui {

// Asks GitHub once per run, in the background, whether a newer Stud exists. Sends
// nothing about the user; any failure is silent. Roblox itself is updated from
// its CDN before every launch.
class UpdateCheck : public QObject {
    Q_OBJECT

public:
    // releases/latest, which stays valid after the next release.
    static QString releasesUrl();

    // Safe to call again: the result is remembered for the process.
    static void start();

    static bool updateAvailable();
    static QString latestVersion();

    // An AppImage downloads the new one, checks it against the .zsync's SHA-1 and
    // replaces itself for the next start; other packages open the release page.
    static void open();

    static UpdateCheck* instance();

signals:
    // Only emitted when a newer version exists.
    void updateFound(const QString& latestVersion);

private:
    explicit UpdateCheck(QObject* parent = nullptr);
};

}  // namespace stud::ui
