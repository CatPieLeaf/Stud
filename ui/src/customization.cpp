#include "customization.h"

#include "stud/stud_paths.h"

#include <QCryptographicHash>
#include <QDir>
#include <QDirIterator>
#include <QFile>
#include <QFileInfo>
#include <QImage>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QMap>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QSaveFile>
#include <QUrl>
#include <QStringList>

namespace stud::ui::customization {

namespace {

struct PartFiles {
    QString directory;
    QStringList files;
};

PartFiles files_of(Part part) {
    switch (part) {
        case Part::Cursor:
            return {QStringLiteral("content/textures/Cursors/KeyboardMouse"),
                    {QStringLiteral("ArrowCursor.png"), QStringLiteral("ArrowFarCursor.png"),
                     QStringLiteral("IBeamCursor.png")}};
        case Part::ShiftLock:
            return {QStringLiteral("content/textures"), {QStringLiteral("MouseLockedCursor.png")}};
        case Part::EmoteWheel:
            return {QStringLiteral("content/textures/ui/Emotes/Large"),
                    {QStringLiteral("SelectedGradient.png"), QStringLiteral("SelectedGradient@2x.png"),
                     QStringLiteral("SelectedGradient@3x.png"), QStringLiteral("SelectedLine.png"),
                     QStringLiteral("SelectedLine@2x.png"), QStringLiteral("SelectedLine@3x.png")}};
        case Part::DeathSound:
            return {QStringLiteral("content/sounds"), {QStringLiteral("ouch.ogg")}};
        case Part::Font:
            return {QStringLiteral("content/fonts"),
                    {QStringLiteral("CustomFont.ttf"), QStringLiteral("CustomFont.otf")}};
        case Part::MonoFont:
            return {QStringLiteral("content/fonts"),
                    {QStringLiteral("CustomMonoFont.ttf"), QStringLiteral("CustomMonoFont.otf")}};
        case Part::AvatarBackground:
            return {QStringLiteral("ExtraContent/places"), {QStringLiteral("Mobile.rbxl")}};
        case Part::CharacterSounds:
            return {QStringLiteral("content/sounds"),
                    {QStringLiteral("action_footsteps_plastic.mp3"), QStringLiteral("action_jump.mp3"),
                     QStringLiteral("action_get_up.mp3"), QStringLiteral("action_falling.mp3"),
                     QStringLiteral("action_jump_land.mp3"), QStringLiteral("action_swim.mp3"),
                     QStringLiteral("impact_water.mp3")}};
    }
    return {};
}

// Beside the bundle: what was changed, and the originals.
QString state_dir(const client::Build& build) {
    return QFileInfo(build.app).absolutePath() + QStringLiteral("/.stud-customizations");
}
QString resources(const client::Build& build) { return build.app + QStringLiteral("/Contents/Resources"); }

bool copy_over(const QString& from, const QString& to) {
    QDir().mkpath(QFileInfo(to).absolutePath());
    QFile::remove(to);
    return QFile::copy(from, to);
}

// Restores every original a previous apply() replaced, and removes files it added.
void restore(const client::Build& build) {
    const QString state = state_dir(build);
    QFile manifest(state + QStringLiteral("/manifest"));
    if (manifest.open(QIODevice::ReadOnly | QIODevice::Text)) {
        for (const QString& relative : QString::fromUtf8(manifest.readAll()).split('\n', Qt::SkipEmptyParts)) {
            const QString target = resources(build) + QLatin1Char('/') + relative;
            const QString original = state + QStringLiteral("/originals/") + relative;
            if (QFileInfo::exists(original)) {
                copy_over(original, target);
            } else {
                QFile::remove(target);
            }
        }
    }
    QDir(state).removeRecursively();
}

class Applier {
public:
    explicit Applier(const client::Build& build) : build_(build), state_(state_dir(build)) {}

    // `data` or the file `from`, keeping the original.
    bool put(const QString& relative, const QString& from, const QByteArray* data = nullptr) {
        const QString target = resources(build_) + QLatin1Char('/') + relative;
        if (QFileInfo::exists(target) && !copy_over(target, state_ + QStringLiteral("/originals/") + relative)) {
            return false;
        }
        changed_ << relative;
        if (data != nullptr) {
            QDir().mkpath(QFileInfo(target).absolutePath());
            QSaveFile out(target);
            return out.open(QIODevice::WriteOnly) && out.write(*data) == data->size() && out.commit();
        }
        return copy_over(from, target);
    }

    bool finish() {
        QDir().mkpath(state_);
        QSaveFile manifest(state_ + QStringLiteral("/manifest"));
        return manifest.open(QIODevice::WriteOnly) &&
               manifest.write(changed_.join(QLatin1Char('\n')).toUtf8()) >= 0 && manifest.commit();
    }

private:
    const client::Build& build_;
    QString state_;
    QStringList changed_;
};

// The last good set, and the trial marker.
QString guard_dir(client::Product product) {
    return QString::fromStdString(stud::paths::data_dir()) +
           (product == client::Product::Studio ? QStringLiteral("/customization-guard/studio")
                                               : QStringLiteral("/customization-guard/player"));
}
QString last_good_dir(client::Product product) { return guard_dir(product) + QStringLiteral("/last-good"); }
QString trial_mark(client::Product product) { return guard_dir(product) + QStringLiteral("/trial"); }

QMap<QString, QByteArray> fingerprint(const QString& root) {
    QMap<QString, QByteArray> files;
    QDirIterator it(root, QDir::Files | QDir::NoSymLinks | QDir::Hidden, QDirIterator::Subdirectories);
    while (it.hasNext()) {
        const QString path = it.next();
        QFile in(path);
        QCryptographicHash hash(QCryptographicHash::Sha256);
        if (in.open(QIODevice::ReadOnly) && hash.addData(&in)) {
            files.insert(path.mid(root.size() + 1), hash.result());
        }
    }
    return files;
}

// Makes `to` an exact copy of `from`, or empties it when `from` doesn't exist.
void mirror(const QString& from, const QString& to) {
    QDir(to).removeRecursively();
    QDirIterator it(from, QDir::Files | QDir::NoSymLinks | QDir::Hidden, QDirIterator::Subdirectories);
    while (it.hasNext()) {
        const QString path = it.next();
        copy_over(path, to + QLatin1Char('/') + path.mid(from.size() + 1));
    }
}

}  // namespace

QString folder(client::Product product) {
    return QString::fromStdString(stud::paths::config_dir()) +
           (product == client::Product::Studio ? QStringLiteral("/studio-customizations") : QStringLiteral("/customizations"));
}

bool apply(const client::Build& build, client::Product product, QString* error) {
    restore(build);
    const QDir customizations(folder(product));
    if (!customizations.exists()) return true;

    Applier applier(build);
    const QString root = customizations.absolutePath() + QLatin1Char('/');
    QDirIterator it(customizations.absolutePath(), QDir::Files | QDir::NoSymLinks | QDir::Hidden,
                    QDirIterator::Subdirectories);
    QString font;
    QString mono_font;
    while (it.hasNext()) {
        const QString path = it.next();
        const QString relative = path.mid(root.size());
        if (relative.startsWith(QLatin1String("..")) || QDir::cleanPath(relative) != relative) continue;
        if (relative == QLatin1String("content/fonts/CustomFont.ttf") ||
            relative == QLatin1String("content/fonts/CustomFont.otf")) {
            font = QFileInfo(relative).fileName();
        }
        if (relative == QLatin1String("content/fonts/CustomMonoFont.ttf") ||
            relative == QLatin1String("content/fonts/CustomMonoFont.otf")) {
            mono_font = QFileInfo(relative).fileName();
        }
        if (!applier.put(relative, path)) {
            applier.finish();
            restore(build);
            *error = QStringLiteral("could not apply the customization file %1").arg(relative);
            return false;
        }
    }

    // Font families the customizations don't replace point at the custom fonts: code
    // fonts at the code font, the rest at the text font. Script fallbacks (Noto) and
    // emoji keep Roblox's, or their text would turn into empty boxes.
    if (!font.isEmpty() || !mono_font.isEmpty()) {
        const QDir families(resources(build) + QStringLiteral("/content/fonts/families"));
        for (const QString& name : families.entryList({QStringLiteral("*.json")}, QDir::Files)) {
            const QString relative = QStringLiteral("content/fonts/families/") + name;
            if (QFileInfo::exists(root + relative)) continue;
            const QString lower = name.toLower();
            if (lower.startsWith(QLatin1String("noto")) || lower.contains(QLatin1String("emoji"))) continue;
            const bool mono = lower.contains(QLatin1String("mono")) || lower == QLatin1String("inconsolata.json");
            const QString chosen = mono ? mono_font : font;
            if (chosen.isEmpty()) continue;
            const QString asset = QStringLiteral("rbxasset://fonts/") + chosen;
            QFile in(families.filePath(name));
            if (!in.open(QIODevice::ReadOnly)) continue;
            QJsonObject family = QJsonDocument::fromJson(in.readAll()).object();
            in.close();
            QJsonArray faces = family.value(QStringLiteral("faces")).toArray();
            if (faces.isEmpty()) continue;
            for (QJsonValueRef face : faces) {
                QJsonObject f = face.toObject();
                f.insert(QStringLiteral("assetId"), asset);
                face = f;
            }
            family.insert(QStringLiteral("faces"), faces);
            const QByteArray data = QJsonDocument(family).toJson(QJsonDocument::Compact);
            if (!applier.put(relative, QString(), &data)) {
                applier.finish();
                restore(build);
                *error = QStringLiteral("could not point %1 at the custom font").arg(name);
                return false;
            }
        }
    }
    if (!applier.finish()) {
        restore(build);
        *error = QStringLiteral("could not record the applied customizations");
        return false;
    }
    return true;
}

bool use_file(Part part, const QString& file, QString* error) {
    const PartFiles p = files_of(part);
    const QString dir = folder(client::Product::Player) + QLatin1Char('/') + p.directory;
    remove(part);
    QStringList targets = p.files;
    if (part == Part::Font || part == Part::MonoFont) {
        targets = {file.endsWith(QLatin1String(".otf"), Qt::CaseInsensitive) ? p.files.at(1) : p.files.at(0)};
    } else if (part == Part::Cursor) {
        targets = {p.files.at(0), p.files.at(1)};  // the text cursor stays Roblox's
    }
    for (const QString& name : targets) {
        if (!copy_over(file, dir + QLatin1Char('/') + name)) {
            *error = QStringLiteral("could not copy %1 into the customizations folder").arg(QFileInfo(file).fileName());
            return false;
        }
    }
    return true;
}

bool recolour(Part part, const QColor& colour, QString* error) {
    const auto build = client::installed(client::Product::Player);
    if (!build) {
        *error = QStringLiteral("Roblox is not installed yet; open it once first.");
        return false;
    }
    const PartFiles p = files_of(part);
    const QString dir = folder(client::Product::Player) + QLatin1Char('/') + p.directory;
    remove(part);
    for (const QString& name : p.files) {
        // From the kept original if a customization replaced it.
        const QString relative = p.directory + QLatin1Char('/') + name;
        QString source = state_dir(*build) + QStringLiteral("/originals/") + relative;
        if (!QFileInfo::exists(source)) source = resources(*build) + QLatin1Char('/') + relative;
        QImage image(source);
        if (image.isNull()) continue;
        image = image.convertToFormat(QImage::Format_ARGB32);
        // Multiply: white becomes the colour, black stays, greys shade between; alpha kept.
        for (int y = 0; y < image.height(); ++y) {
            auto* line = reinterpret_cast<QRgb*>(image.scanLine(y));
            for (int x = 0; x < image.width(); ++x) {
                const int alpha = qAlpha(line[x]);
                if (alpha == 0) continue;
                line[x] = qRgba(qRed(line[x]) * colour.red() / 255, qGreen(line[x]) * colour.green() / 255,
                                qBlue(line[x]) * colour.blue() / 255, alpha);
            }
        }
        QDir().mkpath(dir);
        if (!image.save(dir + QLatin1Char('/') + name, "PNG")) {
            *error = QStringLiteral("could not write %1").arg(name);
            return false;
        }
    }
    return true;
}

void remove(Part part) {
    const PartFiles p = files_of(part);
    for (const QString& name : p.files) {
        QFile::remove(folder(client::Product::Player) + QLatin1Char('/') + p.directory + QLatin1Char('/') + name);
    }
}

void download_classic(Part part, const QString& variant, std::function<void(const QString& error)> done) {
    static const QString kMods =
        QStringLiteral("https://raw.githubusercontent.com/bloxstraplabs/bloxstrap/main/Bloxstrap/Resources/Mods/");
    const PartFiles p = files_of(part);
    // Each target file and its source in Bloxstrap's Mods folder, as Bloxstrap maps them.
    QStringList sources;
    if (part == Part::AvatarBackground) {
        sources = {QStringLiteral("OldAvatarBackground.rbxl")};
    } else if (part == Part::CharacterSounds) {
        sources = {QStringLiteral("Sounds/OldWalk.mp3"), QStringLiteral("Sounds/OldJump.mp3"),
                   QStringLiteral("Sounds/OldGetUp.mp3"), QStringLiteral("Sounds/Empty.mp3"),
                   QStringLiteral("Sounds/Empty.mp3"), QStringLiteral("Sounds/Empty.mp3"),
                   QStringLiteral("Sounds/Empty.mp3")};
    } else if (part == Part::Cursor) {
        sources = {QStringLiteral("Cursor/From%1/ArrowCursor.png").arg(variant),
                   QStringLiteral("Cursor/From%1/ArrowFarCursor.png").arg(variant)};
    }
    if (sources.isEmpty()) {
        done(QStringLiteral("nothing to download for this customization"));
        return;
    }

    auto* net = new QNetworkAccessManager();
    struct Pending {
        QVector<QByteArray> data;
        int left;
        QString error;
    };
    auto pending = std::make_shared<Pending>(Pending{QVector<QByteArray>(sources.size()), int(sources.size()), {}});
    const QString dir = folder(client::Product::Player) + QLatin1Char('/') + p.directory;
    for (int i = 0; i < sources.size(); ++i) {
        QNetworkRequest request{QUrl(kMods + sources.at(i))};
        request.setTransferTimeout(30000);
        QNetworkReply* reply = net->get(request);
        QObject::connect(reply, &QNetworkReply::finished, net, [=] {
            reply->deleteLater();
            if (reply->error() != QNetworkReply::NoError) {
                pending->error = QStringLiteral("could not download %1 from Bloxstrap: %2")
                                     .arg(sources.at(i), reply->errorString());
            } else {
                pending->data[i] = reply->readAll();
            }
            if (--pending->left > 0) return;
            net->deleteLater();
            if (!pending->error.isEmpty()) return done(pending->error);
            if (part == Part::Cursor) remove(Part::Cursor);
            // Files the installed client doesn't ship (Bloxstrap targets the Windows one) are left out.
            const auto build = client::installed(client::Product::Player);
            for (int k = 0; k < sources.size(); ++k) {
                const QString relative = p.directory + QLatin1Char('/') + p.files.at(k);
                if (build && !QFileInfo::exists(resources(*build) + QLatin1Char('/') + relative)) continue;
                QDir().mkpath(dir);
                QSaveFile out(dir + QLatin1Char('/') + p.files.at(k));
                if (!out.open(QIODevice::WriteOnly) || out.write(pending->data[k]) < 0 || !out.commit()) {
                    return done(QStringLiteral("could not save %1 into the customizations folder").arg(p.files.at(k)));
                }
            }
            done(QString());
        });
    }
}

bool present(Part part) {
    const PartFiles p = files_of(part);
    for (const QString& name : p.files) {
        if (QFileInfo::exists(folder(client::Product::Player) + QLatin1Char('/') + p.directory +
                              QLatin1Char('/') + name)) {
            return true;
        }
    }
    return false;
}

QString disabled_folder(client::Product product) { return folder(product) + QStringLiteral("-disabled"); }

void begin_run(client::Product product) {
    const QString last_good = last_good_dir(product);
    if (!QFileInfo::exists(guard_dir(product))) {
        // First run with the guard: the current set counts as good.
        mirror(folder(product), last_good);
        return;
    }
    QFile::remove(trial_mark(product));
    if (fingerprint(folder(product)) != fingerprint(last_good)) {
        QFile mark(trial_mark(product));
        if (mark.open(QIODevice::WriteOnly)) mark.close();
    }
}

QStringList end_run(client::Product product, bool crashed) {
    if (!QFile::remove(trial_mark(product))) return {};
    const QString current = folder(product);
    const QString last_good = last_good_dir(product);
    if (!crashed) {
        mirror(current, last_good);
        return {};
    }
    const QMap<QString, QByteArray> now = fingerprint(current);
    const QMap<QString, QByteArray> good = fingerprint(last_good);
    QStringList set_aside;
    for (auto it = now.cbegin(); it != now.cend(); ++it) {
        if (good.value(it.key()) == it.value()) continue;
        const QString from = current + QLatin1Char('/') + it.key();
        if (copy_over(from, disabled_folder(product) + QLatin1Char('/') + it.key())) {
            QFile::remove(from);
            set_aside << it.key();
        }
    }
    for (auto it = good.cbegin(); it != good.cend(); ++it) {
        if (now.value(it.key()) != it.value()) {
            copy_over(last_good + QLatin1Char('/') + it.key(), current + QLatin1Char('/') + it.key());
        }
    }
    return set_aside;
}

}  // namespace stud::ui::customization
