#pragma once

// `stud-ui --diagnose`: everything a bug report needs, in one page of
// text the user can paste.
//
// This exists because the alternative is a back-and-forth asking for a
// distribution, a GPU, a driver version, whether the APK is configured,
// and which session type is in use -- all of which Stud already knows.
// It reads state and prints it; it changes nothing, launches nothing,
// and prints no credential (the login is reported as present or absent,
// never its value).
namespace stud::ui {

int run_diagnose();

}  // namespace stud::ui
