// M8 test: real QtKeychain-backed credential storage (ui/src/credential_store.h).
// Runs against this machine's real keychain backend (KWallet, under this
// live Plasma session) -- not a mock. Uses a distinct test key so it
// doesn't collide with any real stored Roblox cookie.

#include "credential_store.h"

#include <QCoreApplication>

#include <cstdio>
#include <cstdlib>

namespace {

void check(bool condition, const char* what) {
    if (!condition) {
        std::fprintf(stderr, "FAILED: %s\n", what);
        std::exit(1);
    }
    std::printf("ok: %s\n", what);
}

}  // namespace

int main(int argc, char** argv) {
    // QtKeychain's Job classes need a running Qt event loop (QCoreApplication)
    // to dispatch their real, async D-Bus-backed (libsecret/KWallet) calls --
    // even though stud::ui's wrapper blocks synchronously via a nested
    // QEventLoop per call.
    QCoreApplication app(argc, argv);

    const QString kTestKey = "stud-credential-store-test-key";

    // Clean slate -- delete anything left over from a previous run.
    stud::ui::delete_credential(kTestKey);
    check(!stud::ui::load_credential(kTestKey).has_value(),
          "no stored value for the test key after a clean delete");

    QString error;
    bool stored = stud::ui::store_credential(kTestKey, "test-cookie-value-12345", &error);
    check(stored, ("store_credential succeeded (real keychain write)" +
                    (stored ? QString() : QString(": ") + error))
                       .toStdString()
                       .c_str());

    auto loaded = stud::ui::load_credential(kTestKey);
    check(loaded.has_value(), "load_credential found the real stored value");
    check(loaded.value() == "test-cookie-value-12345",
          "loaded value matches exactly what was stored");

    // Overwrite -- real keychains support updating an existing key.
    stud::ui::store_credential(kTestKey, "updated-value-67890");
    auto reloaded = stud::ui::load_credential(kTestKey);
    check(reloaded.has_value() && reloaded.value() == "updated-value-67890",
          "overwriting an existing key updates the real stored value");

    stud::ui::delete_credential(kTestKey);
    check(!stud::ui::load_credential(kTestKey).has_value(),
          "delete_credential really removes the value from the keychain");

    std::printf("all credential-store checks passed\n");
    return 0;
}
