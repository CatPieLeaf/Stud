#pragma once

#include <QString>
#include <QStringList>

// Removes Stud 1.x's Android engine, APK, assets and caches (gigabytes) on 2.0's
// first run. config.json, flags.json, customizations and logs carry over in place;
// the login doesn't, since the macOS client signs in itself.
namespace stud::ui::stud1 {

struct Removed {
    QStringList paths;  // absolute
    qint64 bytes = 0;
};

// Only 1.x's entries under the data and cache dirs. Empty after the first run.
Removed remove_remnants(const QString& data_dir, const QString& cache_dir);

}  // namespace stud::ui::stud1
