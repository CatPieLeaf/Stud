#pragma once

#include <string>

namespace stud::ui {

// Windows-quality materials for this launch (see settings.h's
// windows_materials). Returns a directory holding:
//
//   assets/        laid over the APK's extracted assets by Process B: the
//                  combined shader pack and the terrain material file
//   brdfLUT.dds    the two textures the desktop material shaders read
//   normaldetail.dds  that the Android engine never binds (render-host)
//
// or an empty string, for the Android look. Nothing is fetched while the
// APK is one it has already prepared for; a new or updated APK is looked
// at again. Every failure, offline, no Windows build for the APK's
// version, a package that does not match its published checksum, a pack
// that does not combine, is reported in the log and means the Android
// look, never a broken one.
std::string prepare_desktop_materials(const std::string& apk_path);

}  // namespace stud::ui
