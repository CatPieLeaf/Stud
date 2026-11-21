#include "customization.h"

#include "github_mirror.h"

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
#include <QRegularExpression>
#include <QSaveFile>
#include <QSet>
#include <QTemporaryDir>
#include <QUrl>

#include <memory>
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
        case Part::Emoji:
            return {QStringLiteral("content/fonts"), {QStringLiteral("TwemojiMozilla.ttf")}};
    }
    return {};
}

// Beside the bundle: what was changed, and the originals.
QString state_dir(const client::Build& build) {
    return QFileInfo(build.app).absolutePath() + QStringLiteral("/.stud-customizations");
}
QString resources(const client::Build& build) { return build.app + QStringLiteral("/Contents/Resources"); }

// Where each part came from, outside the folder so apply() never copies it into the client.
QString sources_file() {
    return QString::fromStdString(stud::paths::config_dir()) + QStringLiteral("/customizations-sources.json");
}

QString source_key(Part part) { return QString::number(static_cast<int>(part)); }

// The old login screen's files, each as {"from": its path in the archive, "to": the client's}.
const QString kLoginKey = QStringLiteral("login");

QJsonObject read_sources() {
    QFile file(sources_file());
    if (!file.open(QIODevice::ReadOnly)) return {};
    return QJsonDocument::fromJson(file.readAll()).object();
}

// An empty `value` forgets `key`.
void store_source(const QString& key, const QJsonValue& value) {
    QJsonObject all = read_sources();
    const bool empty = value.isNull() || value.isUndefined() || (value.isObject() && value.toObject().isEmpty()) ||
                       (value.isArray() && value.toArray().isEmpty());
    if (empty) {
        all.remove(key);
    } else {
        all.insert(key, value);
    }
    QSaveFile file(sources_file());
    if (file.open(QIODevice::WriteOnly)) {
        file.write(QJsonDocument(all).toJson(QJsonDocument::Compact));
        file.commit();
    }
}

void record_source(Part part, const QJsonObject& source) { store_source(source_key(part), source); }

// `relative` in the installed client: itself, or when Roblox renamed it the one file
// beside it whose name starts the same, as the dated login background does.
QString client_name(const client::Build& build, const QString& relative) {
    if (QFileInfo::exists(resources(build) + QLatin1Char('/') + relative)) return relative;
    const QFileInfo wanted(relative);
    const QDir dir(resources(build) + QLatin1Char('/') + wanted.path());
    const QStringList matches = dir.entryList({wanted.completeBaseName() + QStringLiteral("*.") + wanted.suffix()},
                                              QDir::Files);
    return matches.size() == 1 ? wanted.path() + QLatin1Char('/') + matches.first() : QString();
}

// The old login screen's files, moved to the names this build of the client uses.
void follow_renames(const client::Build& build) {
    QJsonArray files = read_sources().value(kLoginKey).toArray();
    bool moved = false;
    for (qsizetype i = 0; i < files.size(); ++i) {
        QJsonObject file = files.at(i).toObject();
        const QString to = file.value(QStringLiteral("to")).toString();
        if (QFileInfo::exists(resources(build) + QLatin1Char('/') + to)) continue;
        const QString now = client_name(build, file.value(QStringLiteral("from")).toString());
        if (now.isEmpty()) continue;
        const QString base = folder(client::Product::Player) + QLatin1Char('/');
        QDir().mkpath(QFileInfo(base + now).absolutePath());
        if (!QFile::rename(base + to, base + now)) continue;
        file.insert(QStringLiteral("to"), now);
        files.replace(i, file);
        moved = true;
    }
    if (moved) store_source(kLoginKey, files);
}

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
    if (product == client::Product::Player) follow_renames(build);

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
    record_source(part, {{QStringLiteral("file"), QFileInfo(file).fileName()}});
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
    record_source(part, {{QStringLiteral("colour"), colour.name()}});
    return true;
}

void remove(Part part) {
    const PartFiles p = files_of(part);
    for (const QString& name : p.files) {
        QFile::remove(folder(client::Product::Player) + QLatin1Char('/') + p.directory + QLatin1Char('/') + name);
    }
    record_source(part, {});
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

Applied applied(Part part) {
    if (!present(part)) return {QStringLiteral("Roblox's default"), {}};
    // Files put in by hand or by an import have no recorded source.
    const QJsonObject source = read_sources().value(source_key(part)).toObject();
    if (source.contains(QStringLiteral("file"))) return {source.value(QStringLiteral("file")).toString(), {}};
    if (source.contains(QStringLiteral("colour"))) {
        return {QStringLiteral("Recoloured"), QColor(source.value(QStringLiteral("colour")).toString())};
    }
    if (source.contains(QStringLiteral("offer"))) {
        return {QStringLiteral("%1's %2").arg(source.value(QStringLiteral("source")).toString(),
                                              source.value(QStringLiteral("offer")).toString()),
                {}};
    }
    if (source.contains(QStringLiteral("classic"))) {
        const QString variant = source.value(QStringLiteral("classic")).toString();
        return {variant.isEmpty() ? QStringLiteral("Bloxstrap's classic") : QStringLiteral("Bloxstrap's %1").arg(variant),
                {}};
    }
    return {QStringLiteral("Custom files"), {}};
}

QImage image(Part part) {
    const PartFiles p = files_of(part);
    if (p.files.isEmpty() || !p.files.first().endsWith(QLatin1String(".png"))) return {};
    const QString custom = folder(client::Product::Player) + QLatin1Char('/') + p.directory + QLatin1Char('/') + p.files.first();
    if (QFileInfo::exists(custom)) return QImage(custom);
    const auto build = client::installed(client::Product::Player);
    if (!build) return {};
    // Roblox's own ArrowCursor is the hand over clickable things; its arrow is ArrowFarCursor.
    const QString relative =
        p.directory + QLatin1Char('/') + (part == Part::Cursor ? QStringLiteral("ArrowFarCursor.png") : p.files.first());
    const QString original = state_dir(*build) + QStringLiteral("/originals/") + relative;
    return QImage(QFileInfo::exists(original) ? original : resources(*build) + QLatin1Char('/') + relative);
}

QString font_file(Part part) {
    if (part != Part::Font && part != Part::MonoFont && part != Part::Emoji) return {};
    const PartFiles p = files_of(part);
    for (const QString& name : p.files) {
        const QString path = folder(client::Product::Player) + QLatin1Char('/') + p.directory + QLatin1Char('/') + name;
        if (QFileInfo::exists(path)) return path;
    }
    // Roblox's own text fonts are many files; its emoji is one.
    if (part != Part::Emoji) return {};
    const auto build = client::installed(client::Product::Player);
    if (!build) return {};
    const QString relative = p.directory + QLatin1Char('/') + p.files.first();
    const QString original = state_dir(*build) + QStringLiteral("/originals/") + relative;
    return QFileInfo::exists(original) ? original : resources(*build) + QLatin1Char('/') + relative;
}

namespace {

// One download, a GitHub file through jsDelivr when that is preferred or GitHub fails.
void get(const QUrl& url, std::function<void(const QByteArray& body, const QString& error)> done) {
    github::get(url, std::move(done));
}

// Several downloads, answered once every one has.
void get_all(const QList<QUrl>& urls,
             std::function<void(const QList<QByteArray>& bodies, const QString& error)> done) {
    struct Pending {
        QList<QByteArray> bodies;
        qsizetype left;
        QString error;
    };
    auto pending = std::make_shared<Pending>(Pending{QList<QByteArray>(urls.size()), urls.size(), {}});
    if (urls.isEmpty()) return done({}, QString());
    for (qsizetype i = 0; i < urls.size(); ++i) {
        get(urls.at(i), [pending, i, done](const QByteArray& body, const QString& error) {
            if (!error.isEmpty()) pending->error = error;
            pending->bodies[i] = body;
            if (--pending->left == 0) done(pending->bodies, pending->error);
        });
    }
}

// A bootstrapper's GitHub repository and where in it its bundled mods are.
struct Repository {
    QString name;
    QString path;
    QString mods;
};

const QList<Repository>& repositories() {
    static const QList<Repository> all = {
        {QStringLiteral("Bloxstrap"), QStringLiteral("bloxstraplabs/bloxstrap"), QStringLiteral("Bloxstrap/Resources/Mods/")},
        {QStringLiteral("Froststrap"), QStringLiteral("Froststrap/Froststrap"), QStringLiteral("Froststrap/Resources/Mods/")},
        {QStringLiteral("Voidstrap"), QStringLiteral("KloBraticc/Voidstrap"),
         QStringLiteral("src/Voidstrap.App/Resources/Mods/")},
    };
    return all;
}

QUrl raw(const Repository& repository, const QString& path) {
    return QUrl(QStringLiteral("https://raw.githubusercontent.com/%1/HEAD/%2").arg(repository.path, path));
}

// Froststrap's catalogue of community mods.
const QUrl kFroststrapCatalogue(QStringLiteral("https://raw.githubusercontent.com/Froststrap/config/HEAD/Data.json"));
// Froststrap's own numbers for the kinds of community mod.
constexpr int kFroststrapAvatarEditor = 4;
const QString kFroststrapOldLogin = QStringLiteral("old-login-screen");
// Community mods Stud leaves out of its menus.
const QStringList kFroststrapSkipped = {QStringLiteral("catavatareditor")};

// The release Bloxstrap's emoji fonts come from.
const QUrl kEmojiFonts(QStringLiteral("https://api.github.com/repos/bloxstraplabs/rbxcustom-fontemojis/releases/latest"));

// "BibataModernIce" -> "Bibata Modern Ice"; "From2006" -> "2006".
QString folder_title(QString name) {
    if (name.startsWith(QLatin1String("From"))) name = name.mid(4);
    static const QRegularExpression boundary(QStringLiteral("(?<=[a-z0-9])(?=[A-Z])|(?<=[A-Z])(?=[A-Z][a-z])"));
    return name.replace(boundary, QStringLiteral(" "));
}

// Every file in `repository` at its default branch: GitHub's tree or jsDelivr's listing.
void list_files(const Repository& repository, std::function<void(const QSet<QString>& paths, const QString& error)> done) {
    const QUrl tree(QStringLiteral("https://api.github.com/repos/%1/git/trees/HEAD?recursive=1").arg(repository.path));
    const QUrl listing(QStringLiteral("https://data.jsdelivr.com/v1/packages/gh/%1@HEAD?structure=flat").arg(repository.path));
    github::get_either(tree, listing, [done](const QByteArray& body, bool from_jsdelivr, const QString& error) {
        QSet<QString> paths;
        const QJsonObject answer = QJsonDocument::fromJson(body).object();
        if (from_jsdelivr) {
            // jsDelivr names them from the repository's root, with a leading slash.
            for (const QJsonValue file : answer.value(QStringLiteral("files")).toArray()) {
                paths.insert(file.toObject().value(QStringLiteral("name")).toString().mid(1));
            }
        } else {
            for (const QJsonValue entry : answer.value(QStringLiteral("tree")).toArray()) {
                paths.insert(entry.toObject().value(QStringLiteral("path")).toString());
            }
        }
        done(paths, error);
    });
}

// Every cursor folder with both arrows, the first repository's copy of each name.
void fetch_cursors(std::function<void(const QList<Offer>&, const QString&)> done) {
    struct Pending {
        QList<QSet<QString>> listed;
        qsizetype left;
        QString error;
    };
    const qsizetype count = repositories().size();
    auto pending = std::make_shared<Pending>(Pending{QList<QSet<QString>>(count), count, {}});
    for (qsizetype r = 0; r < count; ++r) {
        list_files(repositories().at(r), [pending, r, done](const QSet<QString>& paths, const QString& error) {
            if (!error.isEmpty()) pending->error = error;
            pending->listed[r] = paths;
            if (--pending->left > 0) return;
            QList<Offer> offers;
            QStringList seen;
            const QString target = files_of(Part::Cursor).directory + QLatin1Char('/');
            for (qsizetype i = 0; i < pending->listed.size(); ++i) {
                const Repository& repository = repositories().at(i);
                const QSet<QString>& files = pending->listed.at(i);
                const QString cursors = repository.mods + QStringLiteral("Cursor/");
                QStringList folders;
                for (const QString& path : files) {
                    if (!path.startsWith(cursors) || !path.endsWith(QLatin1String("/ArrowCursor.png"))) continue;
                    const QString name = path.mid(cursors.size()).section(QLatin1Char('/'), 0, 0);
                    if (files.contains(cursors + name + QStringLiteral("/ArrowFarCursor.png"))) folders << name;
                }
                folders.sort();
                for (const QString& name : folders) {
                    if (seen.contains(name)) continue;
                    seen << name;
                    const QString base = cursors + name + QLatin1Char('/');
                    offers << Offer{folder_title(name), repository.name, raw(repository, base + QStringLiteral("ArrowCursor.png")),
                                    {{target + QStringLiteral("ArrowCursor.png"), raw(repository, base + QStringLiteral("ArrowCursor.png"))},
                                     {target + QStringLiteral("ArrowFarCursor.png"),
                                      raw(repository, base + QStringLiteral("ArrowFarCursor.png"))}},
                                    {}};
                }
            }
            done(offers, offers.isEmpty() ? pending->error : QString());
        });
    }
}

// Froststrap's community mods, each as {"id", "name", "download", "modtype", ...}.
void fetch_catalogue(std::function<void(const QJsonArray& mods, const QString& error)> done) {
    get(kFroststrapCatalogue, [done](const QByteArray& body, const QString& error) {
        done(QJsonDocument::fromJson(body).object().value(QStringLiteral("communityMods")).toArray(), error);
    });
}

void fetch_avatar_backgrounds(std::function<void(const QList<Offer>&, const QString&)> done) {
    const Repository& bloxstrap = repositories().first();
    const QString target = files_of(Part::AvatarBackground).directory + QLatin1Char('/') +
                           files_of(Part::AvatarBackground).files.first();
    QList<Offer> offers = {{QStringLiteral("Classic"), bloxstrap.name, {},
                            {{target, raw(bloxstrap, bloxstrap.mods + QStringLiteral("OldAvatarBackground.rbxl"))}}, {}}};
    fetch_catalogue([offers, done](const QJsonArray& mods, const QString& error) mutable {
        for (const QJsonValue value : mods) {
            const QJsonObject mod = value.toObject();
            if (mod.value(QStringLiteral("modtype")).toInt() != kFroststrapAvatarEditor ||
                kFroststrapSkipped.contains(mod.value(QStringLiteral("id")).toString())) {
                continue;
            }
            offers << Offer{mod.value(QStringLiteral("name")).toString(), QStringLiteral("Froststrap"), {}, {},
                            QUrl(mod.value(QStringLiteral("download")).toString())};
        }
        done(offers, mods.isEmpty() ? error : QString());
    });
}

QList<Offer> character_sounds() {
    const Repository& bloxstrap = repositories().first();
    const PartFiles p = files_of(Part::CharacterSounds);
    // Bloxstrap's sounds for each of the part's files, in order.
    const QStringList sounds = {QStringLiteral("OldWalk"), QStringLiteral("OldJump"), QStringLiteral("OldGetUp"),
                                QStringLiteral("Empty"),   QStringLiteral("Empty"),   QStringLiteral("Empty"),
                                QStringLiteral("Empty")};
    Offer classic{QStringLiteral("Classic"), bloxstrap.name, {}, {}, {}};
    for (qsizetype i = 0; i < p.files.size(); ++i) {
        classic.files << qMakePair(p.directory + QLatin1Char('/') + p.files.at(i),
                                   raw(bloxstrap, bloxstrap.mods + QStringLiteral("Sounds/%1.mp3").arg(sounds.at(i))));
    }
    return {classic};
}

// Bloxstrap's names for the fonts in its emoji release.
QString emoji_title(const QString& file) {
    static const QMap<QString, QString> titles = {
        {QStringLiteral("Win1122H2SegoeUIEmoji.ttf"), QStringLiteral("Windows 11")},
        {QStringLiteral("Win10April2018SegoeUIEmoji.ttf"), QStringLiteral("Windows 10")},
        {QStringLiteral("Win8.1SegoeUIEmoji.ttf"), QStringLiteral("Windows 8.1")},
    };
    return titles.value(file, QFileInfo(file).completeBaseName());
}

void fetch_emoji(std::function<void(const QList<Offer>&, const QString&)> done) {
    get(kEmojiFonts, [done](const QByteArray& body, const QString& error) {
        const QString target = files_of(Part::Emoji).directory + QLatin1Char('/') + files_of(Part::Emoji).files.first();
        QList<Offer> offers;
        for (const QJsonValue value : QJsonDocument::fromJson(body).object().value(QStringLiteral("assets")).toArray()) {
            const QJsonObject asset = value.toObject();
            const QString name = asset.value(QStringLiteral("name")).toString();
            if (!name.endsWith(QLatin1String(".ttf"), Qt::CaseInsensitive)) continue;
            offers << Offer{emoji_title(name), QStringLiteral("Bloxstrap"), {},
                            {{target, QUrl(asset.value(QStringLiteral("browser_download_url")).toString())}}, {}};
        }
        done(offers, offers.isEmpty() ? error : QString());
    });
}

// `archive` unpacked into a fresh temporary folder, or why it could not be.
std::unique_ptr<QTemporaryDir> unpack(const QByteArray& archive, QString* error) {
    auto dir = std::make_unique<QTemporaryDir>();
    QFile zip(dir->filePath(QStringLiteral("download.zip")));
    if (!dir->isValid() || !zip.open(QIODevice::WriteOnly) || zip.write(archive) != archive.size()) {
        *error = QStringLiteral("could not keep the download");
        return {};
    }
    zip.close();
    if (!client::extract_zip(zip.fileName(), dir->filePath(QStringLiteral("files")), error)) return {};
    return dir;
}

}  // namespace

void fetch_offers(Part part, std::function<void(const QList<Offer>& offers, const QString& error)> done) {
    switch (part) {
        case Part::Cursor:
            return fetch_cursors(done);
        case Part::AvatarBackground:
            return fetch_avatar_backgrounds(done);
        case Part::CharacterSounds:
            return done(character_sounds(), QString());
        case Part::Emoji:
            return fetch_emoji(done);
        default:
            return done({}, QString());
    }
}

void install(Part part, const Offer& offer, std::function<void(const QString& error)> done) {
    const QJsonObject source{{QStringLiteral("offer"), offer.name}, {QStringLiteral("source"), offer.source}};
    const QString dir = folder(client::Product::Player) + QLatin1Char('/');
    if (!offer.archive.isEmpty()) {
        get(offer.archive, [part, source, dir, done](const QByteArray& body, const QString& failed) {
            if (!failed.isEmpty()) return done(failed);
            QString error;
            const auto unpacked = unpack(body, &error);
            if (!unpacked) return done(error);
            // Only the part's own files: anything else in the archive is another customization.
            const PartFiles p = files_of(part);
            remove(part);
            bool any = false;
            for (const QString& name : p.files) {
                const QString relative = p.directory + QLatin1Char('/') + name;
                const QString from = unpacked->filePath(QStringLiteral("files/") + relative);
                if (!QFileInfo::exists(from)) continue;
                if (!copy_over(from, dir + relative)) return done(QStringLiteral("could not save %1").arg(name));
                any = true;
            }
            if (!any) return done(QStringLiteral("the download has nothing for this customization"));
            record_source(part, source);
            done(QString());
        });
        return;
    }
    QList<QUrl> urls;
    for (const auto& [_, url] : offer.files) urls << url;
    get_all(urls, [part, offer, source, dir, done](const QList<QByteArray>& bodies, const QString& error) {
        if (!error.isEmpty()) return done(error);
        remove(part);
        // Files the installed client doesn't ship (Bloxstrap targets the Windows one) are left out.
        const auto build = client::installed(client::Product::Player);
        for (qsizetype i = 0; i < offer.files.size(); ++i) {
            const QString relative = offer.files.at(i).first;
            if (build && !QFileInfo::exists(resources(*build) + QLatin1Char('/') + relative)) continue;
            QDir().mkpath(QFileInfo(dir + relative).absolutePath());
            QSaveFile out(dir + relative);
            if (!out.open(QIODevice::WriteOnly) || out.write(bodies.at(i)) < 0 || !out.commit()) {
                return done(QStringLiteral("could not save %1 into the customizations folder").arg(QFileInfo(relative).fileName()));
            }
        }
        record_source(part, source);
        done(QString());
    });
}

void fetch_image(const QUrl& url, std::function<void(const QImage& image)> done) {
    get(url, [done](const QByteArray& body, const QString& error) { done(error.isEmpty() ? QImage::fromData(body) : QImage()); });
}

bool old_login_screen() {
    const QJsonArray files = read_sources().value(kLoginKey).toArray();
    for (const QJsonValue file : files) {
        if (!QFileInfo::exists(folder(client::Product::Player) + QLatin1Char('/') +
                               file.toObject().value(QStringLiteral("to")).toString())) {
            return false;
        }
    }
    return !files.isEmpty();
}

void set_old_login_screen(bool on, std::function<void(const QString& error)> done) {
    const QString dir = folder(client::Product::Player) + QLatin1Char('/');
    for (const QJsonValue file : read_sources().value(kLoginKey).toArray()) {
        QFile::remove(dir + file.toObject().value(QStringLiteral("to")).toString());
    }
    store_source(kLoginKey, QJsonValue());
    if (!on) return done(QString());
    // The archive's names are the Windows client's; they're matched to this one's.
    const auto build = client::installed(client::Product::Player);
    if (!build) return done(QStringLiteral("Roblox is not installed yet; open it once first."));
    fetch_catalogue([build = *build, dir, done](const QJsonArray& mods, const QString& error) {
        QUrl archive;
        for (const QJsonValue value : mods) {
            if (value.toObject().value(QStringLiteral("id")).toString() == kFroststrapOldLogin) {
                archive = QUrl(value.toObject().value(QStringLiteral("download")).toString());
            }
        }
        if (archive.isEmpty()) return done(error.isEmpty() ? QStringLiteral("Froststrap no longer offers it") : error);
        get(archive, [build, dir, done](const QByteArray& body, const QString& failed) {
            if (!failed.isEmpty()) return done(failed);
            QString unpack_error;
            const auto unpacked = unpack(body, &unpack_error);
            if (!unpacked) return done(unpack_error);
            const QString root = unpacked->filePath(QStringLiteral("files")) + QLatin1Char('/');
            QJsonArray files;
            QDirIterator it(root, QDir::Files, QDirIterator::Subdirectories);
            while (it.hasNext()) {
                const QString from = it.next().mid(root.size());
                const QString to = client_name(build, from);
                if (to.isEmpty() || !copy_over(root + from, dir + to)) continue;
                files << QJsonObject{{QStringLiteral("from"), from}, {QStringLiteral("to"), to}};
            }
            if (files.isEmpty()) return done(QStringLiteral("none of its files are in this client"));
            store_source(kLoginKey, files);
            done(QString());
        });
    });
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
