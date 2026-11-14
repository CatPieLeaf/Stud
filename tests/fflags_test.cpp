// FastFlag presets and the allowlist.

#include "fflags.h"
#include "check.h"

#include <cstdio>

using stud::ui::fflags::Preset;

int main() {
    QJsonObject flags;
    for (const Preset preset : {Preset::Msaa, Preset::TextureQuality, Preset::QualityLevel,
                                Preset::MeshDetail}) {
        check(stud::ui::fflags::get(flags, preset) == -1, "an unset preset is Roblox's");
    }

    stud::ui::fflags::set(flags, Preset::Msaa, 4);
    check(flags["FIntDebugForceMSAASamples"].toString() == "4", "MSAA sets its flag");
    check(stud::ui::fflags::get(flags, Preset::Msaa) == 4, "MSAA reads back");

    stud::ui::fflags::set(flags, Preset::TextureQuality, 0);
    check(flags["DFFlagTextureQualityOverrideEnabled"].toString() == "True" &&
              flags["DFIntTextureQualityOverride"].toString() == "0",
          "texture quality enables the override and sets the level");
    check(stud::ui::fflags::get(flags, Preset::TextureQuality) == 0, "texture quality reads back");

    for (int level = 0; level <= 9; ++level) {
        stud::ui::fflags::set(flags, Preset::MeshDetail, level);
        check(stud::ui::fflags::get(flags, Preset::MeshDetail) == level, "each mesh detail level reads back");
    }
    check(flags["DFIntCSGLevelOfDetailSwitchingDistanceL34"].toString() == "500",
          "mesh detail 9 is Froststrap's full distances");

    stud::ui::fflags::set(flags, Preset::DisableGrass, 1);
    check(stud::ui::fflags::get(flags, Preset::DisableGrass) == 1, "grass off reads back");
    stud::ui::fflags::set(flags, Preset::DisableGrass, 0);
    check(!flags.contains("FIntFRMMinGrassDistance"), "grass back on removes its flags");

    stud::ui::fflags::set(flags, Preset::Msaa, -1);
    check(!flags.contains("FIntDebugForceMSAASamples"), "Roblox's default removes the flag");

    flags["FFlagSomethingRobloxIgnores"] = true;
    const QStringList ignored = stud::ui::fflags::not_allowlisted(flags);
    check(ignored == QStringList{"FFlagSomethingRobloxIgnores"},
          "only flags off the allowlist are reported");

    std::printf("all fflags checks passed\n");
    return 0;
}
