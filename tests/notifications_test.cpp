// Real freedesktop Notifications via QtDBus (ui/src/notifications.h). Sends a
// genuine notification through this machine's session bus, so a popup appears
// on screen during the run. Skipped where there is no notification service.

#include "notifications.h"
#include "check.h"

#include <QCoreApplication>
#include <QDBusConnection>
#include <QDBusConnectionInterface>

#include <cstdio>
#include <cstdlib>

int main(int argc, char** argv) {
    // QtDBus needs a running Qt event loop / QCoreApplication to
    // dispatch its real session-bus connection.
    QCoreApplication app(argc, argv);

    QDBusConnection bus = QDBusConnection::sessionBus();
    const QString service = QStringLiteral("org.freedesktop.Notifications");
    if (!bus.isConnected() || (!bus.interface()->isServiceRegistered(service) &&
                               !bus.interface()->activatableServiceNames().value().contains(service))) {
        std::printf("no notification service on this machine\n");
        return kSkipped;
    }

    bool sent = stud::ui::send_notification("Stud test", "From ctest. Nothing happened.");
    check(sent, "send_notification() returns true against the real, live session bus");

    std::printf("all notifications checks passed\n");
    return 0;
}
