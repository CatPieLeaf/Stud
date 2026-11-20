// The channel Roblox assigned is read from its saved plist, production being
// none; and what Roblox's own installer left in Library/Roblox is removed,
// with nothing else there touched.

#include "mac_client.h"
#include "check.h"

#include <QDir>
#include <QFile>
#include <QTemporaryDir>
#include <cstdio>

namespace {

void write(const QString& path, const QByteArray& bytes) {
    QDir().mkpath(path.left(path.lastIndexOf('/')));
    QFile out(path);
    if (out.open(QIODevice::WriteOnly)) out.write(bytes);
}

QByteArray plist(const char* value) {
    return QByteArray("<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n<plist version=\"1.0\">\n<dict>\n"
                      "\t<key>www.roblox.com</key>\n\t<string>") +
           value + "</string>\n</dict>\n</plist>\n";
}

}  // namespace

int main() {
    using stud::ui::client::Product;
    QTemporaryDir root;
    const QString home = root.path() + QStringLiteral("/Users/me/Library");

    expect(stud::ui::client::assigned_channel(root.path(), Product::Player).isEmpty(),
           "no plist is production");
    write(home + QStringLiteral("/Preferences/com.roblox.RobloxPlayerChannel.plist"), plist("zbisect006"));
    write(home + QStringLiteral("/Preferences/com.roblox.RobloxStudioChannel.plist"), plist("production"));
    expect(stud::ui::client::assigned_channel(root.path(), Product::Player) == QStringLiteral("zbisect006"),
           "the Player's assigned channel is read");
    expect(stud::ui::client::assigned_channel(root.path(), Product::Studio).isEmpty(),
           "production reads as none");

    write(home + QStringLiteral("/Roblox/RobloxPlayer.zip"), "zip");
    write(home + QStringLiteral("/Roblox/RobloxPlayer.app/Contents/Info.plist"), "plist");
    write(home + QStringLiteral("/Roblox/GlobalBasicSettings_13.xml"), "settings");
    write(home + QStringLiteral("/Roblox/LocalStorage/appStorage.json"), "{}");
    const QStringList removed = stud::ui::client::remove_installer_leftovers(root.path());
    expect(removed.size() == 2, "the zip and the unpacked bundle are removed");
    expect(!QFile::exists(home + QStringLiteral("/Roblox/RobloxPlayer.zip")), "the zip is gone");
    expect(!QFile::exists(home + QStringLiteral("/Roblox/RobloxPlayer.app")), "the bundle is gone");
    expect(QFile::exists(home + QStringLiteral("/Roblox/GlobalBasicSettings_13.xml")) &&
               QFile::exists(home + QStringLiteral("/Roblox/LocalStorage/appStorage.json")),
           "Roblox's settings and storage stay");
    expect(stud::ui::client::remove_installer_leftovers(root.path()).isEmpty(), "a second pass finds nothing");

    return finish("release_channel_test");
}
