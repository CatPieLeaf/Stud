#include "tray.h"

#include "log_archive.h"

#include "settings_window.h"

#include "stud/stud_paths.h"

// Ends a running session -- defined in main.cpp, declared here because
// the tray is the only other caller.
namespace stud::ui { void terminate_stud_session(); }

#include <QApplication>
#include <QClipboard>
#include <QFile>
#include <QDir>
#include <QDateTime>
#include <QFileDialog>
#include <QFileInfo>
#include <QStandardPaths>
#include <QIcon>
#include <QMenu>
#include <QMessageBox>
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
// render protocol. A real https link to that one server -- not an
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
// -- nothing unlinks it -- so the tray would have sat there forever
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

}  // namespace

Tray::Tray(QObject* parent) : QObject(parent) {}

bool Tray::show() {
    if (!QSystemTrayIcon::isSystemTrayAvailable()) return false;

    // A tray-only process has no windows, and Qt quits an application
    // when its last window closes. So opening Settings from the tray and
    // closing it again ended the process -- taking the tray icon with it,
    // for the rest of the session, while the game carried on running with
    // no way back to the menu.
    //
    // The session's own lifetime is what ends this process (see
    // checkSessionAlive), not whether a window happens to be open.
    QApplication::setQuitOnLastWindowClosed(false);

    icon_ = new QSystemTrayIcon(QIcon(":/stud-logo-color.png"), this);
    icon_->setToolTip(QStringLiteral("Stud"));

    auto* menu = new QMenu();
    menu->addAction(QStringLiteral("Stud settings"), this, &Tray::openSettings);
    menu->addAction(QStringLiteral("Copy server link"), this, &Tray::copyServerLink);
    menu->addAction(QStringLiteral("Export logs"), this, &Tray::exportLogs);
    menu->addSeparator();
    menu->addAction(QStringLiteral("About Stud"), this, &Tray::showAbout);
    menu->addSeparator();
    menu->addAction(QStringLiteral("Exit Stud"), this, &Tray::quitStud);
    icon_->setContextMenu(menu);
    icon_->show();

    // The tray must not outlive the session it belongs to: once the game
    // is gone there is nothing for it to be a menu for, and an icon left
    // behind is worse than no icon.
    auto* watchdog = new QTimer(this);
    connect(watchdog, &QTimer::timeout, this, &Tray::checkSessionAlive);
    watchdog->start(2000);
    return true;
}

void Tray::checkSessionAlive() {
    if (session_is_running()) return;
    if (icon_ != nullptr) icon_->hide();
    QApplication::quit();
}

void Tray::openSettings() {
    // Owned by nothing and deleted on close, so opening it twice does not
    // leak and closing it does not end the session.
    auto* window = new SettingsWindow();
    window->setAttribute(Qt::WA_DeleteOnClose);
    window->show();
    window->raise();
    window->activateWindow();
}

void Tray::copyServerLink() {
    const QString link = read_server_link();
    if (link.isEmpty()) {
        icon_->showMessage(QStringLiteral("Stud"),
                            QStringLiteral("Not in a game, so there is no server to link to."),
                            QSystemTrayIcon::Information, 4000);
        return;
    }
    QApplication::clipboard()->setText(link);
    icon_->showMessage(QStringLiteral("Stud"), QStringLiteral("Server link copied."),
                        QSystemTrayIcon::Information, 3000);
}

void Tray::exportLogs() {
    // Every session, not just this one.
    //
    // A problem worth reporting is often visible in the run BEFORE the
    // one the user noticed it in -- a crash, a setting that did not take,
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
    QMessageBox::about(nullptr, QStringLiteral("About Stud"),
                        QStringLiteral("<b>Stud</b> " STUD_VERSION "<br><br>"
                                       "A Linux desktop wrapper that runs the real, unmodified "
                                       "Roblox Android app.<br><br>"
                                       "<a href=\"https://github.com/CatPieLeaf/Stud\">"
                                       "github.com/CatPieLeaf/Stud</a>"));
}

void Tray::quitStud() {
    // Ends the session, not just this process: the tray's Exit means what
    // closing the window means.
    if (icon_ != nullptr) icon_->hide();
    terminate_stud_session();
    QApplication::quit();
}

}  // namespace stud::ui
