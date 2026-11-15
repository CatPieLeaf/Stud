#include "flatpak_permissions.h"

#include <QHash>

namespace stud::ui {

QStringList missing_flatpak_permissions(const QString& flatpak_info) {
    // key -> its granted values, from the [Context] group; "!x" revokes x.
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
    const auto has = [&](const char* key, const char* value) {
        return context.value(QString::fromLatin1(key)).contains(QString::fromLatin1(value));
    };
    const bool all_devices = has("devices", "all");

    QStringList missing;
    if (!has("shared", "network")) missing << QStringLiteral("--share=network");
    if (!all_devices && !has("devices", "dri")) missing << QStringLiteral("--device=dri");
    if (!all_devices && !has("devices", "input")) missing << QStringLiteral("--device=input");
    if (!has("sockets", "pulseaudio")) missing << QStringLiteral("--socket=pulseaudio");
    if (!has("sockets", "wayland") && !has("sockets", "x11") && !has("sockets", "fallback-x11")) {
        missing << QStringLiteral("--socket=wayland");
    }
    return missing;
}

}  // namespace stud::ui
