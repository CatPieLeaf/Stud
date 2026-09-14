#include "desktop_entry.h"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QProcess>
#include <QProcessEnvironment>
#include <QStandardPaths>
#include <QString>
#include <QStringList>
#include <QTextStream>

#include <cstdio>

namespace stud::ui {

namespace {

// The sizes packaging/install-desktop-entry.sh installs, and the ones the
// index.theme below indexes. Kept the same on purpose: an icon at a size
// the theme does not list is an icon nothing finds.
const int kIconSizes[] = {16, 24, 32, 48, 64, 128, 256, 512};

// Where this build keeps its own share/ tree: alongside the executable
// for an installed package (/usr/bin/stud -> /usr/share) and inside the
// mounted image for an AppImage, which is the same shape.
QString share_root() {
    const QDir bin_dir(QFileInfo(QCoreApplication::applicationFilePath()).absolutePath());
    for (const char* candidate : {"../share", "../../share"}) {
        const QString path = QDir::cleanPath(bin_dir.absoluteFilePath(QString::fromLatin1(candidate)));
        if (QFileInfo::exists(path + "/applications/" + QStringLiteral(STUD_APP_ID) + ".desktop")) {
            return path;
        }
    }
    return {};
}

// What the entry should run.
//
// $APPIMAGE is set by the AppImage runtime and is the path to the single
// file the user actually has, the mount point the executable lives on
// disappears when Stud exits, so the executable's own path is useless in
// an entry that has to work tomorrow.
QString launcher_command() {
    const QString appimage = qEnvironmentVariable("APPIMAGE");
    if (!appimage.isEmpty() && QFileInfo::exists(appimage)) {
        return QFileInfo(appimage).absoluteFilePath();
    }
    return QCoreApplication::applicationFilePath();
}

// Replaces the program in an Exec= value, keeping its arguments.
QString rewrite_exec(const QString& value, const QString& program) {
    const int space = value.indexOf(QLatin1Char(' '));
    const QString args = space < 0 ? QString() : value.mid(space);
    // Quoted because an AppImage commonly sits somewhere with a space in
    // the path (a Downloads folder, an external drive).
    return QLatin1Char('"') + program + QLatin1Char('"') + args;
}

bool write_index_theme(const QString& icons_dir) {
    const QString path = icons_dir + "/index.theme";
    if (QFileInfo::exists(path)) return true;
    QFile file(path);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate | QIODevice::Text)) return false;
    QTextStream out(&file);
    QStringList dirs;
    for (int size : kIconSizes) dirs << QStringLiteral("%1x%1/apps").arg(size);
    out << "[Icon Theme]\n"
        << "Name=hicolor\n"
        << "Comment=Fallback icon theme\n"
        << "Directories=" << dirs.join(QLatin1Char(',')) << "\n";
    for (int size : kIconSizes) {
        out << "\n[" << size << "x" << size << "/apps]\n"
            << "Size=" << size << "\n"
            << "Context=Applications\n"
            << "Type=Threshold\n";
    }
    return true;
}

// Runs a HOST command, with the host's own library path rather than the
// bundle's.
//
// An AppImage puts its own Qt on LD_LIBRARY_PATH so that bundled plugins
// can find their own libraries (see the AppRun in
// packaging/build-appimage.sh). Every one of the commands below is a Qt
// or GLib application belonging to the host, and handing it this
// bundle's Qt instead of its own is how an AppImage breaks the desktop
// tools it shells out to. AppRun saves the original for exactly this.
void run_best_effort(const QString& program, const QStringList& args) {
    if (QStandardPaths::findExecutable(program).isEmpty()) return;
    QProcess process;
    QProcessEnvironment env = QProcessEnvironment::systemEnvironment();
    if (env.contains(QStringLiteral("STUD_HOST_LD_LIBRARY_PATH"))) {
        const QString host_path = env.value(QStringLiteral("STUD_HOST_LD_LIBRARY_PATH"));
        if (host_path.isEmpty()) {
            env.remove(QStringLiteral("LD_LIBRARY_PATH"));
        } else {
            env.insert(QStringLiteral("LD_LIBRARY_PATH"), host_path);
        }
        env.remove(QStringLiteral("QT_QPA_PLATFORMTHEME"));
    }
    process.setProcessEnvironment(env);
    process.start(program, args);
    process.waitForFinished(30000);
}

// Makes `entry` the default application for `mime_type` in the user's own
// mimeapps.list, leaving every other association in the file alone.
//
// Both sections matter: [Default Applications] is the choice, and
// [Added Associations] is what makes the entry appear in the chooser at
// all on desktops that filter it.
void set_default_application(const QString& mime_type, const QString& entry) {
    const QString path =
        QStandardPaths::writableLocation(QStandardPaths::GenericConfigLocation) +
        QStringLiteral("/mimeapps.list");
    QDir().mkpath(QFileInfo(path).absolutePath());

    QStringList lines;
    QFile file(path);
    if (file.open(QIODevice::ReadOnly | QIODevice::Text)) {
        while (!file.atEnd()) {
            QString line = QString::fromUtf8(file.readLine());
            while (line.endsWith(QLatin1Char('\n')) || line.endsWith(QLatin1Char('\r'))) line.chop(1);
            lines << line;
        }
        file.close();
    }

    for (const QString& section :
         {QStringLiteral("[Default Applications]"), QStringLiteral("[Added Associations]")}) {
        int section_start = lines.indexOf(section);
        if (section_start < 0) {
            if (!lines.isEmpty() && !lines.last().isEmpty()) lines << QString();
            lines << section;
            section_start = lines.size() - 1;
        }
        int section_end = lines.size();
        for (int i = section_start + 1; i < lines.size(); ++i) {
            if (lines.at(i).startsWith(QLatin1Char('['))) {
                section_end = i;
                break;
            }
        }
        const QString wanted = mime_type + QLatin1Char('=') + entry;
        bool replaced = false;
        for (int i = section_start + 1; i < section_end; ++i) {
            if (lines.at(i).startsWith(mime_type + QLatin1Char('='))) {
                lines[i] = wanted;
                replaced = true;
                break;
            }
        }
        if (!replaced) lines.insert(section_end, wanted);
    }

    if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate | QIODevice::Text)) return;
    file.write((lines.join(QLatin1Char('\n')) + QLatin1Char('\n')).toUtf8());
}

// An entry Stud itself installed under its old, pre-application-id name.
//
// It still claims the same URL schemes, with an Exec pointing at wherever
// that build happened to live, so leaving it behind means two entries
// competing to open a roblox:// link and one of them is stale. Removed
// only when it really is Stud's own, the name, and an Exec naming
// Stud's own binary.
void remove_legacy_entry(const QString& apps_dir) {
    const QString path = apps_dir + QStringLiteral("/stud.desktop");
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly | QIODevice::Text)) return;
    const QString body = QString::fromUtf8(file.readAll());
    file.close();
    if (!body.contains(QLatin1String("\nName=Stud"))) return;
    if (!body.contains(QLatin1String("stud-ui")) && !body.contains(QLatin1String("/stud"))) return;
    if (QFile::remove(path)) {
        std::printf("stud: removed the older entry %s\n", qPrintable(path));
    }
}

}  // namespace

int install_desktop_entry() {
    const QString app_id = QStringLiteral(STUD_APP_ID);
    const QString share = share_root();
    if (share.isEmpty()) {
        std::fprintf(stderr,
                     "stud: --install-desktop-entry: this build has no share/ tree next to it "
                     "(a build tree uses packaging/install-desktop-entry.sh instead)\n");
        return 1;
    }

    const QString data_home = QStandardPaths::writableLocation(QStandardPaths::GenericDataLocation);
    const QString apps_dir = data_home + "/applications";
    const QString icons_dir = data_home + "/icons/hicolor";
    if (!QDir().mkpath(apps_dir)) {
        std::fprintf(stderr, "stud: --install-desktop-entry: could not create %s\n",
                     qPrintable(apps_dir));
        return 1;
    }

    // Icons first: the entry below points Icon= at one of these by
    // absolute path, so it has to exist before anything reads the entry.
    QString largest_icon;
    for (int size : kIconSizes) {
        const QString source =
            QStringLiteral("%1/icons/hicolor/%2x%2/apps/%3.png").arg(share).arg(size).arg(app_id);
        if (!QFileInfo::exists(source)) continue;
        const QString target_dir = QStringLiteral("%1/%2x%2/apps").arg(icons_dir).arg(size);
        if (!QDir().mkpath(target_dir)) continue;
        const QString target = target_dir + "/" + app_id + ".png";
        QFile::remove(target);
        if (QFile::copy(source, target)) largest_icon = target;
    }
    if (largest_icon.isEmpty()) {
        std::fprintf(stderr, "stud: --install-desktop-entry: no icon found under %s/icons\n",
                     qPrintable(share));
        return 1;
    }
    write_index_theme(icons_dir);

    QFile source_entry(share + "/applications/" + app_id + ".desktop");
    if (!source_entry.open(QIODevice::ReadOnly | QIODevice::Text)) {
        std::fprintf(stderr, "stud: --install-desktop-entry: could not read %s\n",
                     qPrintable(source_entry.fileName()));
        return 1;
    }
    const QString program = launcher_command();
    QStringList lines;
    while (!source_entry.atEnd()) {
        QString line = QString::fromUtf8(source_entry.readLine());
        while (line.endsWith(QLatin1Char('\n')) || line.endsWith(QLatin1Char('\r'))) line.chop(1);
        if (line.startsWith(QLatin1String("Exec="))) {
            line = QLatin1String("Exec=") + rewrite_exec(line.mid(5), program);
        } else if (line.startsWith(QLatin1String("Icon="))) {
            // An absolute path rather than the theme name, for the same
            // reason packaging/install-desktop-entry.sh uses one: the
            // themed name resolves only once every consumer has noticed a
            // newly-added user icon directory, and an absolute path shows
            // up immediately.
            line = QLatin1String("Icon=") + largest_icon;
        }
        lines << line;
    }
    source_entry.close();

    const QString entry_path = apps_dir + "/" + app_id + ".desktop";
    QFile entry(entry_path);
    if (!entry.open(QIODevice::WriteOnly | QIODevice::Truncate | QIODevice::Text)) {
        std::fprintf(stderr, "stud: --install-desktop-entry: could not write %s\n",
                     qPrintable(entry_path));
        return 1;
    }
    const QByteArray body = (lines.join(QLatin1Char('\n')) + QLatin1Char('\n')).toUtf8();
    entry.write(body);
    entry.close();
    QFile::setPermissions(entry_path, QFileDevice::ReadOwner | QFileDevice::WriteOwner |
                                          QFileDevice::ReadGroup | QFileDevice::ReadOther);

    // Register as the handler for Stud's own URL schemes.
    //
    // Having an entry that DECLARES the scheme is not the same as being
    // the application chosen for it: a browser asks the desktop for the
    // default, and with none set it shows the "choose an application"
    // chooser instead of opening Stud. Reported exactly that way, from
    // Chrome, on a roblox:// link.
    //
    // Written straight into mimeapps.list rather than shelled out to
    // xdg-mime, which is a script that reads the desktop environment out
    // of the environment it is handed, and this one may be an
    // AppImage's. The file's own format is stable and small.
    for (const char* scheme : {"x-scheme-handler/roblox", "x-scheme-handler/roblox-player"}) {
        set_default_application(QString::fromLatin1(scheme), app_id + QStringLiteral(".desktop"));
    }

    // Best-effort index refreshes. KDE in particular keeps its own
    // service index, and a new entry is invisible to the launcher, and
    // to window-to-icon matching, until that is rebuilt.
    run_best_effort(QStringLiteral("gtk-update-icon-cache"),
                    {QStringLiteral("-q"), QStringLiteral("-f"), QStringLiteral("-t"), icons_dir});
    run_best_effort(QStringLiteral("update-desktop-database"), {QStringLiteral("-q"), apps_dir});
    for (const auto& kbuildsycoca : {QStringLiteral("kbuildsycoca6"), QStringLiteral("kbuildsycoca5")}) {
        if (!QStandardPaths::findExecutable(kbuildsycoca).isEmpty()) {
            run_best_effort(kbuildsycoca, {QStringLiteral("--noincremental")});
            break;
        }
    }

    remove_legacy_entry(apps_dir);

    std::printf("stud: installed %s\n", qPrintable(entry_path));
    std::printf("stud: it runs %s\n", qPrintable(program));
    std::fflush(stdout);
    return 0;
}

}  // namespace stud::ui
