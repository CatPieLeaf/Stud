#include "open_location.h"

#include <QDesktopServices>
#include <QProcess>

namespace stud::ui {

void open_location(const QUrl& url) {
    // cpak has no desktop portal, and its xdg-open refuses file:// but maps a plain
    // path in the sandbox's home to where that home really is on the host.
    if (url.isLocalFile() && qEnvironmentVariableIsSet("CPAK_CONTAINER_ID") &&
        QProcess::startDetached(QStringLiteral("xdg-open"), {url.toLocalFile()})) {
        return;
    }
    QDesktopServices::openUrl(url);
}

}  // namespace stud::ui
