#pragma once

#include <QString>
#include <QStringList>

namespace stud::ui {

// Packs `files` into a gzipped tar archive at `target`.
//
// Every entry is stored under a single top-level directory named after
// the archive, so unpacking one in a downloads folder leaves one folder
// rather than a scatter of session logs.
//
// Returns false and fills `error` with something worth showing a user.
bool write_log_tarball(const QString& target, const QStringList& files, QString* error);

}  // namespace stud::ui
