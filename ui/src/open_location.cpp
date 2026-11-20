#include "open_location.h"

#include <QDBusConnection>
#include <QDBusInterface>
#include <QDBusMessage>
#include <QDBusUnixFileDescriptor>
#include <QFile>
#include <QDesktopServices>
#include <QVariantMap>

#include <fcntl.h>
#include <unistd.h>

namespace stud::ui {

namespace {

// Through the desktop portal, which reaches the host's handlers from a container
// such as cpak's, where Qt's xdg-open fallback has nothing to run. The portal takes
// a local file as a descriptor rather than a file:// address.
bool open_through_portal(const QUrl& url) {
    QDBusInterface portal(QStringLiteral("org.freedesktop.portal.Desktop"),
                          QStringLiteral("/org/freedesktop/portal/desktop"),
                          QStringLiteral("org.freedesktop.portal.OpenURI"), QDBusConnection::sessionBus());
    if (!portal.isValid()) return false;
    QDBusMessage reply;
    if (url.isLocalFile()) {
        const int fd = ::open(QFile::encodeName(url.toLocalFile()).constData(), O_PATH | O_CLOEXEC);
        if (fd < 0) return false;
        reply = portal.call(QStringLiteral("OpenFile"), QString(), QVariant::fromValue(QDBusUnixFileDescriptor(fd)),
                            QVariantMap());
        ::close(fd);
    } else {
        reply = portal.call(QStringLiteral("OpenURI"), QString(), url.toString(), QVariantMap());
    }
    return reply.type() == QDBusMessage::ReplyMessage;
}

}  // namespace

void open_location(const QUrl& url) {
    if (!open_through_portal(url)) QDesktopServices::openUrl(url);
}

}  // namespace stud::ui
