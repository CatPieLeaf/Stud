// The Flatpak permission check: what /.flatpak-info grants, overrides and
// revocations included, against what Stud needs.

#include "flatpak_permissions.h"
#include "check.h"

#include <cstdio>

using stud::ui::missing_controller_permissions;
using stud::ui::missing_discord_permissions;
using stud::ui::missing_flatpak_permissions;

int main() {
    const QString full = QStringLiteral(
        "[Application]\nname=io.github.catpieleaf.Stud\n\n"
        "[Context]\nshared=network;ipc;\nsockets=x11;wayland;pulseaudio;fallback-x11;\n"
        "devices=dri;\n\n[Session Bus Policy]\norg.freedesktop.Notifications=talk\n");
    expect(missing_flatpak_permissions(full).isEmpty(), "the manifest's permissions miss nothing");

    const QString revoked = QStringLiteral(
        "[Context]\nshared=ipc;\nsockets=pulseaudio;!wayland;\ndevices=input;\n");
    const QStringList m = missing_flatpak_permissions(revoked);
    expect(m == QStringList({QStringLiteral("--share=network"), QStringLiteral("--device=dri"),
                             QStringLiteral("--socket=wayland")}),
           "revoked network, dri and display are each named");

    const QString all = QStringLiteral(
        "[Context]\nshared=network;\nsockets=fallback-x11;pulseaudio;\ndevices=all;\n");
    expect(missing_flatpak_permissions(all).isEmpty(), "devices=all covers dri");
    expect(missing_controller_permissions(all).isEmpty(), "devices=all covers input");

    // A key of the same name outside [Context] grants nothing.
    const QString elsewhere = QStringLiteral(
        "[Instance]\nshared=network;\n[Context]\nsockets=wayland;pulseaudio;\ndevices=all;\n");
    expect(missing_flatpak_permissions(elsewhere) == QStringList{QStringLiteral("--share=network")},
           "only [Context] counts");

    // Discord and controllers are opt-in: the manifest grants neither.
    expect(missing_controller_permissions(full) == QStringList{QStringLiteral("--device=input")},
           "controllers need --device=input");
    expect(missing_controller_permissions(QStringLiteral("[Context]\ndevices=dri;input;\n")).isEmpty(),
           "an input override covers controllers");
    expect(missing_discord_permissions(full).size() == 3, "Discord's sockets are missing by default");
    expect(missing_discord_permissions(QStringLiteral("[Context]\nfilesystems=xdg-run/discord-ipc-0;\n")).isEmpty(),
           "one Discord socket is enough");
    expect(missing_discord_permissions(
               QStringLiteral("[Context]\nfilesystems=xdg-run/app/com.discordapp.Discord:create;\n")).isEmpty(),
           "a Flatpak Discord's directory is enough");
    expect(missing_discord_permissions(QStringLiteral("[Context]\nfilesystems=!xdg-run/discord-ipc-0;\n")).size() == 3,
           "a revoked socket counts as missing");

    return finish("flatpak_permissions_test");
}
