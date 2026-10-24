#include "desktop_materials.h"

#include "cube_view.h"
#include "stud/android_glue.h"
#include "stud/shader_pack.h"
#include "stud/stud_paths.h"

#include <miniz.h>
#include <miniz_zip.h>

#include <QCoreApplication>
#include <QCryptographicHash>
#include <QEventLoop>
#include <QFile>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLabel>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QString>
#include <QUrl>
#include <QVBoxLayout>
#include <QWidget>

#include <cstdio>
#include <exception>
#include <filesystem>
#include <map>
#include <memory>
#include <optional>
#include <vector>

namespace stud::ui {
namespace {

namespace fs = std::filesystem;

// Roblox's own distribution: the version the Windows client is on, and its
// packages, the same files the official Windows installer downloads.
const QString kWindowsVersionUrl =
    QStringLiteral("https://clientsettingscdn.roblox.com/v2/client-version/WindowsPlayer");
const QString kSetupUrl = QStringLiteral("https://setup.rbxcdn.com/");

const char* const kShaderPackage = "shaders.zip";
const char* const kDesktopPack = "shaders_vulkan_desktop.pack";
const char* const kMobilePackAsset = "shaders/shaders_vulkan_mobile.pack";

// The files kept, each from a package and under its name there.
struct Kept {
    const char* package;
    const char* entry;
    const char* dest;  // relative to the prepared directory
};
const Kept kKept[] = {
    {"content-textures3.zip", "brdfLUT.dds", "brdfLUT.dds"},
    {"content-textures3.zip", "plastic\\normaldetail.dds", "normaldetail.dds"},
    // The engine asks for it and the APK does not carry it.
    {"content-terrain.zip", "materialTransitions.json", "assets/android/terrain/materialTransitions.json"},
};
const char* const kCombinedPack = "assets/shaders/shaders_vulkan_mobile.pack";
// The Windows pack itself, kept so a newer Stud can combine it again
// without asking Roblox (outside assets/, which Process B lays over the
// APK's).
const char* const kKeptDesktopPack = "shaders_vulkan_desktop.pack";

void say(const QString& line) {
    std::printf("stud: Windows Graphical Parity: %s\n", line.toUtf8().constData());
    std::fflush(stdout);
}

// What the user sees while the Windows files are fetched and built: the
// spinning stud and a line saying what is happening. Only made once there
// is something to wait for, so a launch with everything already prepared
// shows nothing; gone when it goes out of scope, however preparing ended.
// The downloads wait in an event loop of their own, so the stud keeps
// spinning through them.
class LoadingScreen {
public:
    void show(const QString& text) {
        if (window_ == nullptr) {
            window_ = std::make_unique<QWidget>();
            window_->setWindowTitle(QStringLiteral("Stud"));
            auto* layout = new QVBoxLayout(window_.get());
            auto* stud = new CubeView(window_.get());
            stud->setFixedSize(128, 128);
            layout->addWidget(stud, 0, Qt::AlignHCenter);
            label_ = new QLabel(window_.get());
            label_->setAlignment(Qt::AlignCenter);
            label_->setWordWrap(true);
            layout->addWidget(label_);
            window_->setFixedWidth(360);
        }
        label_->setText(text);
        window_->show();
        window_->raise();
        // Painted now: the next step may be one that does not return to an
        // event loop for a moment (unpacking, building).
        QCoreApplication::processEvents();
    }

private:
    std::unique_ptr<QWidget> window_;
    QLabel* label_ = nullptr;
};

std::optional<QByteArray> fetch(const QString& url) {
    QNetworkAccessManager manager;
    std::unique_ptr<QNetworkReply> reply(manager.get(QNetworkRequest(QUrl(url))));
    QEventLoop loop;
    QObject::connect(reply.get(), &QNetworkReply::finished, &loop, &QEventLoop::quit);
    loop.exec();
    if (reply->error() != QNetworkReply::NoError) {
        say(QStringLiteral("could not fetch %1: %2").arg(url, reply->errorString()));
        return std::nullopt;
    }
    return reply->readAll();
}

// The release a version string names: its second field, 744 in both the
// APK's 2.744.1584 and the Windows client's 0.744.0.7441080.
QString release_of(const QString& version) {
    const QStringList parts = version.split(QLatin1Char('.'));
    return parts.size() >= 2 ? parts[1] : QString();
}

// Package name -> MD5, from the version's package manifest: a "v0" line,
// then for each package its name, MD5, packed size and unpacked size.
std::map<QString, QByteArray> package_checksums(const QByteArray& manifest) {
    const QList<QByteArray> lines = manifest.split('\n');
    std::map<QString, QByteArray> out;
    for (qsizetype i = 1; i + 1 < lines.size(); i += 4) {
        out[QString::fromUtf8(lines[i].trimmed())] = lines[i + 1].trimmed().toLower();
    }
    return out;
}

std::optional<QByteArray> unzip(const QByteArray& zip, const char* entry) {
    mz_zip_archive archive{};
    if (!mz_zip_reader_init_mem(&archive, zip.constData(), static_cast<size_t>(zip.size()), 0)) {
        return std::nullopt;
    }
    std::optional<QByteArray> out;
    const int index = mz_zip_reader_locate_file(&archive, entry, nullptr, 0);
    size_t size = 0;
    void* data = index < 0 ? nullptr
                           : mz_zip_reader_extract_to_heap(&archive, static_cast<mz_uint>(index), &size, 0);
    if (data != nullptr) {
        out = QByteArray(static_cast<const char*>(data), static_cast<qsizetype>(size));
        mz_free(data);
    }
    mz_zip_reader_end(&archive);
    return out;
}

bool write_file(const fs::path& path, const char* data, size_t size) {
    std::error_code ec;
    fs::create_directories(path.parent_path(), ec);
    QFile file(QString::fromStdString(path.string()));
    return file.open(QIODevice::WriteOnly) &&
           file.write(data, static_cast<qint64>(size)) == static_cast<qint64>(size);
}

bool complete(const fs::path& dir) {
    if (!fs::exists(dir / kCombinedPack)) return false;
    for (const Kept& kept : kKept) {
        if (!fs::exists(dir / kept.dest)) return false;
    }
    return true;
}

// What was prepared, and for which APK: {"apk" (the file), "apkVersion",
// "windows", "ready", "stud" (the version that combined the pack)}, plus
// "refused": the last APK file found to have no Windows build to match,
// kept beside the pack rather than over it.
QJsonObject read_state(const fs::path& path) {
    QFile file(QString::fromStdString(path.string()));
    if (!file.open(QIODevice::ReadOnly)) return {};
    return QJsonDocument::fromJson(file.readAll()).object();
}

void write_state(const fs::path& path, const std::string& apk, const QString& apk_version,
                 const QString& windows, bool ready) {
    const QByteArray json = QJsonDocument(QJsonObject{{"apk", QString::fromStdString(apk)},
                                                      {"apkVersion", apk_version},
                                                      {"windows", windows},
                                                      {"ready", ready},
                                                      {"stud", QStringLiteral(STUD_VERSION)}})
                                .toJson();
    write_file(path, json.constData(), static_cast<size_t>(json.size()));
}

// Combines the APK's pack with the Windows one into `dest`, and logs what
// it did.
bool build_combined(const std::string& apk_path, const QByteArray& desktop_pack, const fs::path& dest) {
    const std::vector<uint8_t> mobile = stud::android_glue::read_apk_asset(apk_path, kMobilePackAsset);
    stud::shader_pack::BuildReport report;
    const std::vector<uint8_t> combined = stud::shader_pack::build_desktop_materials(
        mobile, std::vector<uint8_t>(desktop_pack.begin(), desktop_pack.end()), &report);
    if (!write_file(dest, reinterpret_cast<const char*>(combined.data()), combined.size())) {
        say(QStringLiteral("could not write the combined shader pack"));
        return false;
    }
    say(QStringLiteral("%1 shaders from Windows, %2 of them drop-in, %3 added, %4 normal-map reads adapted")
            .arg(report.substituted)
            .arg(report.drop_in)
            .arg(report.grafted)
            .arg(report.normal_reads));
    for (const auto& [name, count] : report.drop_in_by_shader) {
        say(QStringLiteral("drop-in %1: %2").arg(QString::fromStdString(name)).arg(count));
    }
    return true;
}

// What an earlier Stud prepared, combined again by this one from the
// Windows pack it kept. False when there is none kept (prepared before
// it was) or combining fails; what was there is then left as it was.
bool rebuild(const std::string& apk_path, const fs::path& prepared) {
    QFile file(QString::fromStdString((prepared / kKeptDesktopPack).string()));
    if (!file.open(QIODevice::ReadOnly)) return false;
    try {
        LoadingScreen loading;
        loading.show(QStringLiteral("Preparing Windows Graphical Parity…"));
        const fs::path partial = prepared / (std::string(kCombinedPack) + ".partial");
        if (!build_combined(apk_path, file.readAll(), partial)) return false;
        std::error_code ec;
        fs::rename(partial, prepared / kCombinedPack, ec);
        return !ec;
    } catch (const std::exception& e) {
        say(QStringLiteral("could not combine the kept Windows pack again: %1").arg(QString::fromUtf8(e.what())));
        return false;
    }
}

// The first preparing, or a fresh one, from what Roblox publishes for Windows.
std::string prepare_from_windows(const std::string& apk_path, const fs::path& root, const fs::path& state_path,
                                 const std::string& fingerprint, const QString& apk_version,
                                 const QJsonObject& state);

}  // namespace

std::string prepare_desktop_materials(const std::string& apk_path) {
    const fs::path root = stud::paths::cache_dir() + "/desktop-materials";
    const fs::path state_path = root / "state.json";
    const std::string fingerprint = stud::android_glue::apk_source_fingerprint(apk_path);

    const QString apk_version = QString::fromStdString(stud::android_glue::apk_version_name(apk_path));

    // What was prepared is only ever used for the exact Roblox version it
    // was built from: a pack built against 2.733's shaders does not fit
    // 2.722's, so a downgrade (or any other version) never gets it. For
    // that version it is kept for as long as the APK is: Roblox moving the
    // Windows client to a newer release changes nothing, and another file
    // of the same version (the same APK imported again, or from elsewhere)
    // reuses it as it is. Asking Roblox again could only make it worse,
    // once Windows has moved on to a release whose files no longer fit.
    const QJsonObject state = read_state(state_path);
    const fs::path prepared = root / state.value("windows").toString().toStdString();
    const bool same_version =
        !apk_version.isEmpty() && state.value("apkVersion").toString() == apk_version;
    const bool reusable = same_version && state.value("ready").toBool() && complete(prepared);
    if (reusable) {
        // Another Stud combined it, and how packs are combined changes
        // between versions: combined again from the Windows pack it kept.
        const bool current = state.value("stud").toString() == QStringLiteral(STUD_VERSION);
        if (current || rebuild(apk_path, prepared)) {
            if (!current || state.value("apk").toString().toStdString() != fingerprint) {
                write_state(state_path, fingerprint, apk_version, state.value("windows").toString(), true);
                say(QStringLiteral("reusing what was prepared for %1").arg(apk_version));
            }
            return prepared.string();
        }
    }
    // Prepared by a Stud that kept no Windows pack: fetched again, and
    // what it prepared still used if that does not work out.
    const std::string fresh = prepare_from_windows(apk_path, root, state_path, fingerprint, apk_version, state);
    if (fresh.empty() && reusable) {
        say(QStringLiteral("using what an earlier Stud prepared for %1").arg(apk_version));
        return prepared.string();
    }
    return fresh;
}

namespace {

std::string prepare_from_windows(const std::string& apk_path, const fs::path& root, const fs::path& state_path,
                                 const std::string& fingerprint, const QString& apk_version,
                                 const QJsonObject& state) {
    // This exact file was already found to have no Windows build to match.
    if (!fingerprint.empty() && state.value("refused").toString().toStdString() == fingerprint) {
        return {};
    }

    LoadingScreen loading;
    try {
        const auto version_json = fetch(kWindowsVersionUrl);
        if (!version_json) return {};
        const QJsonObject version = QJsonDocument::fromJson(*version_json).object();
        const QString windows_version = version.value("version").toString();
        const QString upload = version.value("clientVersionUpload").toString();
        if (upload.isEmpty() || release_of(windows_version).isEmpty()) {
            say(QStringLiteral("Roblox did not say which Windows version is current"));
            return {};
        }
        // The two clients are built from the same release, and the shader
        // packs only fit each other within one. Older Windows builds are
        // not published by version any more, so a mismatch waits for an
        // APK of the same release.
        if (release_of(apk_version) != release_of(windows_version)) {
            say(QStringLiteral("the Windows client is on %1 and the APK is %2, a different release: "
                               "materials look as on Android until an APK of the same release")
                    .arg(windows_version, apk_version));
            // Noted beside what was prepared, not over it: going back to
            // the version it was built for finds it still there.
            QJsonObject refused = state;
            refused["refused"] = QString::fromStdString(fingerprint);
            const QByteArray json = QJsonDocument(refused).toJson();
            write_file(state_path, json.constData(), static_cast<size_t>(json.size()));
            return {};
        }

        say(QStringLiteral("downloading the Windows client's %1 files from Roblox").arg(windows_version));
        loading.show(QStringLiteral("Downloading the Windows graphics files from Roblox…"));
        const auto manifest = fetch(kSetupUrl + upload + QStringLiteral("-rbxPkgManifest.txt"));
        if (!manifest) return {};
        const auto checksums = package_checksums(*manifest);
        std::map<QString, QByteArray> packages;
        auto package = [&](const char* name) -> const QByteArray* {
            const QString key = QString::fromLatin1(name);
            if (auto it = packages.find(key); it != packages.end()) return &it->second;
            const auto sum = checksums.find(key);
            if (sum == checksums.end()) {
                say(QStringLiteral("%1 is not in %2's package list").arg(key, upload));
                return nullptr;
            }
            const auto bytes = fetch(kSetupUrl + upload + QLatin1Char('-') + key);
            if (!bytes) return nullptr;
            if (QCryptographicHash::hash(*bytes, QCryptographicHash::Md5).toHex() != sum->second) {
                say(QStringLiteral("%1 does not match its published checksum").arg(key));
                return nullptr;
            }
            return &packages.emplace(key, *bytes).first->second;
        };

        const fs::path dir = root / upload.toStdString();
        const fs::path partial = root / (upload.toStdString() + ".partial");
        std::error_code ec;
        fs::remove_all(partial, ec);
        for (const Kept& kept : kKept) {
            const QByteArray* zip = package(kept.package);
            if (zip == nullptr) return {};
            const auto file = unzip(*zip, kept.entry);
            if (!file || !write_file(partial / kept.dest, file->constData(), static_cast<size_t>(file->size()))) {
                say(QStringLiteral("could not take %1 out of %2").arg(kept.entry, kept.package));
                return {};
            }
        }
        const QByteArray* shaders = package(kShaderPackage);
        if (shaders == nullptr) return {};
        const auto desktop_pack = unzip(*shaders, kDesktopPack);
        if (!desktop_pack) {
            say(QStringLiteral("%1 has no %2").arg(kShaderPackage, kDesktopPack));
            return {};
        }

        loading.show(QStringLiteral("Preparing Windows Graphical Parity…"));
        if (!build_combined(apk_path, *desktop_pack, partial / kCombinedPack) ||
            !write_file(partial / kKeptDesktopPack, desktop_pack->constData(),
                        static_cast<size_t>(desktop_pack->size()))) {
            return {};
        }

        // Swapped in whole, and earlier versions' files dropped.
        fs::remove_all(dir, ec);
        fs::rename(partial, dir, ec);
        if (ec) {
            say(QStringLiteral("could not move the prepared files into place: %1")
                    .arg(QString::fromStdString(ec.message())));
            return {};
        }
        for (const auto& entry : fs::directory_iterator(root, ec)) {
            if (entry.is_directory() && entry.path() != dir) fs::remove_all(entry.path(), ec);
        }
        write_state(state_path, fingerprint, apk_version, upload, true);
        say(QStringLiteral("ready"));
        return dir.string();
    } catch (const std::exception& e) {
        say(QStringLiteral("not used this launch: %1").arg(QString::fromUtf8(e.what())));
        return {};
    }
}

}  // namespace

}  // namespace stud::ui
