#pragma once

#include <QObject>
#include <QString>

class QSystemTrayIcon;
class QMenu;
class QAction;

// Stud's system-tray presence, and the reason Process A now outlives a
// launch at all.
//
// Process A was built to hand off and exit -- "a magnet link launcher
// does not need to stay open". A tray icon has to belong to a process
// that lives as long as the session does, and of the three only Process A
// is a Qt application: render-host holds the window and Process B is
// sandboxed. So with the tray enabled Process A stays, doing nothing but
// owning the icon; with it disabled the old behaviour is unchanged and it
// quits immediately.
//
// Closing the window still quits Stud. The tray is a menu, not a place to
// hide the app: a session left running invisibly still holds the GPU and
// still holds the player's character in a server.

namespace stud::ui {

class Tray : public QObject {
    Q_OBJECT

public:
    explicit Tray(QObject* parent = nullptr);

    // False when no system tray is available -- a desktop without a
    // status-notifier host, which is an ordinary condition rather than an
    // error. The caller then quits as it always did.
    bool show();

private slots:
    void openSettings();
    void copyServerLink();
    void exportLogs();
    void showAbout();
    void quitStud();
    // A newer release exists: change the icon and add an entry for it.
    void showUpdateAvailable(const QString& latestVersion);

private:
    // The menu, kept so the update entry can join it later, and the entry
    // itself, kept so it is only ever added once.
    QMenu* menu_ = nullptr;
    QAction* updateAction_ = nullptr;

    // Whether the session is still running, checked so the tray does not
    // outlive the game it belongs to.
    void checkSessionAlive();

    QSystemTrayIcon* icon_ = nullptr;
};

}  // namespace stud::ui
