#pragma once

#include <QObject>
#include <QString>

class QSystemTrayIcon;
class QMenu;
class QAction;

// Stud's system-tray presence: Settings, logs and Exit for the session.
// It belongs to stud-ui, which lives exactly as long as the client does.
//
// Closing Roblox still quits Stud. The tray is a menu, not a place to hide
// the game: a session left running invisibly still holds the player's
// character in a server.

namespace stud::ui {

class Tray : public QObject {
    Q_OBJECT

public:
    explicit Tray(QObject* parent = nullptr);

    // False when no system tray is available, a desktop without a
    // status-notifier host, which is an ordinary condition rather than an
    // error. The caller then quits as it always did.
    bool show();

private slots:
    void openSettings();
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
    // Puts an update entry at the top of the menu and switches the icon.
    void addUpdateEntry(QAction* action, const QString& toolTip);

    QSystemTrayIcon* icon_ = nullptr;
};

}  // namespace stud::ui
