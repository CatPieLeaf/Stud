#pragma once

#include <QString>
#include <optional>

// Real credential storage (M8, locked decision: "QtKeychain:
// cross-DE (backs onto libsecret on GNOME, KWallet on Plasma, etc.)").
// Used for the Roblox auth cookie (.ROBLOSECURITY) after a real
// QtWebEngine login (see login_window.h), not exercised with real
// Roblox credentials in this codebase's own tests, but the storage
// mechanism itself is fully real and testable in isolation with a
// synthetic value.
//
// Synchronous wrapper over QtKeychain's real async Job API (WritePasswordJob/
// ReadPasswordJob/DeletePasswordJob) via a local QEventLoop, QtKeychain's
// own documented pattern for simple, blocking-style call sites like a
// settings/login window doesn't need to be async for.

namespace stud::ui {

// NOT for secrets. This is the raw keyring, and the only thing Stud puts
// in it is the safe-storage key (safe_storage.h), never a session
// cookie or any other credential, not even transiently and not to
// migrate one out of an older layout. A secret goes through
// store_secret()/load_secret(), which encrypt it and leave the keyring
// holding a key alone.


// "Stud" is the QtKeychain "service" name (a real key/service pair,
// not app-branded credential-store keys under someone else's identity).
inline constexpr auto kKeychainService = "Stud";

// Blocking write. Returns true on success; on failure, *error_out
// (if non-null) is set to QtKeychain's own real error string.
bool store_credential(const QString& key, const QString& value, QString* error_out = nullptr);

// Blocking read. std::nullopt if the key doesn't exist or on any
// other real error, callers can't distinguish those two cases from
// this return value alone (matches "no stored login yet" and "keychain
// unavailable" both meaning "show the login window" for this app).
std::optional<QString> load_credential(const QString& key);

// Blocking delete, e.g. for a future "log out" action. Not an
// error if the key didn't exist.
void delete_credential(const QString& key);

}  // namespace stud::ui
