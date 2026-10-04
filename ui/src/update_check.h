#pragma once

#include <QObject>
#include <QString>

namespace stud::ui {

// Whether a newer Stud has been released, asked of GitHub once per run,
// and whether the imported Roblox build is behind the one Roblox ships.
//
// One GET to the releases API, at startup, in the background. It sends
// nothing about the machine or the user, the request carries no
// identity, and the reply is one version string that is compared with
// this build's own. A failure of any kind (offline, rate-limited,
// unparseable) is silence: the check exists to be helpful, and an
// application that complains about not reaching the internet when the
// user did not ask it to is not.
class UpdateCheck : public QObject {
    Q_OBJECT

public:
    // The release page a user is sent to. `latest` rather than a
    // version-specific URL, so it keeps working after the next release.
    static QString releasesUrl();

    // Starts the check. Safe to call more than once; the result is
    // remembered for the life of the process, so the second caller,
    // Settings, when the tray has already asked, costs nothing.
    static void start();

    // What the check found, for a window opened after it finished.
    static bool updateAvailable();
    static QString latestVersion();

    // Whether the imported Roblox build is older than the one Roblox ships
    // now. Read against the stored APK each time, so importing a newer
    // one clears it without asking the network again.
    static bool robloxOutdated();

    // The one instance, so both the tray and Settings can connect.
    static UpdateCheck* instance();

signals:
    // Emitted once, if and only if a newer version exists.
    void updateFound(const QString& latestVersion);
    // Emitted once, if and only if the imported Roblox build is outdated.
    void robloxOutdatedFound();

private:
    explicit UpdateCheck(QObject* parent = nullptr);
};

}  // namespace stud::ui
