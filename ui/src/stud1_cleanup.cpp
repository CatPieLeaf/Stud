#include "stud1_cleanup.h"

#include <QDir>
#include <QDirIterator>
#include <QFileInfo>

namespace stud::ui {

qint64 disk_size(const QString& path) {
    const QFileInfo info(path);
    if (info.isSymLink() || !info.isDir()) return info.isSymLink() ? 0 : info.size();
    qint64 total = 0;
    QDirIterator it(path, QDir::Files | QDir::Hidden | QDir::System | QDir::NoSymLinks,
                    QDirIterator::Subdirectories);
    while (it.hasNext()) {
        it.next();
        total += it.fileInfo().size();
    }
    return total;
}

}  // namespace stud::ui

namespace stud::ui::stud1 {

namespace {

// Everything Stud 1.x wrote under the data and cache dirs, none of 2.0's (client/,
// prefix/, studio/, icons/, webview/, history.json; webview/ in cache).
constexpr const char* kData[] = {
    "apk",           // the imported Roblox APK
    "files",         // the Android engine's files directory
    "shared_prefs",  // its preferences
    "secrets",       // the 1.x login, encrypted
};
constexpr const char* kCache[] = {
    "libroblox.so", "libroblox.so.source",  // the engine, extracted
    "assets", "assets.ready", "assets.source", "android",  // the APK's assets
    "engine",             // the engine's asset store and patches
    "cache",              // its cache directory
    "textures",           // the texture cache
    "desktop-materials",  // Windows materials
};

bool remove(const QString& path) {
    const QFileInfo info(path);
    // A link is removed as a link, never by deleting what it points at.
    if (info.isSymLink() || !info.isDir()) return QFile::remove(path);
    return QDir(path).removeRecursively();
}

}  // namespace

Removed remove_remnants(const QString& data_dir, const QString& cache_dir) {
    Removed removed;
    const auto sweep = [&](const QString& root, const auto& names) {
        for (const char* name : names) {
            const QString path = root + QLatin1Char('/') + QLatin1String(name);
            const QFileInfo info(path);
            if (!info.exists() && !info.isSymLink()) continue;
            // Links aren't followed: 1.x linked parts of its files directory out to the cache.
            const qint64 bytes = disk_size(path);
            if (remove(path)) {
                removed.paths << path;
                removed.bytes += bytes;
            }
        }
    };
    sweep(data_dir, kData);
    sweep(cache_dir, kCache);
    return removed;
}

}  // namespace stud::ui::stud1
