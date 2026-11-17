#include "mac_client.h"

#include "code_signature.h"

#include "stud/stud_paths.h"

#include <QCoreApplication>
#include <QCryptographicHash>
#include <QDir>
#include <QDirIterator>
#include <QEventLoop>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QPointer>
#include <QProcess>
#include <QProcessEnvironment>
#include <QRegularExpression>
#include <QSaveFile>
#include <QStandardPaths>
#include <QTimer>

#include <vector>

#include <miniz.h>
#include <miniz_zip.h>

#include <sys/stat.h>
#include <unistd.h>

namespace stud::ui::client {
namespace {

// What cancel() stops: the request in flight, or the prefix being made.
bool g_cancelled = false;
QPointer<QNetworkReply> g_reply;
QPointer<QProcess> g_process;

// Roblox's client-version API, which answers for one binary type.
constexpr const char* kVersionHosts[] = {
    "https://clientsettingscdn.roblox.com",
    "https://clientsettings.roblox.com",
};
// Roblox's setup CDN and its two mirrors, tried in order.
constexpr const char* kSetupHosts[] = {
    "https://setup.rbxcdn.com",
    "https://setup-aws.rbxcdn.com",
    "https://setup-ak.rbxcdn.com",
};
// How Roblox names each product on its CDN, and what its zip unpacks to.
struct ProductInfo {
    const char* binary_type;  // client-version/<this>
    const char* zip;          // <upload>-<this>
    const char* app;          // the bundle inside the zip
    const char* executable;   // Contents/MacOS/<this>
    const char* directory;    // under Stud's data directory
    const char* identifier;   // what Roblox signs its executable as
};

const ProductInfo& info(Product product) {
    static const ProductInfo kPlayer{"MacPlayer", "RobloxPlayer.zip", "RobloxPlayer.app",
                                     "RobloxPlayer", "client", "com.roblox.RobloxPlayer"};
    static const ProductInfo kStudio{"MacStudio", "RobloxStudioApp.zip", "RobloxStudio.app",
                                     "RobloxStudio", "studio", "com.Roblox.RobloxStudio"};
    return product == Product::Studio ? kStudio : kPlayer;
}

QString current_marker(Product product) { return clients_dir(product) + QStringLiteral("/current"); }

// The manifest of a build's code, written when it is unpacked; beside the
// bundle, not in it.
QString integrity_file(Product product, const QString& upload) {
    return clients_dir(product) + "/" + upload + QStringLiteral("/.stud-integrity");
}

enum class Integrity { Intact, Changed, Unknown };

// Whether the installed build's code is still what was unpacked. Unknown for
// a build installed before Stud kept a manifest.
Integrity check_integrity(Product product, const Build& build) {
    QFile recorded(integrity_file(product, build.upload));
    if (!recorded.open(QIODevice::ReadOnly)) return Integrity::Unknown;
    return recorded.readAll() == code_manifest(build.app) ? Integrity::Intact : Integrity::Changed;
}

QNetworkRequest request_for(const QString& url) {
    QNetworkRequest request{QUrl(url)};
    request.setRawHeader("User-Agent", "Stud/" STUD_VERSION);
    request.setAttribute(QNetworkRequest::RedirectPolicyAttribute,
                         QNetworkRequest::NoLessSafeRedirectPolicy);
    return request;
}

// The upload and version Roblox serves for macOS now; empty when no answer.
std::pair<QString, QString> served_build(Product product, const QString& channel) {
    QNetworkAccessManager net;
    for (const char* host : kVersionHosts) {
        if (g_cancelled) break;
        QString url = QStringLiteral("%1/v2/client-version/%2")
                          .arg(QString::fromLatin1(host), QString::fromLatin1(info(product).binary_type));
        if (!channel.isEmpty()) url += QStringLiteral("/channel/") + channel;
        QNetworkRequest request = request_for(url);
        request.setTransferTimeout(10000);
        QNetworkReply* reply = net.get(request);
        g_reply = reply;
        QEventLoop loop;
        QObject::connect(reply, &QNetworkReply::finished, &loop, &QEventLoop::quit);
        loop.exec();
        reply->deleteLater();
        if (reply->error() != QNetworkReply::NoError) continue;
        const QJsonObject json = QJsonDocument::fromJson(reply->readAll()).object();
        const QString upload = json.value(QStringLiteral("clientVersionUpload")).toString();
        const QString version = json.value(QStringLiteral("version")).toString();
        // It names a folder Stud replaces and deletes: nothing but the shape
        // Roblox gives it.
        static const QRegularExpression upload_name(QStringLiteral("^version-[0-9a-fA-F]+$"));
        if (upload_name.match(upload).hasMatch()) return {upload, version};
    }
    return {};
}

// Downloads `url` to `path`, reporting progress. False on any failure.
bool download(const QString& url, const QString& path, const Progress& progress,
              const QString& what) {
    if (g_cancelled) return false;
    QNetworkAccessManager net;
    QSaveFile file(path);
    if (!file.open(QIODevice::WriteOnly)) return false;
    QNetworkRequest request = request_for(url);
    // A stall, not the whole transfer: a slow connection still gets there.
    request.setTransferTimeout(30000);
    QNetworkReply* reply = net.get(request);
    g_reply = reply;
    bool write_failed = false;
    QObject::connect(reply, &QNetworkReply::readyRead, reply, [&]() {
        if (file.write(reply->readAll()) < 0) write_failed = true;
    });
    QObject::connect(reply, &QNetworkReply::downloadProgress, reply,
                     [&](qint64 received, qint64 total) {
                         if (!progress) return;
                         progress(what, total > 0 ? static_cast<int>(received * 100 / total) : -1);
                     });
    QEventLoop loop;
    QObject::connect(reply, &QNetworkReply::finished, &loop, &QEventLoop::quit);
    loop.exec();
    reply->deleteLater();
    if (g_cancelled || reply->error() != QNetworkReply::NoError || write_failed) {
        // Nothing of it is kept: QSaveFile drops its temporary file.
        file.cancelWriting();
        std::fprintf(stderr, "stud: download of %s failed: %s\n", qPrintable(url),
                     qPrintable(reply->errorString()));
        return false;
    }
    file.write(reply->readAll());
    return file.commit();
}

}  // namespace

// Unpacks the zip into `dir`. Each entry is checked to stay inside `dir`, and
// a file keeps the permission bits the archive gives it (the executables need
// them). Symbolic links (Studio's frameworks have them) are made last, so no
// file is ever written through one, and only when their target is relative
// and stays inside `dir` too.
bool extract_zip(const QString& zip_path, const QString& dir, QString* error,
                 const std::function<void(int percent)>& step) {
    mz_zip_archive zip{};
    const QByteArray zip_name = QFile::encodeName(zip_path);
    if (!mz_zip_reader_init_file(&zip, zip_name.constData(), 0)) {
        *error = QStringLiteral("the download is not a readable zip");
        return false;
    }
    const QString root = QDir(dir).absolutePath() + QLatin1Char('/');
    const auto inside = [&root](const QString& path) {
        return (QDir::cleanPath(path) + QLatin1Char('/')).startsWith(root);
    };
    std::vector<std::pair<QString, QByteArray>> links;
    bool ok = true;
    const mz_uint count = mz_zip_reader_get_num_files(&zip);
    int shown = -1;
    for (mz_uint i = 0; i < count && ok; ++i) {
        // The window stays live, so Cancel can be pressed mid-unpack.
        QCoreApplication::processEvents();
        if (g_cancelled) {
            *error = QStringLiteral("cancelled");
            ok = false;
            break;
        }
        if (const int percent = static_cast<int>(qint64(i) * 100 / count); step && percent != shown) {
            shown = percent;
            step(percent);
        }
        mz_zip_archive_file_stat stat;
        if (!mz_zip_reader_file_stat(&zip, i, &stat)) {
            ok = false;
            break;
        }
        const QString name = QString::fromUtf8(stat.m_filename);
        const QString target = QDir::cleanPath(root + name);
        if (name.startsWith(QLatin1Char('/')) || !inside(target)) {
            *error = QStringLiteral("the download names a path outside its folder");
            ok = false;
            break;
        }
        if (mz_zip_reader_is_file_a_directory(&zip, i)) {
            ok = QDir().mkpath(target);
            continue;
        }
        if (S_ISLNK(stat.m_external_attr >> 16)) {
            size_t size = 0;
            void* data = mz_zip_reader_extract_to_heap(&zip, i, &size, 0);
            if (data == nullptr) {
                ok = false;
                break;
            }
            const QByteArray link_target(static_cast<const char*>(data), qsizetype(size));
            mz_free(data);
            const QString resolved =
                QFileInfo(target).absolutePath() + QLatin1Char('/') + QString::fromUtf8(link_target);
            if (link_target.isEmpty() || link_target.startsWith('/') || !inside(resolved)) {
                *error = QStringLiteral("the download links to a path outside its folder");
                ok = false;
                break;
            }
            links.emplace_back(target, link_target);
            continue;
        }
        QDir().mkpath(QFileInfo(target).absolutePath());
        const QByteArray target_name = QFile::encodeName(target);
        if (!mz_zip_reader_extract_to_file(&zip, i, target_name.constData(), 0)) {
            ok = false;
            break;
        }
        // Unix permissions sit in the high half of the external attributes.
        const mode_t mode = (stat.m_external_attr >> 16) & 0777;
        if (mode != 0) ::chmod(target_name.constData(), mode);
    }
    mz_zip_reader_end(&zip);
    for (const auto& [path, link_target] : links) {
        if (!ok) break;
        QDir().mkpath(QFileInfo(path).absolutePath());
        const QByteArray path_name = QFile::encodeName(path);
        ::unlink(path_name.constData());
        ok = ::symlink(link_target.constData(), path_name.constData()) == 0;
    }
    if (!ok && error->isEmpty()) *error = QStringLiteral("the download could not be unpacked");
    return ok;
}

namespace {

// Removes every build but `keep`, and anything a failed install left.
void remove_other_builds(Product product, const QString& keep) {
    QDir dir(clients_dir(product));
    for (const QString& name : dir.entryList(QDir::Dirs | QDir::NoDotAndDotDot)) {
        if (name != keep) QDir(dir.filePath(name)).removeRecursively();
    }
}

}  // namespace

QString display_name(Product product) {
    return product == Product::Studio ? QStringLiteral("Roblox Studio") : QStringLiteral("Roblox Player");
}

QString clients_dir(Product product) {
    return QString::fromStdString(stud::paths::data_dir()) + "/" + info(product).directory;
}

QString prefix_dir() { return QString::fromStdString(stud::paths::data_dir()) + "/prefix"; }

std::optional<Build> installed(Product product) {
    QFile marker(current_marker(product));
    if (!marker.open(QIODevice::ReadOnly | QIODevice::Text)) return std::nullopt;
    const QStringList lines = QString::fromUtf8(marker.readAll()).split('\n', Qt::SkipEmptyParts);
    if (lines.isEmpty()) return std::nullopt;
    Build build;
    build.upload = lines.at(0).trimmed();
    build.version = lines.size() > 1 ? lines.at(1).trimmed() : QString();
    build.app = clients_dir(product) + "/" + build.upload + "/" + info(product).app;
    if (!QFileInfo(build.app + "/Contents/MacOS/" + info(product).executable).isExecutable()) {
        return std::nullopt;
    }
    return build;
}

std::optional<Build> ensure_current(Product product, const Progress& progress, QString* error) {
    const QString name = display_name(product);
    const std::optional<Build> have = installed(product);
    // Only a client as Roblox shipped it runs. A changed one is downloaded
    // again; so is one from before Stud kept a record, when Roblox can be
    // reached, since nothing says what it should be.
    const Integrity integrity = have ? check_integrity(product, *have) : Integrity::Intact;
    if (integrity == Integrity::Changed) {
        std::fprintf(stderr, "stud: %s %s was changed since it was installed\n", qPrintable(name),
                     qPrintable(have->upload));
    }
    if (progress) progress(QStringLiteral("Checking for %1 updates").arg(name), -1);
    // The build of the channel Roblox has this account on; production's when
    // it has none.
    const QString channel = assigned_channel(prefix_dir() + QStringLiteral("/root"), product);
    if (!channel.isEmpty()) {
        std::printf("stud: Roblox has this account on the %s channel\n", qPrintable(channel));
    }
    const auto [upload, version] = served_build(product, channel);
    if (g_cancelled) {
        *error = QStringLiteral("Cancelled.");
        return std::nullopt;
    }
    if (upload.isEmpty()) {
        // Offline, or Roblox's API did not answer: what is installed runs,
        // unless it was changed.
        if (have && integrity != Integrity::Changed) return have;
        *error = have ? QStringLiteral("%1 was changed since it was installed, and Roblox could not "
                                       "be reached to download it again.").arg(name)
                      : QStringLiteral("Could not reach Roblox to download %1.").arg(name);
        return std::nullopt;
    }
    if (have && have->upload == upload && integrity == Integrity::Intact) return have;

    QDir().mkpath(clients_dir(product));
    // What an earlier download left when Stud was stopped mid-way: a zip or
    // its temporary file, and a half-unpacked build.
    QDir clients(clients_dir(product));
    for (const QString& stale : clients.entryList({QStringLiteral("*.zip*")}, QDir::Files | QDir::Hidden)) {
        clients.remove(stale);
    }
    for (const QString& stale : clients.entryList({QStringLiteral("*.partial")}, QDir::Dirs)) {
        QDir(clients.filePath(stale)).removeRecursively();
    }
    const QString zip_path = clients_dir(product) + "/" + upload + ".zip";
    const QString what = have ? QStringLiteral("Updating %1 to %2").arg(name, version)
                              : QStringLiteral("Downloading %1 %2").arg(name, version);
    bool fetched = false;
    for (const char* host : kSetupHosts) {
        const QString url =
            // A channel's builds are under channel/common, production's at the top.
            QStringLiteral("%1/%4mac/%2-%3")
                .arg(QString::fromLatin1(host), upload, QString::fromLatin1(info(product).zip),
                     channel.isEmpty() ? QString() : QStringLiteral("channel/common/"));
        if (download(url, zip_path, progress, what)) {
            fetched = true;
            break;
        }
        if (g_cancelled) break;
    }
    if (!fetched) {
        QFile::remove(zip_path);
        if (g_cancelled) {
            *error = QStringLiteral("Cancelled.");
            return std::nullopt;
        }
        if (have && integrity != Integrity::Changed) return have;
        *error = QStringLiteral("Could not download %1.").arg(name);
        return std::nullopt;
    }

    const QString unpacking = QStringLiteral("Unpacking %1 %2").arg(name, version);
    if (progress) progress(unpacking, 0);
    const QString staging = clients_dir(product) + "/" + upload + ".partial";
    QDir(staging).removeRecursively();
    QString extract_error;
    const bool unpacked = extract_zip(zip_path, staging, &extract_error, [&](int percent) {
        if (progress) progress(unpacking, percent);
    });
    QFile::remove(zip_path);
    const QString final_dir = clients_dir(product) + "/" + upload;
    bool in_place = unpacked &&
                    QFileInfo(staging + "/" + info(product).app + "/Contents/MacOS/" +
                              info(product).executable)
                        .exists();
    // Signed by Roblox, and every file as Roblox sealed it, before it
    // replaces anything: the download is the one moment the whole bundle is
    // Roblox's own, before FFlags and customizations go into it.
    if (in_place) {
        const QString unsigned_reason =
            codesign::verify_app(staging + "/" + info(product).app, QString::fromLatin1(info(product).identifier));
        if (!unsigned_reason.isEmpty()) {
            std::printf("stud: the downloaded %s failed Roblox's signature check: %s\n", qPrintable(name),
                        qPrintable(unsigned_reason));
            extract_error = QStringLiteral("it failed Roblox's signature check (%1)").arg(unsigned_reason);
            in_place = false;
        }
    }
    if (in_place) {
        QDir(final_dir).removeRecursively();
        in_place = QDir().rename(staging, final_dir);
    }
    if (!in_place) {
        QDir(staging).removeRecursively();
        if (g_cancelled) {
            *error = QStringLiteral("Cancelled.");
            return std::nullopt;
        }
        if (have && integrity != Integrity::Changed) return have;
        *error = QStringLiteral("Could not unpack %1: %2").arg(name)
                     .arg(extract_error.isEmpty() ? QStringLiteral("unexpected layout") : extract_error);
        return std::nullopt;
    }

    QSaveFile record(integrity_file(product, upload));
    if (!record.open(QIODevice::WriteOnly) ||
        record.write(code_manifest(final_dir + "/" + info(product).app)) < 0 || !record.commit()) {
        *error = QStringLiteral("Could not record the installed %1.").arg(name);
        return std::nullopt;
    }
    QSaveFile marker(current_marker(product));
    if (!marker.open(QIODevice::WriteOnly) ||
        marker.write((upload + "\n" + version + "\n").toUtf8()) < 0 || !marker.commit()) {
        *error = QStringLiteral("Could not record the installed %1.").arg(name);
        return std::nullopt;
    }
    remove_other_builds(product, upload);
    std::printf("stud: %s %s (%s) installed\n", qPrintable(name), qPrintable(version),
                qPrintable(upload));
    return installed(product);
}

QByteArray code_manifest(const QString& app) {
    const QString contents = app + QStringLiteral("/Contents");
    QStringList files;
    for (const QString& root : {contents + QStringLiteral("/MacOS"), contents + QStringLiteral("/_CodeSignature")}) {
        QDirIterator it(root, QDir::Files | QDir::System | QDir::Hidden, QDirIterator::Subdirectories);
        while (it.hasNext()) files << it.next();
    }
    if (files.isEmpty()) return {};
    files << contents + QStringLiteral("/Info.plist");
    const QString settings = contents + QStringLiteral("/MacOS/ClientSettings/");
    QStringList lines;
    for (const QString& file : files) {
        if (file.startsWith(settings)) continue;
        const QFileInfo fi(file);
        QCryptographicHash hash(QCryptographicHash::Sha256);
        if (fi.isSymLink()) {
            hash.addData(QFile::encodeName(fi.symLinkTarget()));
        } else {
            QFile in(file);
            if (!in.open(QIODevice::ReadOnly) || !hash.addData(&in)) continue;
        }
        lines << QString::fromLatin1(hash.result().toHex()) + QStringLiteral("  ") +
                     file.mid(app.size() + 1);
    }
    lines.sort();
    return (lines.join('\n') + '\n').toUtf8();
}

QString find_rosemary() {
    if (const QString forced = qEnvironmentVariable("STUD_ROSEMARY"); !forced.isEmpty()) {
        return QFileInfo(forced).isExecutable() ? forced : QString();
    }
    const QString appdir = qEnvironmentVariable("APPDIR");
    const QString here = QCoreApplication::applicationDirPath();
    for (const QString& candidate :
         {appdir.isEmpty() ? QString() : appdir + "/libexec/stud/rosemary",
          // /usr/lib/stud is where an Arch package keeps it, Arch having no libexec.
          here + "/../libexec/stud/rosemary", here + "/../lib/stud/rosemary",
          here + "/../rosemary/rosemary"}) {
        if (!candidate.isEmpty() && QFileInfo(candidate).isExecutable()) {
            return QFileInfo(candidate).absoluteFilePath();
        }
    }
    return QStandardPaths::findExecutable(QStringLiteral("rosemary"));
}

bool ensure_prefix(const QString& rosemary, const Progress& progress, QString* error) {
    if (QFileInfo(prefix_dir() + "/root").isDir()) return true;
    if (g_cancelled) {
        *error = QStringLiteral("Cancelled.");
        return false;
    }
    if (progress) progress(QStringLiteral("Setting up the macOS environment"), -1);
    QProcess create;
    g_process = &create;
    create.setProgram(rosemary);
    create.setArguments({QStringLiteral("prefix"), QStringLiteral("create"), prefix_dir()});
    QProcessEnvironment env = QProcessEnvironment::systemEnvironment();
    env.insert(QStringLiteral("ROSEMARY_PREFIX_SETUP"), QStringLiteral("download"));
    create.setProcessEnvironment(env);
    create.setStandardInputFile(QProcess::nullDevice());
    create.setProcessChannelMode(QProcess::ForwardedChannels);
    QEventLoop loop;
    QObject::connect(&create, &QProcess::finished, &loop, &QEventLoop::quit);
    QObject::connect(&create, &QProcess::errorOccurred, &loop, &QEventLoop::quit);
    create.start();
    loop.exec();
    if (g_cancelled) {
        // Half a prefix would pass for a whole one at the next start.
        create.waitForFinished();
        QDir(prefix_dir()).removeRecursively();
        *error = QStringLiteral("Cancelled.");
        return false;
    }
    if (create.exitStatus() != QProcess::NormalExit || create.exitCode() != 0 ||
        !QFileInfo(prefix_dir() + "/root").isDir()) {
        *error = QStringLiteral("Could not set up the macOS environment (rosemary prefix create "
                                "failed; the log has its output).");
        return false;
    }
    return true;
}

QString executable(const Build& build) {
    QFile plist(build.app + QStringLiteral("/Contents/Info.plist"));
    if (!plist.open(QIODevice::ReadOnly)) return {};
    static const QRegularExpression name(
        QStringLiteral("<key>CFBundleExecutable</key>\\s*<string>([^<]+)</string>"));
    const QRegularExpressionMatch match = name.match(QString::fromUtf8(plist.readAll()));
    if (!match.hasMatch()) return {};
    const QString path = build.app + QStringLiteral("/Contents/MacOS/") + match.captured(1);
    return QFileInfo(path).isFile() ? path : QString();
}

bool write_app_settings(const Build& build, const QJsonObject& flags, QString* error) {
    const QString dir = build.app + "/Contents/MacOS/ClientSettings";
    QDir().mkpath(dir);
    QSaveFile file(dir + "/ClientAppSettings.json");
    if (!file.open(QIODevice::WriteOnly) ||
        file.write(QJsonDocument(flags).toJson(QJsonDocument::Indented)) < 0 || !file.commit()) {
        *error = QStringLiteral("Could not write ClientAppSettings.json.");
        return false;
    }
    return true;
}

void cancel() {
    g_cancelled = true;
    if (g_reply) g_reply->abort();
    if (g_process) g_process->kill();
}

bool cancelled() { return g_cancelled; }

QString assigned_channel(const QString& root, Product product) {
    static const QRegularExpression saved(
        QStringLiteral(R"(<key>www\.roblox\.com</key>\s*<string>([^<]*)</string>)"));
    const QString plist = QStringLiteral("/Library/Preferences/com.roblox.%1Channel.plist")
                              .arg(product == Product::Studio ? QStringLiteral("RobloxStudio")
                                                              : QStringLiteral("RobloxPlayer"));
    const QDir users(root + QStringLiteral("/Users"));
    for (const QString& user : users.entryList(QDir::Dirs | QDir::NoDotAndDotDot)) {
        QFile in(users.filePath(user) + plist);
        if (!in.open(QIODevice::ReadOnly)) continue;
        const QString channel = saved.match(QString::fromUtf8(in.readAll())).captured(1).trimmed();
        if (channel.isEmpty() || channel.compare(QLatin1String("production"), Qt::CaseInsensitive) == 0) {
            continue;
        }
        // Written inside the prefix, and it goes into a download URL: a
        // channel's name and nothing else.
        static const QRegularExpression channel_name(QStringLiteral("^[A-Za-z0-9_-]+$"));
        if (!channel_name.match(channel).hasMatch()) continue;
        return channel;
    }
    return {};
}

QStringList remove_installer_leftovers(const QString& root) {
    QStringList removed;
    const QDir users(root + QStringLiteral("/Users"));
    for (const QString& user : users.entryList(QDir::Dirs | QDir::NoDotAndDotDot)) {
        QDir roblox(users.filePath(user) + QStringLiteral("/Library/Roblox"));
        for (const QString& zip : roblox.entryList({QStringLiteral("*.zip")}, QDir::Files)) {
            if (roblox.remove(zip)) removed << roblox.filePath(zip);
        }
        for (const QString& app : roblox.entryList({QStringLiteral("*.app")}, QDir::Dirs | QDir::NoDotAndDotDot)) {
            if (QDir(roblox.filePath(app)).removeRecursively()) removed << roblox.filePath(app);
        }
    }
    return removed;
}

}  // namespace stud::ui::client
