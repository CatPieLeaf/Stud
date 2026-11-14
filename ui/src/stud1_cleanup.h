#pragma once

#include <QString>
#include <QStringList>

// What Stud 1.x left behind, removed the first time Stud 2.0 runs.
//
// Stud 1.x ran Roblox's Android client. Its engine, the APK it was extracted
// from, the extracted assets and every cache built from them mean nothing to
// the macOS client, and they run to gigabytes. What carries over stays where
// it is, because 2.0 reads it from the same place: config.json (whose 1.x
// names the settings loader maps, see stud-config), flags.json, the
// customizations folders and the session logs. The 1.x login does not carry
// over: the macOS client signs in for itself.
namespace stud::ui::stud1 {

struct Removed {
    QStringList paths;   // what was removed, absolute
    qint64 bytes = 0;    // the space it took
};

// Removes Stud 1.x's own entries under `data_dir` and `cache_dir` (Stud's
// ~/.local/share/stud and ~/.cache/stud) and nothing else. Empty when there
// was nothing of 1.x's there, which is every run after the first.
Removed remove_remnants(const QString& data_dir, const QString& cache_dir);

}  // namespace stud::ui::stud1
