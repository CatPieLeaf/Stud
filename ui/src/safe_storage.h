#pragma once

#include <QString>
#include <optional>

// Safe storage: the keyring holds a key, not the secret.
//
// This is the arrangement Chromium and Electron use, and the reason to
// prefer it here is the same: the keyring ends up with ONE long-lived
// entry -- "Stud Safe Storage" -- holding a random encryption key,
// instead of an entry per secret sitting there in plaintext under a name
// that announces what it is. A user browsing KWallet or Seahorse sees an
// opaque app key rather than a readable ".ROBLOSECURITY" value they
// might copy out, and revoking Stud's access is one deletion.
//
// The secret itself is encrypted with that key using AES-256-GCM, which
// authenticates as well as encrypts: a tampered or truncated file fails
// to decrypt rather than yielding a plausible-looking wrong value. The
// ciphertext lives under Stud's own data directory with owner-only
// permissions. Losing the keyring entry makes the ciphertext
// permanently unreadable, which is the intended behaviour -- it means a
// stolen copy of the file alone is worth nothing.
//
// Honest scope: this protects a secret at rest against someone reading
// files, and against the secret being casually visible in a keyring UI.
// It does not protect against a process already running as this user,
// which can ask the keyring for the key exactly as Stud does. No
// local-storage scheme can.

namespace stud::ui {

// The keyring entry that holds the encryption key. Named for what it is
// so it reads sensibly in KWallet/Seahorse.
inline constexpr auto kSafeStorageKeyName = "Stud Safe Storage";

// Encrypts `value` with the safe-storage key (creating that key on first
// use) and writes it under `name` in Stud's data directory. Returns
// false and sets *error_out on any failure -- a failed write must not
// look like a successful one, or a login silently stops persisting.
bool store_secret(const QString& name, const QString& value, QString* error_out = nullptr);

// Decrypts a previously stored secret. std::nullopt when it is absent,
// when the keyring key is gone, or when authentication fails.
std::optional<QString> load_secret(const QString& name);

// Removes the stored ciphertext. Not an error if it was not there.
void delete_secret(const QString& name);

}  // namespace stud::ui
