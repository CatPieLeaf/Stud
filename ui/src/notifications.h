#pragma once

#include <QString>

// Desktop notifications via org.freedesktop.Notifications over D-Bus (the
// notification portal inside a Flatpak), or a Mac-style banner in the running
// client when the notifications setting asks.

namespace stud::ui {

// The socket the client shows banners from (ROSEMARY_NOTIFICATIONS), and the
// file it keeps its window's state in (ROSEMARY_WINDOW_STATE), Studio's apart.
QString banner_socket_path(bool studio);
QString window_state_path(bool studio);

// Notify in the running client's banner when the setting and its window ask for
// it, otherwise over D-Bus; false when neither shows it. `icon` is drawn large,
// so leave it empty unless the notification is about something with a picture;
// Stud's own icon comes from the desktop-entry hint.
struct NotificationOptions {
    // No sound and no history entry, for short-lived notes like a join's server region.
    bool transient = false;
    bool suppress_sound = false;
};

bool send_notification(const QString& summary, const QString& body, const QString& icon = QString(),
                        const NotificationOptions& options = {});

}  // namespace stud::ui
