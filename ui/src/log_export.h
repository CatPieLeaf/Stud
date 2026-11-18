#pragma once

#include <QString>

class QWidget;

namespace stud::ui {

// Every session log plus Roblox's logs from the same span, as a .tar.gz. Returns the
// message to show; empty when nothing was saved (failures report themselves).
QString export_all_logs(QWidget* parent);

// Only the newest session log, as a .log.
QString export_latest_log(QWidget* parent);

}  // namespace stud::ui
