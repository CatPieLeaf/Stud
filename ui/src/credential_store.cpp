#include "credential_store.h"

#include <cstdio>

#include <qt6keychain/keychain.h>

#include <QEventLoop>

namespace stud::ui {

bool store_credential(const QString& key, const QString& value, QString* error_out) {
    QKeychain::WritePasswordJob job(kKeychainService);
    job.setAutoDelete(false);
    job.setKey(key);
    job.setTextData(value);

    QEventLoop loop;
    QObject::connect(&job, &QKeychain::Job::finished, &loop, &QEventLoop::quit);
    job.start();
    loop.exec();

    if (job.error() != QKeychain::NoError) {
        if (error_out != nullptr) {
            *error_out = job.errorString();
        }
        return false;
    }
    return true;
}

namespace {
// Set by load_credential(); see last_load_was_absent().
bool g_last_load_absent = false;
}  // namespace

std::optional<QString> load_credential(const QString& key) {
    QKeychain::ReadPasswordJob job(kKeychainService);
    job.setAutoDelete(false);
    job.setKey(key);

    QEventLoop loop;
    QObject::connect(&job, &QKeychain::Job::finished, &loop, &QEventLoop::quit);
    job.start();
    loop.exec();

    if (job.error() != QKeychain::NoError) {
        g_last_load_absent = job.error() == QKeychain::EntryNotFound;
        if (!g_last_load_absent) {
            std::fprintf(stderr, "stud: could not read \"%s\" from the keyring: %s\n",
                         key.toUtf8().constData(), job.errorString().toUtf8().constData());
        }
        return std::nullopt;
    }
    g_last_load_absent = false;
    return job.textData();
}

bool last_load_was_absent() { return g_last_load_absent; }

void delete_credential(const QString& key) {
    QKeychain::DeletePasswordJob job(kKeychainService);
    job.setAutoDelete(false);
    job.setKey(key);

    QEventLoop loop;
    QObject::connect(&job, &QKeychain::Job::finished, &loop, &QEventLoop::quit);
    job.start();
    loop.exec();
    // Errors here (e.g. key didn't exist) are not actionable for callers
    // "log out" should succeed either way.
}

}  // namespace stud::ui
