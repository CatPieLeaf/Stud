// Stud 1.x's leftovers go on 2.0's first start; 2.0's files and carried-over ones stay.

#include "stud1_cleanup.h"
#include "check.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QTemporaryDir>

#include <cstdio>

namespace {


void write(const QString& path, qint64 bytes) {
    QDir().mkpath(QFileInfo(path).absolutePath());
    QFile file(path);
    file.open(QIODevice::WriteOnly);
    file.write(QByteArray(bytes, 'x'));
}

bool exists(const QString& path) { return QFileInfo::exists(path) || QFileInfo(path).isSymLink(); }

}  // namespace

int main() {
    QTemporaryDir root;
    const QString data = root.path() + QStringLiteral("/share/stud");
    const QString cache = root.path() + QStringLiteral("/cache/stud");
    const QString elsewhere = root.path() + QStringLiteral("/elsewhere");

    // Stud 1.x's layout.
    write(data + "/apk/roblox.apk", 1000);
    write(data + "/files/exe/settings", 100);
    write(data + "/shared_prefs/prefs.xml", 10);
    write(data + "/secrets/cookie", 10);
    write(cache + "/libroblox.so", 2000);
    write(cache + "/assets/android/textures/studs.dds", 300);
    write(cache + "/engine/store", 50);
    write(cache + "/textures/v/a", 40);
    // A link target outside the swept directories must survive.
    write(elsewhere + "/keep", 7);
    QFile::link(elsewhere, data + "/files/linked");

    // Stud 2.0's own, and what it carries over from 1.x.
    write(data + "/prefix/root/marker", 5);
    write(data + "/client/version-x/marker", 5);
    write(data + "/webview/marker", 5);
    write(data + "/history.json", 5);
    write(cache + "/webview/marker", 5);

    const auto removed = stud::ui::stud1::remove_remnants(data, cache);
    expect(!exists(data + "/apk") && !exists(data + "/files") && !exists(data + "/shared_prefs") &&
              !exists(data + "/secrets"),
          "Stud 1.x's data entries are removed, the 1.x login among them");
    expect(!exists(cache + "/libroblox.so") && !exists(cache + "/assets") &&
              !exists(cache + "/engine") && !exists(cache + "/textures"),
          "Stud 1.x's cache entries are removed");
    expect(exists(elsewhere + "/keep"), "a link is removed as a link, not followed");
    expect(exists(data + "/prefix/root/marker") && exists(data + "/client/version-x/marker") &&
              exists(data + "/webview/marker") && exists(data + "/history.json") &&
              exists(cache + "/webview/marker"),
          "Stud 2.0's own files are left alone");
    expect(removed.paths.size() == 8, "each removed entry is reported");
    expect(removed.bytes == 1000 + 100 + 10 + 10 + 2000 + 300 + 50 + 40,
          "the space freed is what the files took, links not followed");

    const auto again = stud::ui::stud1::remove_remnants(data, cache);
    expect(again.paths.isEmpty() && again.bytes == 0, "a second start finds nothing to remove");

    return finish("stud1 cleanup");
}
