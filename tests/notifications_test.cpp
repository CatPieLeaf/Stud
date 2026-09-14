// M8 test: real freedesktop Notifications via QtDBus
// (ui/src/notifications.h). Sends a genuine notification through this
// machine's real, live D-Bus session bus and notification daemon,
// not a mock. If this passes, a real notification popup appeared on
// screen during this test run.

#include "notifications.h"

#include <QCoreApplication>

#include <cstdio>
#include <cstdlib>

namespace {

void check(bool condition, const char* what) {
    if (!condition) {
        std::fprintf(stderr, "FAILED: %s\n", what);
        std::exit(1);
    }
    std::printf("ok: %s\n", what);
}

}  // namespace

int main(int argc, char** argv) {
    // QtDBus needs a running Qt event loop / QCoreApplication to
    // dispatch its real session-bus connection.
    QCoreApplication app(argc, argv);

    bool sent = stud::ui::send_notification("Meow", "Meow");
    check(sent, "send_notification() returns true against the real, live session bus");

    std::printf("all notifications checks passed\n");
    return 0;
}
