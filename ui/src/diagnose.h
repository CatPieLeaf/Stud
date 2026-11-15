#pragma once

#include <string>

// The diagnostic header every session log opens with.
//
// A bug report needs a distribution, a GPU, a driver, which session type
// is in use and which client is installed, all of which Stud already
// knows and used to make someone ask for. It is written into the log
// itself rather than hidden behind a flag nobody would think to run:
// the log is what gets sent, so the facts belong at the top of it.
//
// Reads state and prints it. Changes nothing, launches nothing, and
// never prints a credential, a login is reported as present or
// absent, never its value.
namespace stud::ui {

void write_diagnostics();

// The same text, for copying into a report.
std::string diagnostics_text();

}  // namespace stud::ui
