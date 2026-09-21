#pragma once

#include <string>

// The one place in Stud that writes the session log.
//
// The old arrangement had every process write the file itself, inline, on
// the thread that produced the line. That is what made the engine wait on
// disk I/O to say something, and a single printf on the render thread was
// enough to freeze a game join for seconds (see commit 431b9b5, and the
// tee's own note in session_log.h).
//
// So the work is split by who can afford to wait. B and C throw their
// lines at Process A -- a write into a kernel socket buffer, no disk
// behind it -- and carry straight on. Process A collects them in memory
// and writes the file in batches on a timer. A is the launcher; it has
// nothing to be late for.
//
// Nothing is batched on the sending side, and that is what makes this
// safe to depend on: a line is in A's memory the instant it is produced,
// so a process that dies abruptly has already handed over everything it
// ever said. The crash report flushes that memory before the dialog
// appears, which is the last chance to keep it.
//
// Process A's own output goes through the same socket -- it connects to
// its own collector -- so all three processes share one path with no
// special case for the one that happens to own the file.
namespace stud::ui {

// Starts listening on `socket_path` and writing to `log_path`.
//
// False if either cannot be opened, in which case the caller should leave
// STUD_LOG_SOCKET unset: the children then write the file themselves,
// which is what they did before this existed.
bool start_log_collector(const std::string& socket_path, const std::string& log_path);

// Everything collected so far, on disk now rather than at the next tick.
//
// Called when a crash is being reported. The process that died has
// already handed its lines over; this is what puts them in the file the
// user is about to be offered.
void flush_log_collector();

}  // namespace stud::ui
