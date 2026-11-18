#pragma once

#include <QString>
#include <QStringList>

namespace stud::ui {

// Packs `files` into a .tar.gz under one top-level folder named after the archive.
// `client_logs` go under roblox/ with join tickets scrubbed. On failure, `error`
// is user-readable.
bool write_log_tarball(const QString& target, const QStringList& files, QString* error,
                       const QStringList& client_logs = {});

}  // namespace stud::ui
