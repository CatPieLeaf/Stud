#include "project_links.h"
#include "tray.h"

#include "mac_client.h"

#include "game_activity.h"
#include "log_export.h"
#include "notifications.h"

#include "settings_window.h"

#include "stud/stud_paths.h"

// Defined in main.cpp.
namespace stud::ui { void kill_stud_session(); }
#include "update_check.h"

#include <QApplication>
#include <QClipboard>
#include <QDBusConnection>
#include <QDBusMessage>
#include <QDBusPendingCallWatcher>
#include <QDBusReply>
#include <QDBusVariant>

#include <cstdio>
#include <QDir>
#include <QDateTime>
#include <QFileDialog>
#include <QFileInfo>
#include <QStandardPaths>
#include <QIcon>
#include <QAction>
#include <QDialog>
#include <QDialogButtonBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QVBoxLayout>
#include <QStyle>
#include <QMenu>
#include <QMessageBox>
#include <QFontMetrics>
#include <QPixmap>
#include <QPointer>
#include <QProcess>
#include <QSystemTrayIcon>
#include <QTimer>


#include <cstdlib>
#include <string>

namespace stud::ui {

namespace {

// Qt asks the bus whether org.kde.StatusNotifierWatcher has an owner. A sandbox
// that refuses that question (cpak before #78) gets no tray icon at all.
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
    if (!system_tray_exists()) return false;

    // A tray-only process has no windows, so closing Settings would quit it and
    // take the tray icon away mid-game. The client's exit ends this process.
    QApplication::setQuitOnLastWindowClosed(false);

    icon_ = new QSystemTrayIcon(QIcon(":/stud-logo-color.png"), this);
    icon_->setToolTip(QStringLiteral("Stud"));

    auto* menu = new QMenu();
    // Kept for the update entry, added if the check finds one.
    menu_ = menu;
    menu->addAction(QIcon::fromTheme(QStringLiteral("configure"), QIcon::fromTheme(QStringLiteral("preferences-system"))),
                    tr("Stud settings"), this, &Tray::openSettings);
    // Shown only while in a server someone else can join.
    QAction* copy_link = menu->addAction(QIcon::fromTheme(QStringLiteral("edit-copy")),
                                         tr("Copy server link"), this, &Tray::copyServerLink);
    copy_link->setVisible(false);
    connect(menu, &QMenu::aboutToShow, this,
            [copy_link] { copy_link->setVisible(!current_server_link().isEmpty()); });
    menu->addAction(QIcon::fromTheme(QStringLiteral("document-save")), tr("Export logs"), this,
                    &Tray::exportLogs);
    menu->addSeparator();
    menu->addAction(QIcon::fromTheme(QStringLiteral("help-about")), tr("About Stud"), this, &Tray::showAbout);
    menu->addSeparator();
    menu->addAction(QIcon::fromTheme(QStringLiteral("application-exit")), tr("Exit Stud"), this,
                    &Tray::quitStud);
    icon_->setContextMenu(menu);
    icon_->show();

    // Qt registers the item asynchronously and stays silent if the watcher never
    // takes it; read the watcher's list back to tell which happened.
    QTimer::singleShot(2000, this, [] {
        QDBusMessage ask = QDBusMessage::createMethodCall(
            QStringLiteral("org.kde.StatusNotifierWatcher"),
            QStringLiteral("/StatusNotifierWatcher"),
            QStringLiteral("org.freedesktop.DBus.Properties"),
            QStringLiteral("Get"));
        ask.setArguments({QStringLiteral("org.kde.StatusNotifierWatcher"),
                          QStringLiteral("RegisteredStatusNotifierItems")});

        auto* watcher = new QDBusPendingCallWatcher(QDBusConnection::sessionBus().asyncCall(ask, 3000));
        QObject::connect(watcher, &QDBusPendingCallWatcher::finished, watcher, [](QDBusPendingCallWatcher* call) {
            call->deleteLater();
            const QDBusMessage reply = call->reply();
            if (reply.type() == QDBusMessage::ErrorMessage) {
                std::fprintf(stderr, "stud: could not read the tray's item list (%s)\n",
                             qUtf8Printable(reply.errorName()));
                return;
            }
            const QString self = QDBusConnection::sessionBus().baseService();
            const QStringList items = reply.arguments().value(0).value<QDBusVariant>().variant().toStringList();
            // Entries read ":1.47/StatusNotifierItem"; the separator stops ":1.4" matching.
            for (const QString& item : items) {
                if (item.startsWith(self + QLatin1Char('/'))) {
                    std::fprintf(stderr, "stud: the tray accepted this session's icon\n");
                    return;
                }
            }
            std::fprintf(stderr, "stud: the tray did not take this session's icon; it holds %lld other item(s)\n",
                         static_cast<long long>(items.size()));
        });
    });

    connect(UpdateCheck::instance(), &UpdateCheck::updateFound, this, &Tray::showUpdateAvailable);
    if (UpdateCheck::updateAvailable()) showUpdateAvailable(UpdateCheck::latestVersion());
    UpdateCheck::start();

    return true;
}

// Changes the icon as well as the menu, since nobody sees a menu entry without
// opening the menu. No popup: an update isn't worth interrupting a game.
void Tray::showUpdateAvailable(const QString& latestVersion) {
    if (icon_ == nullptr || menu_ == nullptr || updateAction_ != nullptr) return;
    updateAction_ = new QAction(tr("\u26a0\ufe0f Update Stud"), menu_);
    connect(updateAction_, &QAction::triggered, this, [] { UpdateCheck::open(); });
    icon_->setIcon(QIcon(QStringLiteral(":/stud-logo-update.png")));
    icon_->setToolTip(tr("Stud: version %1 is available").arg(latestVersion));
    // At the top: the only entry that is news.
    menu_->insertAction(menu_->actions().value(0), updateAction_);
    menu_->insertSeparator(menu_->actions().value(1));
}

void Tray::openSettings() {
    SettingsWindow::showSingleton();
}

void Tray::copyServerLink() {
    const QString link = current_server_link();
    if (link.isEmpty()) return;  // left the game while the menu was open
    // Wayland only accepts a clipboard offer tied to an input event on one of the
    // app's surfaces, and the tray menu isn't one, so wl-copy serves it from a
    // process of its own. Qt's clipboard still covers X11.
    if (qEnvironmentVariableIsSet("WAYLAND_DISPLAY")) {
        QProcess copier;
        copier.setProgram(QStringLiteral("wl-copy"));
        copier.setArguments({QStringLiteral("--type"), QStringLiteral("text/plain")});
        copier.setStandardOutputFile(QProcess::nullDevice());
        copier.setStandardErrorFile(QProcess::nullDevice());
        copier.start();
        if (!copier.waitForStarted(2000)) {
            send_notification(QStringLiteral("Stud"),
                              tr("Could not copy: this desktop needs wl-clipboard installed."));
            return;
        }
        copier.write(link.toUtf8());
        copier.closeWriteChannel();
        // wl-copy forks and keeps serving the selection.
        copier.waitForFinished(2000);
    }
    QApplication::clipboard()->setText(link);
    send_notification(QStringLiteral("Stud"), tr("Server link copied."), QString(),
                      {/*transient=*/true, /*suppress_sound=*/false});
}

void Tray::exportLogs() {
    const QString saved = export_all_logs(nullptr);
    if (!saved.isEmpty()) {
        send_notification(QStringLiteral("Stud"), saved);
    }
}

void Tray::showAbout() {
    // One dialog however often it's clicked; QMessageBox::about makes a new one each time.
    static QPointer<QDialog> about;
    if (about.isNull()) {
        about = new QDialog();
        about->setWindowTitle(tr("About Stud"));
        about->setAttribute(Qt::WA_DeleteOnClose);
        auto* layout = new QVBoxLayout(about);
        layout->setSizeConstraint(QLayout::SetFixedSize);
        auto* top = new QHBoxLayout();
        // The square icon; a wide wordmark would shrink to a sliver.
        constexpr int kIcon = 96;
        auto* icon = new QLabel(about);
        const QPixmap logo(QStringLiteral(":/stud-logo-color.png"));
        if (!logo.isNull()) {
            icon->setPixmap(logo.scaled(kIcon, kIcon, Qt::KeepAspectRatio, Qt::SmoothTransformation));
        }
        top->addWidget(icon, 0, Qt::AlignTop);
        auto* side = new QVBoxLayout();
        auto* header = new QHBoxLayout();
        auto* title = new QLabel(about);
        title->setPixmap(QPixmap(QStringLiteral(":/stud-title.png"))
                             .scaledToHeight(kIcon * 42 / 100, Qt::SmoothTransformation));
        auto* lead = new QVBoxLayout();
        lead->addWidget(title, 0, Qt::AlignLeft | Qt::AlignTop);
        lead->addSpacing(about->style()->pixelMetric(QStyle::PM_LayoutVerticalSpacing));
        auto* text = new QLabel(tr("Runs Roblox's real, unmodified macOS client on Linux."), about);
        text->setWordWrap(true);
        // Two lines, each two thirds of the one-line width. A fixed width, since a
        // wrapping label would otherwise shrink the fixed-size dialog to three lines.
        const QFontMetrics metrics(text->font());
        text->setFixedWidth(metrics.horizontalAdvance(text->text()) * 2 / 3);
        lead->addWidget(text);
        lead->addStretch();
        header->addLayout(lead, 1);
        auto* credits = new QVBoxLayout();
        auto* version = new QLabel(tr("Version %1").arg(QString::fromLatin1(STUD_VERSION)), about);
        auto* by = new QLabel(tr("By %1").arg(QStringLiteral("CatPieLeaf")), about);
        credits->addWidget(version, 0, Qt::AlignRight);
        credits->addWidget(by, 0, Qt::AlignRight);
        credits->addStretch();
        header->addLayout(credits);
        side->addLayout(header);
        side->addStretch();
        top->addLayout(side, 1);
        layout->addLayout(top);
        layout->addSpacing(2 * about->style()->pixelMetric(QStyle::PM_LayoutVerticalSpacing));
        auto* bottom = new QHBoxLayout();
        bottom->addWidget(github_button(about));
        bottom->addWidget(discord_button(about));
        bottom->addStretch();
        auto* close = new QDialogButtonBox(QDialogButtonBox::Close, about);
        QObject::connect(close, &QDialogButtonBox::rejected, about, &QDialog::reject);
        bottom->addWidget(close);
        layout->addLayout(bottom);
    }
    about->show();
    about->raise();
    about->activateWindow();
}

void Tray::quitStud() {
    // Kills the whole session at once, so a frozen Roblox ends without a task manager.
    if (icon_ != nullptr) icon_->hide();
    kill_stud_session();
    QApplication::quit();
}

}  // namespace stud::ui
