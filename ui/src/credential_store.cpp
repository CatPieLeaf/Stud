#include "credential_store.h"

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

std::optional<QString> load_credential(const QString& key) {
    QKeychain::ReadPasswordJob job(kKeychainService);
    job.setAutoDelete(false);
    job.setKey(key);

    QEventLoop loop;
    QObject::connect(&job, &QKeychain::Job::finished, &loop, &QEventLoop::quit);
    job.start();
    loop.exec();

    if (job.error() != QKeychain::NoError) {
        return std::nullopt;
    }
    return job.textData();
}

void delete_credential(const QString& key) {
    QKeychain::DeletePasswordJob job(kKeychainService);
    job.setAutoDelete(false);
    job.setKey(key);

    QEventLoop loop;
    QObject::connect(&job, &QKeychain::Job::finished, &loop, &QEventLoop::quit);
    job.start();
    loop.exec();
    // Errors here (e.g. key didn't exist) are not actionable for callers
    // -- "log out" should succeed either way.
}

}  // namespace stud::ui
