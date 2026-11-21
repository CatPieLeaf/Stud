#pragma once

// Requests for what Stud gets from GitHub, with jsDelivr's mirror of GitHub beside it:
// the host Settings prefers is asked first, and the other one when that fails.

#include <QByteArray>
#include <QString>
#include <QUrl>

#include <functional>

namespace stud::ui::github {

// jsDelivr's copy of a raw.githubusercontent.com file; empty for anything else.
QUrl jsdelivr_copy(const QUrl& url);

// GET `github`, or `jsdelivr` (empty when jsDelivr has no copy) for the same thing.
// `done` gets the body and whether jsDelivr sent it, or why both hosts failed.
void get_either(const QUrl& github, const QUrl& jsdelivr,
                std::function<void(const QByteArray& body, bool from_jsdelivr, const QString& error)> done);

// GET `url`, a GitHub file coming from jsDelivr instead when that is preferred or
// GitHub fails; any other address is fetched as it is.
void get(const QUrl& url, std::function<void(const QByteArray& body, const QString& error)> done);

}  // namespace stud::ui::github
