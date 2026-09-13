#pragma once

#include <QString>

// Real freedesktop.org Notifications (M8, locked decision: "Generic
// freedesktop Notifications spec (org.freedesktop.Notifications) via
// QtDBus -- works on any DE, not just Plasma, matching AppImage's
// run-anywhere goal"). Generic mechanism -- specific features that use
// it (the server-location indicator, real IP geolocation, opt-in) are
// separate, not-yet-built M10-adjacent work; this is the underlying
// send-a-notification primitive they'll call.

namespace stud::ui {

// Real, synchronous D-Bus call to org.freedesktop.Notifications.Notify.
// Returns true if the notification server accepted it (a valid,
// non-error D-Bus reply) -- false if no notification daemon is running,
// or the call otherwise failed. `icon` is the notification's own picture
// -- a freedesktop icon name (e.g. "dialog-information") -- and a server
// draws it large, so leave it empty unless the notification is genuinely
// about a thing with a picture. Stud's own identity does not go here: it
// travels as the "desktop-entry" hint, which is what puts the small
// application badge on every notification Stud sends.
// How the notification server should treat it.
struct NotificationOptions {
    // No sound, and not kept in the notification history. Both are real
    // freedesktop hints ("suppress-sound", "transient"): a server-region
    // note on every join is information for the moment it appears, and a
    // sound plus a permanent entry per join is noise.
    bool transient = false;
    bool suppress_sound = false;
};

bool send_notification(const QString& summary, const QString& body, const QString& icon = QString(),
                        const NotificationOptions& options = {});

}  // namespace stud::ui
