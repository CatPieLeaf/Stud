#include "flatpak_permissions.h"

#include <QHash>
#include <QtGlobal>

namespace stud::ui {

namespace {

// key -> its granted values, from the [Context] group; "!x" revokes x.
QHash<QString, QStringList> context_of(const QString& flatpak_info) {
    QHash<QString, QStringList> context;
    bool in_context = false;
    for (const QString& raw : flatpak_info.split(QLatin1Char('\n'))) {
        const QString line = raw.trimmed();
        if (line.startsWith(QLatin1Char('['))) {
            in_context = line == QLatin1String("[Context]");
            continue;
        }
        const qsizetype equals = line.indexOf(QLatin1Char('='));
        if (!in_context || equals <= 0) continue;
        QStringList values;
        for (const QString& v : line.mid(equals + 1).split(QLatin1Char(';'), Qt::SkipEmptyParts)) {
            if (!v.startsWith(QLatin1Char('!'))) values << v;
        }
        context.insert(line.left(equals), values);
    }
    return context;
}

bool has(const QHash<QString, QStringList>& context, const char* key, const char* value) {
    return context.value(QString::fromLatin1(key)).contains(QString::fromLatin1(value));
}

}  // namespace

QStringList missing_flatpak_permissions(const QString& flatpak_info) {
    const auto context = context_of(flatpak_info);
    QStringList missing;
    if (!has(context, "shared", "network")) missing << QStringLiteral("--share=network");
    if (!has(context, "devices", "all") && !has(context, "devices", "dri")) missing << QStringLiteral("--device=dri");
    if (!has(context, "sockets", "pulseaudio")) missing << QStringLiteral("--socket=pulseaudio");
    if (!has(context, "sockets", "wayland") && !has(context, "sockets", "x11") &&
        !has(context, "sockets", "fallback-x11")) {
        missing << QStringLiteral("--socket=wayland");
    }
    return missing;
}

QStringList missing_discord_permissions(const QString& flatpak_info) {
    // Any one of Discord's sockets, or a sandboxed Discord's directory, is enough.
    for (const QString& fs : context_of(flatpak_info).value(QStringLiteral("filesystems"))) {
        const QString path = fs.section(QLatin1Char(':'), 0, 0);
        if (path == QLatin1String("xdg-run") || path.startsWith(QLatin1String("xdg-run/discord-ipc-")) ||
            path == QLatin1String("xdg-run/app/com.discordapp.Discord") ||
            path == QLatin1String("xdg-run/app/dev.vencord.Vesktop")) {
            return {};
        }
    }
    return {QStringLiteral("--filesystem=xdg-run/discord-ipc-0"),
            QStringLiteral("--filesystem=xdg-run/app/com.discordapp.Discord:create"),
            QStringLiteral("--filesystem=xdg-run/app/dev.vencord.Vesktop:create")};
}

QStringList missing_controller_permissions(const QString& flatpak_info) {
    const auto context = context_of(flatpak_info);
    if (has(context, "devices", "all") || has(context, "devices", "input")) return {};
    return {QStringLiteral("--device=input")};
}

QString flatpak_override_command(const QStringList& options) {
    return QStringLiteral("flatpak override --user %1 %2")
        .arg(options.join(QLatin1Char(' ')), qEnvironmentVariable("FLATPAK_ID", QStringLiteral(STUD_APP_ID)));
}

}  // namespace stud::ui
