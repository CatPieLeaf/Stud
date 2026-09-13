#include "safe_storage.h"

#include "stud/stud_paths.h"

#include "credential_store.h"

#include <QByteArray>
#include <QDir>
#include <QFile>
#include <QStandardPaths>

#include <openssl/evp.h>
#include <openssl/rand.h>

namespace stud::ui {

namespace {

constexpr int kKeyBytes = 32;   // AES-256
constexpr int kNonceBytes = 12; // GCM's own nominal size
constexpr int kTagBytes = 16;

// ~/.local/share/stud/secrets, spelled explicitly rather than taken from
// Qt's AppDataLocation: that returns the application NAME as Qt was told
// it ("Stud"), so the secrets landed in a differently-cased directory to
// every other path Stud uses. One spelling, shared with the other two
// processes (stud_paths.h).
QString secrets_dir() {
    const QString dir = QString::fromStdString(stud::paths::data_dir()) + "/secrets";
    QDir().mkpath(dir);
    return dir;
}


QString secret_path(const QString& name) {
    return secrets_dir() + "/" + name + ".bin";
}

// The key lives in the keyring, base64 so it survives a text-only
// backend. Generated once, on first use, from the system CSPRNG.
std::optional<QByteArray> safe_storage_key(QString* error_out) {
    if (auto existing = load_credential(kSafeStorageKeyName)) {
        const QByteArray key = QByteArray::fromBase64(existing->toUtf8());
        if (key.size() == kKeyBytes) return key;
        // Wrong size means it is not a key this build wrote. Replacing it
        // would silently destroy whatever it protects, so refuse instead.
        if (error_out != nullptr) *error_out = "existing safe-storage key is malformed";
        return std::nullopt;
    }
    QByteArray key(kKeyBytes, '\0');
    if (RAND_bytes(reinterpret_cast<unsigned char*>(key.data()), kKeyBytes) != 1) {
        if (error_out != nullptr) *error_out = "could not generate a safe-storage key";
        return std::nullopt;
    }
    QString write_error;
    if (!store_credential(kSafeStorageKeyName, QString::fromUtf8(key.toBase64()), &write_error)) {
        if (error_out != nullptr) *error_out = write_error;
        return std::nullopt;
    }
    return key;
}

}  // namespace

bool store_secret(const QString& name, const QString& value, QString* error_out) {
    const auto key = safe_storage_key(error_out);
    if (!key) return false;

    QByteArray nonce(kNonceBytes, '\0');
    if (RAND_bytes(reinterpret_cast<unsigned char*>(nonce.data()), kNonceBytes) != 1) {
        if (error_out != nullptr) *error_out = "could not generate a nonce";
        return false;
    }

    const QByteArray plain = value.toUtf8();
    QByteArray cipher(plain.size(), '\0');
    QByteArray tag(kTagBytes, '\0');

    EVP_CIPHER_CTX* ctx = EVP_CIPHER_CTX_new();
    if (ctx == nullptr) {
        if (error_out != nullptr) *error_out = "no cipher context";
        return false;
    }
    bool ok = EVP_EncryptInit_ex(ctx, EVP_aes_256_gcm(), nullptr, nullptr, nullptr) == 1 &&
              EVP_CIPHER_CTX_ctrl(ctx, EVP_CTRL_GCM_SET_IVLEN, kNonceBytes, nullptr) == 1 &&
              EVP_EncryptInit_ex(ctx, nullptr, nullptr,
                                 reinterpret_cast<const unsigned char*>(key->constData()),
                                 reinterpret_cast<const unsigned char*>(nonce.constData())) == 1;
    int len = 0;
    if (ok) {
        ok = EVP_EncryptUpdate(ctx, reinterpret_cast<unsigned char*>(cipher.data()), &len,
                               reinterpret_cast<const unsigned char*>(plain.constData()),
                               static_cast<int>(plain.size())) == 1;
    }
    int final_len = 0;
    if (ok) {
        ok = EVP_EncryptFinal_ex(ctx, reinterpret_cast<unsigned char*>(cipher.data()) + len,
                                 &final_len) == 1 &&
             EVP_CIPHER_CTX_ctrl(ctx, EVP_CTRL_GCM_GET_TAG, kTagBytes, tag.data()) == 1;
    }
    EVP_CIPHER_CTX_free(ctx);
    if (!ok) {
        if (error_out != nullptr) *error_out = "encryption failed";
        return false;
    }
    cipher.truncate(len + final_len);

    // nonce || tag || ciphertext, written whole then moved into place so
    // a crash mid-write cannot leave a half-file that fails to decrypt.
    const QString path = secret_path(name);
    const QString temp = path + ".new";
    QFile file(temp);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        if (error_out != nullptr) *error_out = "could not open " + temp;
        return false;
    }
    file.setPermissions(QFileDevice::ReadOwner | QFileDevice::WriteOwner);
    file.write(nonce);
    file.write(tag);
    file.write(cipher);
    file.close();
    QFile::remove(path);
    if (!QFile::rename(temp, path)) {
        QFile::remove(temp);
        if (error_out != nullptr) *error_out = "could not replace " + path;
        return false;
    }
    return true;
}

std::optional<QString> load_secret(const QString& name) {
    QFile file(secret_path(name));
    if (!file.open(QIODevice::ReadOnly)) return std::nullopt;
    const QByteArray blob = file.readAll();
    file.close();
    if (blob.size() < kNonceBytes + kTagBytes) return std::nullopt;

    const auto key = safe_storage_key(nullptr);
    if (!key) return std::nullopt;

    const QByteArray nonce = blob.left(kNonceBytes);
    QByteArray tag = blob.mid(kNonceBytes, kTagBytes);
    const QByteArray cipher = blob.mid(kNonceBytes + kTagBytes);
    QByteArray plain(cipher.size(), '\0');

    EVP_CIPHER_CTX* ctx = EVP_CIPHER_CTX_new();
    if (ctx == nullptr) return std::nullopt;
    bool ok = EVP_DecryptInit_ex(ctx, EVP_aes_256_gcm(), nullptr, nullptr, nullptr) == 1 &&
              EVP_CIPHER_CTX_ctrl(ctx, EVP_CTRL_GCM_SET_IVLEN, kNonceBytes, nullptr) == 1 &&
              EVP_DecryptInit_ex(ctx, nullptr, nullptr,
                                 reinterpret_cast<const unsigned char*>(key->constData()),
                                 reinterpret_cast<const unsigned char*>(nonce.constData())) == 1;
    int len = 0;
    if (ok) {
        ok = EVP_DecryptUpdate(ctx, reinterpret_cast<unsigned char*>(plain.data()), &len,
                               reinterpret_cast<const unsigned char*>(cipher.constData()),
                               static_cast<int>(cipher.size())) == 1;
    }
    if (ok) {
        ok = EVP_CIPHER_CTX_ctrl(ctx, EVP_CTRL_GCM_SET_TAG, kTagBytes, tag.data()) == 1;
    }
    int final_len = 0;
    if (ok) {
        // Fails when the file was tampered with or the key is wrong --
        // which is the point of GCM: no plausible-looking wrong answer.
        ok = EVP_DecryptFinal_ex(ctx, reinterpret_cast<unsigned char*>(plain.data()) + len,
                                 &final_len) == 1;
    }
    EVP_CIPHER_CTX_free(ctx);
    if (!ok) return std::nullopt;
    plain.truncate(len + final_len);
    return QString::fromUtf8(plain);
}

void delete_secret(const QString& name) { QFile::remove(secret_path(name)); }

}  // namespace stud::ui
