#pragma once

#include <string>

// The diagnostic header at the top of every session log: distribution, GPU, driver,
// session type and client. Read-only; a login is reported as present, never its value.
namespace stud::ui {

void write_diagnostics();

// The same text, for Settings to copy.
std::string diagnostics_text();

}  // namespace stud::ui
