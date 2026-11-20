#pragma once

#include <string_view>

namespace stud::ui {

// A roblox: or roblox-player: link, which goes to the client unchanged.
bool is_roblox_link(std::string_view argument);

}  // namespace stud::ui
