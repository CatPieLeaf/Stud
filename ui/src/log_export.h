#pragma once

#include <QString>

class QWidget;

namespace stud::ui {

// Every session log, and Roblox's own logs from the same span, as one
// .tar.gz the user picks the place of. What to tell the user once it is
// saved; empty when there was nothing to save, the dialog was cancelled, or
// saving failed (which this says itself).
QString export_all_logs(QWidget* parent);

// Only the newest session log, saved as the .log it is. As above.
QString export_latest_log(QWidget* parent);

}  // namespace stud::ui
