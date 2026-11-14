#pragma once

#include "mac_client.h"

#include <optional>

namespace stud::ui {

// Asks which to open, Roblox or Roblox Studio: what Stud shows when it is
// started on its own, from the application menu or a desktop entry. Empty
// when the window is closed without a choice.
std::optional<client::Product> choose_product();

}  // namespace stud::ui
