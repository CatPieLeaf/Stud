#include "fflags.h"

#include "stud/stud_paths.h"

#include <QDir>
#include <QFile>
#include <QJsonDocument>
#include <QSaveFile>

#include <array>

namespace stud::ui::fflags {

namespace {

// Roblox's allowlist for local flags, as published.
constexpr std::array kAllowlist = {
    "DFFlagDebugPauseVoxelizer",
    "DFFlagDisableDPIScale",
    "DFFlagTextureQualityOverrideEnabled",
    "DFIntCSGLevelOfDetailSwitchingDistance",
    "DFIntCSGLevelOfDetailSwitchingDistanceL12",
    "DFIntCSGLevelOfDetailSwitchingDistanceL23",
    "DFIntCSGLevelOfDetailSwitchingDistanceL34",
    "DFIntDebugFRMQualityLevelOverride",
    "DFIntTextureQualityOverride",
    "FFlagDebugGraphicsPreferD3D11",
    "FFlagDebugGraphicsPreferOpenGL",
    "FFlagDebugGraphicsPreferVulkan",
    "FFlagDebugSkyGray",
    "FFlagHandleAltEnterFullscreenManually",
    "FIntDebugForceMSAASamples",
    "FIntFRMMaxGrassDistance",
    "FIntFRMMinGrassDistance",
    "FIntGrassMovementReducedMotionFactor",
};

// Mesh detail: the four switching distances at level 9, scaled down by level.
constexpr std::array kMeshFlags = {"DFIntCSGLevelOfDetailSwitchingDistance",
                                   "DFIntCSGLevelOfDetailSwitchingDistanceL12",
                                   "DFIntCSGLevelOfDetailSwitchingDistanceL23",
                                   "DFIntCSGLevelOfDetailSwitchingDistanceL34"};
constexpr std::array kMeshBase = {2000, 1500, 1000, 500};
constexpr int kMeshLevels = 9;
constexpr std::array kGrassFlags = {"FIntFRMMinGrassDistance", "FIntFRMMaxGrassDistance",
                                    "FIntGrassMovementReducedMotionFactor"};

QString text(int value) { return QString::number(value); }
int number(const QJsonObject& flags, const char* key) {
    const QJsonValue v = flags.value(QLatin1String(key));
    if (v.isDouble()) return v.toInt();
    bool ok = false;
    const int n = v.toString().toInt(&ok);
    return ok ? n : -1;
}
bool on(const QJsonObject& flags, const char* key) {
    const QJsonValue v = flags.value(QLatin1String(key));
    return v.isBool() ? v.toBool() : v.toString().compare(QStringLiteral("true"), Qt::CaseInsensitive) == 0;
}
void toggle(QJsonObject& flags, const char* key, int value) {
    if (value == 1) {
        flags.insert(QLatin1String(key), QStringLiteral("True"));
    } else {
        flags.remove(QLatin1String(key));
    }
}

QString profile_dir() { return QString::fromStdString(stud::paths::config_dir()) + QStringLiteral("/flag-profiles"); }
QString profile_path(const QString& name) {
    QString safe = name;
    safe.replace(QLatin1Char('/'), QLatin1Char('_'));
    return profile_dir() + QLatin1Char('/') + safe + QStringLiteral(".json");
}

}  // namespace

bool allowlisted(const QString& name) {
    for (const char* allowed : kAllowlist) {
        if (name == QLatin1String(allowed)) return true;
    }
    return false;
}

QStringList not_allowlisted(const QJsonObject& flags) {
    QStringList out;
    for (auto it = flags.begin(); it != flags.end(); ++it) {
        if (!allowlisted(it.key())) out << it.key();
    }
    return out;
}

void set(QJsonObject& flags, Preset preset, int value) {
    switch (preset) {
        case Preset::Msaa:
            if (value > 0) flags.insert(QStringLiteral("FIntDebugForceMSAASamples"), text(value));
            else flags.remove(QStringLiteral("FIntDebugForceMSAASamples"));
            break;
        case Preset::TextureQuality:
            if (value >= 0) {
                flags.insert(QStringLiteral("DFFlagTextureQualityOverrideEnabled"), QStringLiteral("True"));
                flags.insert(QStringLiteral("DFIntTextureQualityOverride"), text(value));
            } else {
                flags.remove(QStringLiteral("DFFlagTextureQualityOverrideEnabled"));
                flags.remove(QStringLiteral("DFIntTextureQualityOverride"));
            }
            break;
        case Preset::QualityLevel:
            if (value > 0) flags.insert(QStringLiteral("DFIntDebugFRMQualityLevelOverride"), text(value));
            else flags.remove(QStringLiteral("DFIntDebugFRMQualityLevelOverride"));
            break;
        case Preset::MeshDetail:
            for (size_t i = 0; i < kMeshFlags.size(); ++i) {
                if (value >= 0) {
                    flags.insert(QLatin1String(kMeshFlags[i]), text(kMeshBase[i] * value / kMeshLevels));
                } else {
                    flags.remove(QLatin1String(kMeshFlags[i]));
                }
            }
            break;
        case Preset::DisableGrass:
            for (const char* key : kGrassFlags) {
                if (value == 1) flags.insert(QLatin1String(key), QStringLiteral("0"));
                else flags.remove(QLatin1String(key));
            }
            break;
        case Preset::GraySky: toggle(flags, "FFlagDebugSkyGray", value); break;
        case Preset::PauseVoxelizer: toggle(flags, "DFFlagDebugPauseVoxelizer", value); break;
        case Preset::KeepQualityWhenScaled: toggle(flags, "DFFlagDisableDPIScale", value); break;
    }
}

int get(const QJsonObject& flags, Preset preset) {
    switch (preset) {
        case Preset::Msaa: return number(flags, "FIntDebugForceMSAASamples");
        case Preset::TextureQuality:
            return on(flags, "DFFlagTextureQualityOverrideEnabled") ? number(flags, "DFIntTextureQualityOverride") : -1;
        case Preset::QualityLevel: return number(flags, "DFIntDebugFRMQualityLevelOverride");
        case Preset::MeshDetail: {
            const int first = number(flags, kMeshFlags[0]);
            // Rounded: the stored distances were truncated on the way in.
            return first < 0 ? -1 : (first * kMeshLevels + kMeshBase[0] / 2) / kMeshBase[0];
        }
        case Preset::DisableGrass:
            for (const char* key : kGrassFlags) {
                if (number(flags, key) != 0) return 0;
            }
            return 1;
        case Preset::GraySky: return on(flags, "FFlagDebugSkyGray") ? 1 : 0;
        case Preset::PauseVoxelizer: return on(flags, "DFFlagDebugPauseVoxelizer") ? 1 : 0;
        case Preset::KeepQualityWhenScaled: return on(flags, "DFFlagDisableDPIScale") ? 1 : 0;
    }
    return -1;
}

QStringList profiles() {
    QStringList names;
    for (const QString& file : QDir(profile_dir()).entryList({QStringLiteral("*.json")}, QDir::Files, QDir::Name)) {
        names << file.chopped(int(sizeof(".json")) - 1);
    }
    return names;
}

bool save_profile(const QString& name, const QJsonObject& flags) {
    QDir().mkpath(profile_dir());
    QSaveFile out(profile_path(name));
    const QByteArray data = QJsonDocument(flags).toJson(QJsonDocument::Indented);
    return out.open(QIODevice::WriteOnly) && out.write(data) == data.size() && out.commit();
}

QJsonObject load_profile(const QString& name) {
    QFile in(profile_path(name));
    return in.open(QIODevice::ReadOnly) ? QJsonDocument::fromJson(in.readAll()).object() : QJsonObject();
}

void delete_profile(const QString& name) { QFile::remove(profile_path(name)); }

}  // namespace stud::ui::fflags
