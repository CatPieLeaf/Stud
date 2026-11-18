#pragma once

#include <QString>

// Desktop notifications via org.freedesktop.Notifications over D-Bus.

namespace stud::ui {

// Notify over D-Bus; false when no notification server answered. `icon` is drawn
// large, so leave it empty unless the notification is about something with a picture;
// Stud's own icon comes from the desktop-entry hint.
struct NotificationOptions {
    // No sound and no history entry, for short-lived notes like a join's server region.
    bool transient = false;
    bool suppress_sound = false;
};

bool send_notification(const QString& summary, const QString& body, const QString& icon = QString(),
                        const NotificationOptions& options = {});

}  // namespace stud::ui
