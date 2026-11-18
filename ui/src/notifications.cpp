#include "notifications.h"

#include <QDBusConnection>
#include <QDBusInterface>
#include <QDBusReply>
#include <QStringList>
#include <QVariantMap>

namespace stud::ui {

bool send_notification(const QString& summary, const QString& body, const QString& icon,
                        const NotificationOptions& options) {
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
