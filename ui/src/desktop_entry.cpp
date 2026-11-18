#include "desktop_entry.h"

#include <QCoreApplication>
#include <QDir>
#include <QHash>
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

// Must match install-desktop-entry.sh and index.theme: unlisted sizes aren't found.
const int kIconSizes[] = {16, 24, 32, 48, 64, 128, 256, 512};

// The share/ tree beside the executable, in a package and in the AppImage alike.
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

// The AppImage file itself ($APPIMAGE): the mount the executable runs from is gone after exit.

QString rewrite_exec(const QString& value, const QString& program) {
    const int space = value.indexOf(QLatin1Char(' '));
    const QString args = space < 0 ? QString() : value.mid(space);
    return desktop_exec_quote(program) + args;
}

}  // namespace

// Quoted, since AppImages often live in paths with spaces. ", `, $ and \ take a
// backslash, the file format doubles backslashes, and % is doubled.
QString desktop_exec_quote(const QString& program) {
    QString quoted;
    for (const QChar c : program) {
        if (c == QLatin1Char('\\')) {
            quoted += QStringLiteral("\\\\\\\\");
        } else if (c == QLatin1Char('"') || c == QLatin1Char('`') || c == QLatin1Char('$')) {
            quoted += QStringLiteral("\\\\") + c;
        } else if (c == QLatin1Char('%')) {
            quoted += QStringLiteral("%%");
        } else {
            quoted += c;
        }
    }
    return QLatin1Char('"') + quoted + QLatin1Char('"');
}

namespace {

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

// Runs a host tool with the host's library path, so an AppImage's bundled
// libraries don't break it.
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

// Sets `entry` as the default for `mime_type` in mimeapps.list. [Added Associations]
// is needed too, or some desktops leave it out of the chooser.
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

// Removes Stud's entry from before it used an application id: it claims the same
// schemes with a stale Exec. Only removed when its Exec names Stud's binary.
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

QString launcher_command() {
    const QString appimage = qEnvironmentVariable("APPIMAGE");
    if (!appimage.isEmpty() && QFileInfo::exists(appimage)) {
        return QFileInfo(appimage).absoluteFilePath();
    }
    return QCoreApplication::applicationFilePath();
}

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

    // Icons first: the entry points Icon= at them by absolute path.
    QHash<QString, QString> largest;
    for (const QString& name : {app_id, app_id + QStringLiteral("-player"), app_id + QStringLiteral("-studio")}) {
        for (int size : kIconSizes) {
            const QString source =
                QStringLiteral("%1/icons/hicolor/%2x%2/apps/%3.png").arg(share).arg(size).arg(name);
            if (!QFileInfo::exists(source)) continue;
            const QString target_dir = QStringLiteral("%1/%2x%2/apps").arg(icons_dir).arg(size);
            if (!QDir().mkpath(target_dir)) continue;
            const QString target = target_dir + "/" + name + ".png";
            QFile::remove(target);
            if (QFile::copy(source, target)) largest[name] = target;
        }
    }
    const QString largest_icon = largest.value(app_id);
    if (largest_icon.isEmpty()) {
        std::fprintf(stderr, "stud: --install-desktop-entry: no icon found under %s/icons\n",
                     qPrintable(share));
        return 1;
    }
    write_index_theme(icons_dir);

    // Stud's entry and the hidden <id>.Player and <id>.Studio ones, with this install's Exec and Icon.
    const QString program = launcher_command();
    const auto install_entry = [&](const QString& name) {
        QFile source_entry(share + "/applications/" + name + ".desktop");
        if (!source_entry.open(QIODevice::ReadOnly | QIODevice::Text)) {
            std::fprintf(stderr, "stud: --install-desktop-entry: could not read %s\n",
                         qPrintable(source_entry.fileName()));
            return false;
        }
        QStringList lines;
        while (!source_entry.atEnd()) {
            QString line = QString::fromUtf8(source_entry.readLine());
            while (line.endsWith(QLatin1Char('\n')) || line.endsWith(QLatin1Char('\r'))) line.chop(1);
            if (line.startsWith(QLatin1String("Exec="))) {
                line = QLatin1String("Exec=") + rewrite_exec(line.mid(5), program);
            } else if (line.startsWith(QLatin1String("Icon="))) {
                // An absolute path shows up at once; a theme name waits for caches to notice.
                line = QLatin1String("Icon=") + largest.value(line.mid(5), largest_icon);
            }
            lines << line;
        }
        const QString entry_path = apps_dir + "/" + name + ".desktop";
        QFile entry(entry_path);
        if (!entry.open(QIODevice::WriteOnly | QIODevice::Truncate | QIODevice::Text)) {
            std::fprintf(stderr, "stud: --install-desktop-entry: could not write %s\n",
                         qPrintable(entry_path));
            return false;
        }
        entry.write((lines.join(QLatin1Char('\n')) + QLatin1Char('\n')).toUtf8());
        entry.close();
        QFile::setPermissions(entry_path, QFileDevice::ReadOwner | QFileDevice::WriteOwner |
                                              QFileDevice::ReadGroup | QFileDevice::ReadOther);
        return true;
    };
    if (!install_entry(app_id)) return 1;
    install_entry(app_id + QStringLiteral(".Player"));
    install_entry(app_id + QStringLiteral(".Studio"));
    const QString entry_path = apps_dir + "/" + app_id + ".desktop";

    // Declaring a scheme isn't enough: with no default set, browsers show a chooser
    // instead of opening Stud. Written directly instead of via xdg-mime, which reads
    // the desktop from an environment that may be the AppImage's.
    for (const char* scheme : {"x-scheme-handler/roblox", "x-scheme-handler/roblox-player",
                               "x-scheme-handler/roblox-studio",
                               "x-scheme-handler/roblox-studio-auth"}) {
        set_default_application(QString::fromLatin1(scheme), app_id + QStringLiteral(".desktop"));
    }

    const QString mime_dir = data_home + QStringLiteral("/mime");
    const QString mime_file = mime_dir + QStringLiteral("/packages/") + app_id + QStringLiteral(".xml");
    QDir().mkpath(mime_dir + QStringLiteral("/packages"));
    QFile::remove(mime_file);
    if (QFile::copy(share + QStringLiteral("/mime/packages/") + app_id + QStringLiteral(".xml"), mime_file)) {
        run_best_effort(QStringLiteral("update-mime-database"), {mime_dir});
    }

    // KDE won't show a new entry, or match windows to its icon, until its index is rebuilt.
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
