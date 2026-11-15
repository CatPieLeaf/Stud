// The Flatpak permission check: what /.flatpak-info grants, overrides and
// revocations included, against what Stud needs.

#include "flatpak_permissions.h"

#include <cstdio>

using stud::ui::missing_flatpak_permissions;

static int failures = 0;

static void expect(bool ok, const char* what) {
    if (!ok) {
        std::fprintf(stderr, "FAIL: %s\n", what);
        ++failures;
    }
}

int main() {
    const QString full = QStringLiteral(
        "[Application]\nname=io.github.catpieleaf.Stud\n\n"
        "[Context]\nshared=network;ipc;\nsockets=x11;wayland;pulseaudio;fallback-x11;\n"
        "devices=dri;input;\n\n[Session Bus Policy]\norg.freedesktop.Notifications=talk\n");
    expect(missing_flatpak_permissions(full).isEmpty(), "the manifest's permissions miss nothing");

    const QString revoked = QStringLiteral(
        "[Context]\nshared=ipc;\nsockets=pulseaudio;!wayland;\ndevices=input;\n");
    const QStringList m = missing_flatpak_permissions(revoked);
    expect(m == QStringList({QStringLiteral("--share=network"), QStringLiteral("--device=dri"),
                             QStringLiteral("--socket=wayland")}),
           "revoked network, dri and display are each named");

    const QString all = QStringLiteral(
        "[Context]\nshared=network;\nsockets=fallback-x11;pulseaudio;\ndevices=all;\n");
    expect(missing_flatpak_permissions(all).isEmpty(), "devices=all covers dri and input");

    // A key of the same name outside [Context] grants nothing.
    const QString elsewhere = QStringLiteral(
        "[Instance]\nshared=network;\n[Context]\nsockets=wayland;pulseaudio;\ndevices=all;\n");
    expect(missing_flatpak_permissions(elsewhere) == QStringList{QStringLiteral("--share=network")},
           "only [Context] counts");

    if (failures == 0) std::printf("flatpak_permissions_test: ok\n");
    return failures == 0 ? 0 : 1;
}
