// The release-channel guard: a saved test channel is emptied, so Roblox reads
// production; a production one and Roblox's other preferences are left alone.

#include "mac_client.h"

#include <QDir>
#include <QFile>
#include <QTemporaryDir>
#include <cstdio>

namespace {

int failures = 0;

void expect(bool ok, const char* what) {
    if (!ok) {
        std::fprintf(stderr, "FAILED: %s\n", what);
        ++failures;
    }
}

QByteArray plist(const char* value) {
    return QByteArray("<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n<plist version=\"1.0\">\n<dict>\n"
                      "\t<key>www.roblox.com</key>\n\t<string>") +
           value + "</string>\n</dict>\n</plist>\n";
}

void write(const QString& path, const QByteArray& bytes) {
    QFile out(path);
    if (out.open(QIODevice::WriteOnly)) out.write(bytes);
}

QByteArray read(const QString& path) {
    QFile in(path);
    return in.open(QIODevice::ReadOnly) ? in.readAll() : QByteArray();
}

}  // namespace

int main() {
    QTemporaryDir root;
    const QString prefs = root.path() + QStringLiteral("/Users/me/Library/Preferences");
    QDir().mkpath(prefs);
    const QString player = prefs + QStringLiteral("/com.roblox.RobloxPlayerChannel.plist");
    const QString studio = prefs + QStringLiteral("/com.roblox.RobloxStudioChannel.plist");
    const QString other = prefs + QStringLiteral("/com.roblox.RobloxPlayer.plist");
    write(player, plist("zaflag"));
    write(studio, plist("production"));
    write(other, plist("zaflag"));

    const QStringList reset = stud::ui::client::reset_release_channels(root.path());
    expect(reset == QStringList{QStringLiteral("zaflag")}, "the test channel is reported");
    expect(!read(player).contains("zaflag") && read(player).contains("<dict/>"), "the test channel is emptied");
    expect(read(studio) == plist("production"), "production is left as it is");
    expect(read(other) == plist("zaflag"), "other preferences are not touched");
    expect(stud::ui::client::reset_release_channels(root.path()).isEmpty(), "a second pass finds nothing");

    if (failures == 0) std::printf("release_channel_test: ok\n");
    return failures == 0 ? 0 : 1;
}
