#include "tray.h"

#include "log_archive.h"

#include "settings_window.h"

#include "stud/stud_paths.h"

// Ends a running session, defined in main.cpp, declared here because
// the tray is the only other caller.
namespace stud::ui { void terminate_stud_session(); }
// Defined in main.cpp: whether a crash report is on screen right now.
namespace stud::ui { bool crash_dialog_is_open(); }
// Defined in main.cpp: whether the session that just ended said it
// meant to. See note_clean_exit().
namespace stud::ui { bool session_ended_cleanly(); }

#include "update_check.h"

#include <QApplication>
#include <QClipboard>
#include <QDBusConnection>
#include <QDBusMessage>
#include <QDBusReply>
#include <QDBusVariant>

#include <cstdio>
#include <QFile>
#include <QDir>
#include <QDateTime>
#include <QFileDialog>
#include <QFileInfo>
#include <QStandardPaths>
#include <QIcon>
#include <QAction>
#include <QDesktopServices>
#include <QMenu>
#include <QMessageBox>
#include <QPixmap>
#include <QPointer>
#include <QProcess>
#include <QSystemTrayIcon>
#include <QTextStream>
#include <QTimer>

#include <fcntl.h>
#include <sys/file.h>
#include <unistd.h>

#include <cstdlib>
#include <string>

namespace stud::ui {

namespace {

// Where render-host records the link to the server the player is
// currently in, so the tray can offer it without needing to speak the
// render protocol. A real https link to that one server, not an
// invite: minting one of those needs the Lua SocialService inside an
// experience (see ui/src/main.cpp's --game-info). Written on every presence change and removed on the
// way back to the app shell, so its absence means "not in a game".
QString server_link_path() {
    if (const char* xdg = std::getenv("XDG_RUNTIME_DIR")) {
        return QString::fromUtf8(xdg) + "/stud/invite";
    }
    return QStringLiteral("/tmp/stud/invite");
}

QString read_server_link() {
    QFile file(server_link_path());
    if (!file.open(QIODevice::ReadOnly | QIODevice::Text)) return {};
    return QTextStream(&file).readAll().trimmed();
}

// The session is render-host: it lives exactly as long as the game does.
//
// Tested with the same flock render-host holds for its whole life, not by
// looking for its socket file. A socket file outlives a SIGKILLed process
// nothing unlinks it, so the tray would have sat there forever
// believing a dead session was alive. The kernel releases a lock however
// the holder dies, so this cannot go stale.
bool session_is_running() {
    const char* xdg = std::getenv("XDG_RUNTIME_DIR");
    const std::string path =
        std::string(xdg != nullptr ? xdg : "/tmp") + "/stud/stud.lock";
    const int fd = ::open(path.c_str(), O_RDWR | O_CREAT | O_CLOEXEC, 0600);
    if (fd < 0) return true;  // Cannot tell: do not kill the tray over it.
    const bool free_to_take = ::flock(fd, LOCK_EX | LOCK_NB) == 0;
    if (free_to_take) ::flock(fd, LOCK_UN);
    ::close(fd);
    return !free_to_take;
}

// Whether a system tray exists, as Qt sees it.
//
// Qt asks the bus daemon whether org.kde.StatusNotifierWatcher has an owner.
// A sandbox that mediates the daemon can refuse that question; cpak refuses
// any policy naming org.freedesktop.DBus, which is the daemon itself.
//
// Ignoring the refusal and registering with the watcher directly does not
// work, and it is a tempting enough idea to be worth ruling out here. On a
// refusal the Qt platform theme builds no tray icon at all
// (QGenericUnixTheme::createPlatformSystemTrayIcon returns null), so
// QSystemTrayIcon::show() has nothing to show. Stud cannot reach around that.
//
// The sandbox has to allow the question. cpak does now: a policy granting
// talk to a name may ask who owns that name, and a client owning a name
// receives calls addressed to it.
bool system_tray_exists() {
    if (QSystemTrayIcon::isSystemTrayAvailable()) return true;
    std::fprintf(stderr,
                 "stud: no system tray; the session bus reports no "
                 "org.kde.StatusNotifierWatcher\n");
    return false;
}

}  // namespace

Tray::Tray(QObject* parent) : QObject(parent) {}

bool Tray::show() {
    // An absent icon gives no way to tell a trayless desktop from a failed
    // registration, so both cases say which one happened.
    if (!system_tray_exists()) return false;

    // A tray-only process has no windows, and Qt quits an application
    // when its last window closes. So opening Settings from the tray and
    // closing it again ended the process, taking the tray icon with it,
    // for the rest of the session, while the game carried on running with
    // no way back to the menu.
    //
    // The session's own lifetime is what ends this process (see
    // checkSessionAlive), not whether a window happens to be open.
    QApplication::setQuitOnLastWindowClosed(false);

    icon_ = new QSystemTrayIcon(QIcon(":/stud-logo-color.png"), this);
    icon_->setToolTip(QStringLiteral("Stud"));

    auto* menu = new QMenu();
    // Kept so the update entry can be added to it later, when and if the
    // check comes back saying there is one.
    menu_ = menu;
    menu->addAction(QStringLiteral("Stud settings"), this, &Tray::openSettings);
    menu->addAction(QStringLiteral("Copy server link"), this, &Tray::copyServerLink);
    menu->addAction(QStringLiteral("Export logs"), this, &Tray::exportLogs);
    menu->addSeparator();
    menu->addAction(QStringLiteral("About Stud"), this, &Tray::showAbout);
    menu->addSeparator();
    menu->addAction(QStringLiteral("Exit Stud"), this, &Tray::quitStud);
    icon_->setContextMenu(menu);
    icon_->show();

    // Qt registers the item asynchronously and says nothing when the watcher
    // never takes it, which looks the same as an icon that failed to draw.
    // Read the watcher's own list back and report which happened.
    QTimer::singleShot(2000, this, [] {
        QDBusMessage ask = QDBusMessage::createMethodCall(
            QStringLiteral("org.kde.StatusNotifierWatcher"),
            QStringLiteral("/StatusNotifierWatcher"),
            QStringLiteral("org.freedesktop.DBus.Properties"),
            QStringLiteral("Get"));
        ask.setArguments({QStringLiteral("org.kde.StatusNotifierWatcher"),
                          QStringLiteral("RegisteredStatusNotifierItems")});

        const QDBusMessage reply =
            QDBusConnection::sessionBus().call(ask, QDBus::Block, 3000);
        if (reply.type() == QDBusMessage::ErrorMessage) {
            std::fprintf(stderr, "stud: could not read the tray's item list (%s)\n",
                         qUtf8Printable(reply.errorName()));
            return;
        }

        const QString self = QDBusConnection::sessionBus().baseService();
        const QStringList items =
            reply.arguments().value(0).value<QDBusVariant>().variant().toStringList();
        // Entries read ":1.47/StatusNotifierItem". Matching on the unique
        // name plus its separator stops ":1.4" matching ":1.47".
        for (const QString& item : items) {
            if (item.startsWith(self + QLatin1Char('/'))) {
                std::fprintf(stderr, "stud: the tray accepted this session's icon\n");
                return;
            }
        }
        std::fprintf(stderr,
                     "stud: the tray did not take this session's icon; it holds "
                     "%lld other item(s)\n",
                     static_cast<long long>(items.size()));
    });

    // Ask GitHub whether there is a newer Stud, once, in the background.
    // Nothing appears unless there is; see UpdateCheck.
    connect(UpdateCheck::instance(), &UpdateCheck::updateFound, this, &Tray::showUpdateAvailable);
    if (UpdateCheck::updateAvailable()) showUpdateAvailable(UpdateCheck::latestVersion());
    UpdateCheck::start();

    // The tray must not outlive the session it belongs to: once the game
    // is gone there is nothing for it to be a menu for, and an icon left
    // behind is worse than no icon.
    auto* watchdog = new QTimer(this);
    connect(watchdog, &QTimer::timeout, this, &Tray::checkSessionAlive);
    watchdog->start(2000);
    return true;
}

// A newer release exists: say so in the one place a user is already
// looking, and give them somewhere to go.
//
// The icon changes as well as the menu, because a menu entry nobody opens
// the menu to see is not a notification. It is deliberately NOT a popup:
// an update is not urgent enough to interrupt a game.
void Tray::showUpdateAvailable(const QString& latestVersion) {
    if (icon_ == nullptr || updateAction_ != nullptr) return;
    icon_->setIcon(QIcon(QStringLiteral(":/stud-logo-update.png")));
    icon_->setToolTip(QStringLiteral("Stud: version %1 is available").arg(latestVersion));
    if (menu_ != nullptr) {
        updateAction_ = new QAction(QStringLiteral("\u26a0\ufe0f Update Stud"), menu_);
        connect(updateAction_, &QAction::triggered, this, [] {
            QDesktopServices::openUrl(QUrl(UpdateCheck::releasesUrl()));
        });
        // At the top, above Settings: it is the only entry here that is
        // news rather than a thing the user came to do.
        menu_->insertAction(menu_->actions().value(0), updateAction_);
        menu_->insertSeparator(menu_->actions().value(1));
    }
}

void Tray::checkSessionAlive() {
    if (session_is_running()) return;
    // A crash is exactly when the session stops running, so this watchdog
    // and the crash report fire together -- and quitting here closes the
    // report before it can be read. Live-caught: the dialog appeared for
    // a split second on every crash, which looked like a Qt modality
    // problem and was really this. The report owns the shutdown while it
    // is up; closing it quits.
    if (stud::ui::crash_dialog_is_open()) return;
    // And defer BEFORE it is up, too. Waiting for the dialog to exist is
    // a race between two one-second timers: this one noticing the
    // session is gone, and the crash watcher deciding to report it. The
    // clean-exit note settles it without racing -- if the session ended
    // without one, a report is coming, and quitting now would eat it.
    if (!stud::ui::session_ended_cleanly()) return;
    if (icon_ != nullptr) icon_->hide();
    QApplication::quit();
}

void Tray::openSettings() {
    // One window, whether the request came from here or from the desktop
    // entry's own Settings action; see SettingsWindow::showSingleton().
    SettingsWindow::showSingleton();
}

void Tray::copyServerLink() {
    const QString link = read_server_link();
    if (link.isEmpty()) {
        icon_->showMessage(QStringLiteral("Stud"),
                            QStringLiteral("Not in a game, so there is no server to link to."),
                            QSystemTrayIcon::Information, 4000);
        return;
    }
    // On Wayland a clipboard offer is only accepted with the serial of a
    // real input event on one of the application's own surfaces, and
    // this application has no window at all while it sits in the tray
    // (the menu is the desktop's, over D-Bus, not a Qt surface). So
    // QClipboard::setText was silently ignored and the entry copied
    // nothing, which is exactly what was reported.
    //
    // wl-copy owns the selection in its own short-lived process, which
    // is the normal way a windowless program copies on Wayland. Qt's own
    // clipboard is still set as well: it is what works on X11, and it
    // costs nothing where it does not.
    const bool wayland = qEnvironmentVariableIsSet("WAYLAND_DISPLAY");
    if (wayland) {
        QProcess copier;
        copier.setProgram(QStringLiteral("wl-copy"));
        copier.setArguments({QStringLiteral("--type"), QStringLiteral("text/plain")});
        copier.setStandardOutputFile(QProcess::nullDevice());
        copier.setStandardErrorFile(QProcess::nullDevice());
        copier.start();
        if (copier.waitForStarted(2000)) {
            copier.write(link.toUtf8());
            copier.closeWriteChannel();
            // wl-copy forks and keeps serving the selection, so this
            // returns as soon as the text has been handed over.
            copier.waitForFinished(2000);
        } else {
            icon_->showMessage(
                QStringLiteral("Stud"),
                QStringLiteral("Could not copy: this desktop needs wl-clipboard installed."),
                QSystemTrayIcon::Warning, 5000);
            return;
        }
    }
    QApplication::clipboard()->setText(link);
    icon_->showMessage(QStringLiteral("Stud"), QStringLiteral("Server link copied."),
                        QSystemTrayIcon::Information, 3000);
}

void Tray::exportLogs() {
    // Every session, not just this one.
    //
    // A problem worth reporting is often visible in the run BEFORE the
    // one the user noticed it in: a crash, a setting that did not take,
    // a launch that went to the home screen. Exporting only the current
    // session made the user pick which run mattered, from a directory
    // they have no reason to know exists, before they knew.
    const QDir log_dir(QString::fromStdString(stud::paths::log_dir()));
    const QStringList sessions =
        log_dir.entryList({QStringLiteral("session-*.log")}, QDir::Files, QDir::Time);
    if (sessions.isEmpty()) {
        QMessageBox::information(nullptr, QStringLiteral("Stud"),
                                  QStringLiteral("There are no logs to export yet."));
        return;
    }
    QStringList files;
    files.reserve(sessions.size());
    for (const QString& name : sessions) files << log_dir.filePath(name);

    const QString name = QStringLiteral("stud-logs-%1.tar.gz")
                             .arg(QDateTime::currentDateTime().toString(
                                 QStringLiteral("yyyyMMdd-HHmmss")));
    const QString suggested =
        QDir(QStandardPaths::writableLocation(QStandardPaths::DownloadLocation)).filePath(name);
    QString target = QFileDialog::getSaveFileName(
        nullptr, QStringLiteral("Export Stud's logs"), suggested,
        QStringLiteral("Archives (*.tar.gz);;All files (*)"));
    if (target.isEmpty()) return;
    // A user who types a bare name in the dialog means the archive, not a
    // file with no type.
    if (!target.endsWith(QStringLiteral(".tar.gz")) && !target.endsWith(QStringLiteral(".tgz"))) {
        target += QStringLiteral(".tar.gz");
    }

    QString error;
    if (!write_log_tarball(target, files, &error)) {
        QMessageBox::warning(nullptr, QStringLiteral("Stud"),
                              QStringLiteral("Could not export the logs: %1").arg(error));
        return;
    }
    icon_->showMessage(
        QStringLiteral("Stud"),
        QStringLiteral("%1 log%2 saved to %3")
            .arg(files.size())
            .arg(files.size() == 1 ? QString() : QStringLiteral("s"), target),
        QSystemTrayIcon::Information, 4000);
}

void Tray::showAbout() {
    // One card, however many times the entry is clicked. QMessageBox::about
    // builds a new dialog every call, so a few clicks left a stack of
    // identical windows, the same reason Settings is a singleton.
    static QPointer<QMessageBox> about;
    if (about.isNull()) {
        about = new QMessageBox(QMessageBox::NoIcon, QStringLiteral("About Stud"),
                                QStringLiteral("<b>Stud</b> " STUD_VERSION "<br><br>"
                                               "A Linux desktop wrapper that runs the real, "
                                               "unmodified Roblox Android app.<br><br>"
                                               "<a href=\"https://github.com/CatPieLeaf/Stud\">"
                                               "CatPieLeaf/Stud</a>"),
                                QMessageBox::Ok);
        about->setAttribute(Qt::WA_DeleteOnClose);
        about->setTextFormat(Qt::RichText);
        about->setTextInteractionFlags(Qt::TextBrowserInteraction);
        // The ICON, not the wordmark. A dialog's icon slot is square, and
        // stud-logo.png is the wide 3840x2160 logo, fitting that into 96
        // square leaves a sliver a few pixels tall. stud-logo-color.png is
        // the square one, and is already what the tray and the window use.
        const QPixmap icon(QStringLiteral(":/stud-logo-color.png"));
        if (!icon.isNull()) {
            about->setIconPixmap(
                icon.scaled(96, 96, Qt::KeepAspectRatio, Qt::SmoothTransformation));
        }
    }
    about->show();
    about->raise();
    about->activateWindow();
}

void Tray::quitStud() {
    // Ends the session, not just this process: the tray's Exit means what
    // closing the window means.
    if (icon_ != nullptr) icon_->hide();
    terminate_stud_session();
    QApplication::quit();
}

}  // namespace stud::ui
