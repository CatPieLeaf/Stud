#pragma once

// FastFlags as the current client takes them: only the flags on Roblox's
// allowlist do anything when set locally, so Stud says which of the user's do
// not, and its presets use allowlisted flags only (the values Froststrap uses).

#include <QJsonObject>
#include <QString>
#include <QStringList>

namespace stud::ui::fflags {

// Roblox's allowlist of flags a local ClientAppSettings.json may set.
bool allowlisted(const QString& name);

// The flags in `flags` that are not on the allowlist.
QStringList not_allowlisted(const QJsonObject& flags);

// Presets: each sets its flags, or removes them for Roblox's default.
enum class Preset {
    Msaa,             // samples: 1, 2, 4
    TextureQuality,   // 0 lowest, 1 low, 2 medium
    QualityLevel,     // 1-21
    MeshDetail,       // 0-9
    DisableGrass,     // on/off
    GraySky,          // on/off
    PauseVoxelizer,   // on/off
    KeepQualityWhenScaled,  // on/off: DPI scaling without lowering quality
};

// Sets `preset` to `value` in `flags` (-1 removes it: Roblox's default; for an
// on/off preset, 1 is on).
void set(QJsonObject& flags, Preset preset, int value);

// What `flags` holds for `preset`, -1 when Roblox decides.
int get(const QJsonObject& flags, Preset preset);

// Named sets of flags, kept in ~/.config/stud/flag-profiles.
QStringList profiles();
bool save_profile(const QString& name, const QJsonObject& flags);
QJsonObject load_profile(const QString& name);
void delete_profile(const QString& name);

}  // namespace stud::ui::fflags
