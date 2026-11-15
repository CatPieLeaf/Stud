#include "roblox_settings.h"

#include "mac_client.h"

#include <QDateTime>
#include <QDir>
#include <QFileInfo>
#include <QRegularExpression>

namespace stud::ui::roblox_settings {

namespace {

QRegularExpression element(const QString& name) {
    return QRegularExpression(QStringLiteral(R"(<(\w+) name="%1">([^<]*)</\1>)")
                                  .arg(QRegularExpression::escape(name)));
}

}  // namespace

QString player_file() {
    const QDir users(client::prefix_dir() + QStringLiteral("/root/Users"));
    QString newest;
    QDateTime newest_time;
    for (const QString& user : users.entryList(QDir::Dirs | QDir::NoDotAndDotDot)) {
        const QDir roblox(users.filePath(user) + QStringLiteral("/Library/Roblox"));
        for (const QFileInfo& f :
             roblox.entryInfoList({QStringLiteral("GlobalBasicSettings_*.xml")}, QDir::Files)) {
            if (f.fileName().endsWith(QLatin1String("_Studio.xml"))) continue;
            if (newest.isEmpty() || f.lastModified() > newest_time) {
                newest = f.absoluteFilePath();
                newest_time = f.lastModified();
            }
        }
    }
    return newest;
}

std::optional<std::pair<QString, QString>> read(const QString& xml, const QString& name) {
    const QRegularExpressionMatch m = element(name).match(xml);
    if (!m.hasMatch()) return std::nullopt;
    return std::pair{m.captured(1), m.captured(2)};
}

QString write(const QString& xml, const QString& name, const QString& text) {
    const QRegularExpressionMatch m = element(name).match(xml);
    if (!m.hasMatch()) return xml;
    QString out = xml;
    out.replace(m.capturedStart(2), m.capturedLength(2), text);
    return out;
}

}  // namespace stud::ui::roblox_settings
