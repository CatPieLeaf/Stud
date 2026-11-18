#pragma once

#include <QObject>
#include <QString>

class QSystemTrayIcon;
class QMenu;
class QAction;

// Tray menu for the session: Settings, logs, Exit. Closing Roblox still quits Stud,
// since a hidden session would keep the character in a server.

namespace stud::ui {

class Tray : public QObject {
    Q_OBJECT

public:
    explicit Tray(QObject* parent = nullptr);

    // False without a status-notifier host; the caller then quits as before.
    bool show();

private slots:
    void openSettings();
    void copyServerLink();
    void exportLogs();
    void showAbout();
    void quitStud();
    // Switches the icon and adds an update entry.
    void showUpdateAvailable(const QString& latestVersion);

private:
    QMenu* menu_ = nullptr;
    QAction* updateAction_ = nullptr;
    void addUpdateEntry(QAction* action, const QString& toolTip);

    QSystemTrayIcon* icon_ = nullptr;
};

}  // namespace stud::ui
