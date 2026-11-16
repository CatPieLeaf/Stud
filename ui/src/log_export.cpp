#include "log_export.h"

#include "log_archive.h"
#include "mac_client.h"
#include "stud/stud_paths.h"

#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileDialog>
#include <QFileInfo>
#include <QMessageBox>
#include <QStandardPaths>

namespace stud::ui {

namespace {

// The session logs, newest first. No symlinks: the sandbox can write this
// directory, and what is exported is made to be shared.
QStringList session_logs() {
    const QDir log_dir(QString::fromStdString(stud::paths::log_dir()));
    QStringList files;
    for (const QString& name : log_dir.entryList({QStringLiteral("session-*.log")},
                                                 QDir::Files | QDir::NoSymLinks, QDir::Time)) {
        files << log_dir.filePath(name);
    }
    return files;
}

QString in_downloads(const QString& name) {
    return QDir(QStandardPaths::writableLocation(QStandardPaths::DownloadLocation)).filePath(name);
}

bool nothing_to_export(QWidget* parent, const QStringList& files) {
    if (!files.isEmpty()) return false;
    QMessageBox::information(parent, QStringLiteral("Stud"), QStringLiteral("There are no logs to export yet."));
    return true;
}

}  // namespace

QString export_all_logs(QWidget* parent) {
    // Every session, not just this one: a problem worth reporting is often
    // visible in the run before the one the user noticed it in.
    const QStringList files = session_logs();
    if (nothing_to_export(parent, files)) return {};
    // Roblox's own logs from the same span: complete, where the session log
    // can lose what the client had not written out when it stopped.
    const QFileInfo oldest(files.last());
    const QDateTime since = oldest.birthTime().isValid() ? oldest.birthTime() : oldest.lastModified();
    // The input logs of sessions started with STUD_INPUT_LOG=1.
    QStringList with_input = files;
    const QDir log_dir(QString::fromStdString(stud::paths::log_dir()));
    for (const QString& name :
         log_dir.entryList({QStringLiteral("input-*.log")}, QDir::Files | QDir::NoSymLinks, QDir::Time)) {
        with_input << log_dir.filePath(name);
    }
    QStringList client_logs;
    const QDir users(client::prefix_dir() + QStringLiteral("/root/Users"));
    for (const QString& user : users.entryList(QDir::Dirs | QDir::NoDotAndDotDot)) {
        const QDir roblox(users.filePath(user) + QStringLiteral("/Library/Logs/Roblox"));
        for (const QFileInfo& log :
             roblox.entryInfoList({QStringLiteral("*.log")}, QDir::Files | QDir::NoSymLinks)) {
            if (log.lastModified() >= since) client_logs << log.absoluteFilePath();
        }
    }

    const QString name = QStringLiteral("stud-logs-%1.tar.gz")
                             .arg(QDateTime::currentDateTime().toString(QStringLiteral("yyyyMMdd-HHmmss")));
    QString target = QFileDialog::getSaveFileName(parent, QStringLiteral("Export Stud's logs"), in_downloads(name),
                                                  QStringLiteral("Archives (*.tar.gz);;All files (*)"));
    if (target.isEmpty()) return {};
    // A bare name typed in the dialog means the archive, not a file with no type.
    if (!target.endsWith(QStringLiteral(".tar.gz")) && !target.endsWith(QStringLiteral(".tgz"))) {
        target += QStringLiteral(".tar.gz");
    }
    QString error;
    if (!write_log_tarball(target, with_input, &error, client_logs)) {
        QMessageBox::warning(parent, QStringLiteral("Stud"), QStringLiteral("Could not export the logs: %1").arg(error));
        return {};
    }
    return QStringLiteral("%1 log%2 saved to %3")
        .arg(files.size())
        .arg(files.size() == 1 ? QString() : QStringLiteral("s"), target);
}

QString export_latest_log(QWidget* parent) {
    const QStringList files = session_logs();
    if (nothing_to_export(parent, files)) return {};
    const QString latest = files.first();
    QString target = QFileDialog::getSaveFileName(parent, QStringLiteral("Export the latest log"),
                                                  in_downloads(QFileInfo(latest).fileName()),
                                                  QStringLiteral("Log files (*.log);;All files (*)"));
    if (target.isEmpty()) return {};
    if (!target.endsWith(QStringLiteral(".log"))) target += QStringLiteral(".log");
    QFile::remove(target);
    if (!QFile::copy(latest, target)) {
        QMessageBox::warning(parent, QStringLiteral("Stud"), QStringLiteral("Could not write %1.").arg(target));
        return {};
    }
    return QStringLiteral("The latest log saved to %1").arg(target);
}

}  // namespace stud::ui
