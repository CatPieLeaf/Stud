#pragma once

#include "mac_client.h"

// The client's login cookies live in the system keyring between sessions: written
// out as the client's cookie file for a launch, saved back and removed after it.
// Without a reachable keyring the file stays on disk as before.
namespace stud::ui::login {

// Before the client starts.
void restore(client::Product product);

// After the client exits.
void seal(client::Product product);

}  // namespace stud::ui::login
