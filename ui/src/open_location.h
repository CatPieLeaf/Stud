#pragma once

#include <QUrl>

namespace stud::ui {

// Opens `url`, a folder, file or web address, with the desktop's own handler.
void open_location(const QUrl& url);

}  // namespace stud::ui
