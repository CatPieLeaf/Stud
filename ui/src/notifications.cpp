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

    // Real Notify(app_name, replaces_id, app_icon, summary, body,
    // actions, hints, expire_timeout) signature per the freedesktop.org
    // spec -- app_name "Stud" (real, our own app identity, not
    // impersonating anything), replaces_id 0 (always a new
    // notification, not replacing a prior one -- this mechanism doesn't
    // track IDs across calls yet, no real caller needs that yet),
    // expire_timeout -1 (respect the notification server's own default
    // duration).
    QVariantMap hints;
    // Real freedesktop hints, all understood by every server that
    // implements the spec and ignored harmlessly by one that does not.
    if (options.transient) hints.insert("transient", true);
    if (options.suppress_sound) hints.insert("suppress-sound", true);
    // Names the desktop entry this notification belongs to, and this is
    // what puts Stud's icon on it -- as the small application badge in
    // the header, which is the whole point. A server ties the
    // notification back to the application from this: Plasma draws the
    // entry's icon beside the app name, groups by it, and honours any
    // per-application settings the user has set for Stud. It must be the
    // entry's own basename, which is the application id -- the same
    // identity the window carries via its app_id.
    //
    // It used to be the literal "stud", from back when the entry was
    // named that. Once the entry became io.github.catpieleaf.Stud.desktop
    // there was no entry by the old name for a server to find, and every
    // notification arrived with no icon at all.
    //
    // Deliberately NOT app_icon (the third argument below, left as
    // whatever the caller passed, i.e. empty): that is the notification's
    // own picture and Plasma draws it large, down the left-hand side. An
    // application identifying itself does not want a poster of its own
    // logo on every notification it sends.
    hints.insert("desktop-entry", QStringLiteral(STUD_APP_ID));
    QDBusReply<uint> reply = iface.call("Notify", QString("Stud"), 0u, icon, summary, body,
                                         QStringList(), hints, -1);
    return reply.isValid();
}

}  // namespace stud::ui
