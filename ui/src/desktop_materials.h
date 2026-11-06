#pragma once

#include <string>

namespace stud::ui {

// Windows Graphical Parity for this launch (see settings.h's
// windows_materials). Returns a directory holding:
//
//   assets/        laid over the APK's extracted assets by Process B: the
//                  combined shader pack and the terrain material file
//   brdfLUT.dds    the two textures the desktop material shaders read
//   normaldetail.dds  that the Android engine never binds (render-host)
//   shaders_vulkan_desktop.pack  the Windows pack, combined again by a
//                  newer Stud without fetching it
//
// or an empty string, for the Android look. Windows files of the APK's own
// release are kept for as long as the APK is, with nothing fetched; an APK
// of another release uses the newest Windows files and follows them. Every
// failure, offline, a package that does not match its published checksum,
// a pack that does not combine, is reported in the log and means the
// Android look, never a broken one.
std::string prepare_desktop_materials(const std::string& apk_path);

}  // namespace stud::ui
