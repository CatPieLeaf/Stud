#pragma once

// The client ignores local flags off Roblox's allowlist, so Stud flags those and its
// presets only use allowlisted ones (Froststrap's values).

#include <QJsonObject>
#include <QString>
#include <QStringList>

namespace stud::ui::fflags {

bool allowlisted(const QString& name);

QStringList not_allowlisted(const QJsonObject& flags);

enum class Preset {
    Msaa,             // samples: 1, 2, 4
    TextureQuality,   // 0 lowest, 1 low, 2 medium, 3 high
    QualityLevel,     // 1-21
    MeshDetail,       // 0-9
    DisableGrass,     // on/off
    GraySky,          // on/off
    PauseVoxelizer,   // on/off
    KeepQualityWhenScaled,  // on/off: DPI scaling without lowering quality
};

// -1 removes it (Roblox's default); on/off presets use 1 for on.
void set(QJsonObject& flags, Preset preset, int value);

// -1 when Roblox decides.
int get(const QJsonObject& flags, Preset preset);

// Saved in ~/.config/stud/flag-profiles.
QStringList profiles();
bool save_profile(const QString& name, const QJsonObject& flags);
QJsonObject load_profile(const QString& name);
void delete_profile(const QString& name);

}  // namespace stud::ui::fflags
