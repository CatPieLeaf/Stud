#include "notifications.h"

#include <QDBusConnection>
#include <QDBusInterface>
#include <QDBusMessage>
#include <QDBusVariant>
#include <QDBusReply>
#include <QFile>
#include <QLocalSocket>
#include <QStringList>
#include <QVariantMap>

#include <cstdio>
#include <exception>

#include "stud/settings.h"
#include "stud/stud_paths.h"

namespace stud::ui {

QString banner_socket_path(bool studio) {
    return QString::fromStdString(stud::paths::runtime_dir()) +
           (studio ? QStringLiteral("/notifications-studio") : QStringLiteral("/notifications"));
}

QString window_state_path(bool studio) {
    return QString::fromStdString(stud::paths::state_dir()) +
           (studio ? QStringLiteral("/window-studio") : QStringLiteral("/window"));
}

namespace {

// The client writes its window's mode first: normal, maximized or fullscreen.
bool fullscreen(bool studio) {
    QFile file(window_state_path(studio));
    return file.open(QIODevice::ReadOnly) &&
           file.readLine().trimmed().split(' ').value(0) == QByteArrayLiteral("fullscreen");
}

// Into a running client's banner, when the setting asks for it there; a client
// that is not running refuses the connection.
bool show_in_client(const QString& summary, const QString& body) {
    std::string where = stud::config::StudSettings().notifications;
    try {
        where = stud::config::load_settings(stud::config::default_config_path()).notifications;
    } catch (const std::exception&) {
    }
    if (where == "system") return false;
    for (const bool studio : {false, true}) {
        if (where == "fullscreen" && !fullscreen(studio)) continue;
        QLocalSocket socket;
        socket.connectToServer(banner_socket_path(studio), QIODevice::WriteOnly);
        if (!socket.waitForConnected()) continue;
        socket.write((summary + QLatin1Char('\n') + body).toUtf8());
        socket.disconnectFromServer();
        if (socket.state() != QLocalSocket::UnconnectedState) socket.waitForDisconnected();
        return true;
    }
    return false;
}

// Through the notification portal, which a Flatpak reaches without a D-Bus
// permission of its own; the portal gives it the app's icon itself.
bool show_through_portal(const QString& summary, const QString& body, const NotificationOptions& options) {
    QDBusInterface portal("org.freedesktop.portal.Desktop", "/org/freedesktop/portal/desktop",
                          "org.freedesktop.portal.Notification", QDBusConnection::sessionBus());
    if (!portal.isValid()) return false;
    QVariantMap notification{{QStringLiteral("title"), summary},
                             {QStringLiteral("body"), body},
                             {QStringLiteral("priority"), options.transient ? QStringLiteral("low")
                                                                            : QStringLiteral("normal")}};
    // Version 2 added these; an older portal may refuse keys it doesn't know.
    if (portal.property("version").toUInt() >= 2) {
        if (options.transient) notification.insert(QStringLiteral("display-hint"), QStringList{QStringLiteral("transient")});
        if (options.suppress_sound) {
            notification.insert(QStringLiteral("sound"), QVariant::fromValue(QDBusVariant(QStringLiteral("silent"))));
        }
    }
    static uint sent = 0;
    const QDBusMessage reply = portal.call(QStringLiteral("AddNotification"),
                                           QStringLiteral("stud-%1").arg(sent++), notification);
    return reply.type() != QDBusMessage::ErrorMessage;
}

}  // namespace

bool send_notification(const QString& summary, const QString& body, const QString& icon,
                        const NotificationOptions& options) {
    if (show_in_client(summary, body)) {
        std::printf("stud: notification \"%s\" shown in the client\n", qPrintable(summary));
        return true;
    }
    std::printf("stud: notification \"%s\" sent to the desktop\n", qPrintable(summary));
    if (stud::config::in_flatpak()) return show_through_portal(summary, body, options);
    QDBusInterface iface("org.freedesktop.Notifications", "/org/freedesktop/Notifications",
                          "org.freedesktop.Notifications", QDBusConnection::sessionBus());
    if (!iface.isValid()) {
        return false;
    }

    // expire_timeout -1 uses the server's default; replaces_id 0 is always a new notification.
    QVariantMap hints;
    if (options.transient) hints.insert("transient", true);
    if (options.suppress_sound) hints.insert("suppress-sound", true);
    // desktop-entry puts Stud's icon in the header and lets the server group it and
    // apply per-app settings; it must be the entry's basename, the application id.
    // app_icon stays empty: Plasma would draw it large down the side.
    hints.insert("desktop-entry", QStringLiteral(STUD_APP_ID));
    QDBusReply<uint> reply = iface.call("Notify", QString("Stud"), 0u, icon, summary, body,
                                         QStringList(), hints, -1);
    return reply.isValid();
}

}  // namespace stud::ui
