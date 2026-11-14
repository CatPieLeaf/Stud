#include "stud1_cleanup.h"

#include <QDir>
#include <QDirIterator>
#include <QFileInfo>

namespace stud::ui::stud1 {

namespace {

// Every entry Stud 1.x wrote under its data and cache directories
// (android-glue/include/stud/stud_paths.h and its users on 1.x's main), and
// none Stud 2.0 writes there. 2.0's own are client/, prefix/, studio/,
// icons/, webview/ and history.json in data, and webview/ in cache.
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

// What `path` takes on disk, links not followed: 1.x linked parts of its
// files directory out to the cache.
qint64 size_of(const QString& path) {
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
            const qint64 bytes = size_of(path);
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
