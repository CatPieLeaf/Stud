#include "login_keyring.h"

#include <QFile>
#include <QFileDevice>
#include <QSaveFile>

// GLib names a struct field `signals`, which Qt defines as a macro.
#pragma push_macro("signals")
#undef signals
#include <libsecret/secret.h>
#pragma pop_macro("signals")

#include <cstdio>

namespace stud::ui::login {

namespace {

// Whether this session's restore reached the keyring, per product; a missing jar
// after a session only means "signed out" when it did.
bool g_keyring_read[2] = {false, false};

const SecretSchema* schema() {
    static const SecretSchema kSchema = {
        "io.github.catpieleaf.Stud.Login", SECRET_SCHEMA_NONE,
        {{"jar", SECRET_SCHEMA_ATTRIBUTE_STRING}, {nullptr, SECRET_SCHEMA_ATTRIBUTE_STRING}}};
    return &kSchema;
}

QString jar_name(client::Product product) {
    return product == client::Product::Studio ? QStringLiteral("com.Roblox.RobloxStudio.cookies.tsv")
                                              : QStringLiteral("com.roblox.RobloxPlayer.cookies.tsv");
}

QString jar_path(client::Product product) {
    const QString home = client::prefix_home();
    return home.isEmpty() ? QString() : home + QStringLiteral("/Library/Cookies/") + jar_name(product);
}

bool report(GError* error, const char* what) {
    if (error == nullptr) return true;
    std::fprintf(stderr, "stud: keyring %s failed: %s\n", what, error->message);
    g_error_free(error);
    return false;
}

bool store(const QString& name, const QByteArray& jar) {
    GError* error = nullptr;
    const QByteArray key = name.toUtf8();
    secret_password_store_sync(schema(), SECRET_COLLECTION_DEFAULT, "Stud: Roblox login", jar.constData(),
                               nullptr, &error, "jar", key.constData(), nullptr);
    return report(error, "save");
}

bool write_private(const QString& path, const QByteArray& data) {
    QSaveFile out(path);
    if (!out.open(QIODevice::WriteOnly)) return false;
    out.setPermissions(QFileDevice::ReadOwner | QFileDevice::WriteOwner);
    out.write(data);
    return out.commit();
}

}  // namespace

void restore(client::Product product) {
    bool& read = g_keyring_read[product == client::Product::Studio];
    read = false;
    const QString path = jar_path(product);
    if (path.isEmpty()) return;
    QFile file(path);
    if (file.open(QIODevice::ReadOnly)) {
        // Left on disk by a session that didn't end here, or from before the keyring.
        const QByteArray jar = file.readAll();
        file.close();
        file.setPermissions(QFileDevice::ReadOwner | QFileDevice::WriteOwner);
        if (!jar.isEmpty()) read = store(jar_name(product), jar);
        return;
    }
    GError* error = nullptr;
    const QByteArray key = jar_name(product).toUtf8();
    gchar* jar = secret_password_lookup_sync(schema(), nullptr, &error, "jar", key.constData(), nullptr);
    if (!report(error, "read")) return;
    read = true;
    if (jar == nullptr) return;
    if (!write_private(path, QByteArray(jar))) std::fprintf(stderr, "stud: could not write the login for Roblox\n");
    secret_password_free(jar);
}

void seal(client::Product product) {
    const QString path = jar_path(product);
    if (path.isEmpty()) return;
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        // The client removed it: signed out.
        if (!g_keyring_read[product == client::Product::Studio]) return;
        GError* error = nullptr;
        const QByteArray key = jar_name(product).toUtf8();
        secret_password_clear_sync(schema(), nullptr, &error, "jar", key.constData(), nullptr);
        report(error, "clear");
        return;
    }
    const QByteArray jar = file.readAll();
    file.close();
    // Only removed once the keyring holds it, so a missing keyring never signs anyone out.
    if (!jar.isEmpty() && store(jar_name(product), jar)) QFile::remove(path);
}

}  // namespace stud::ui::login
