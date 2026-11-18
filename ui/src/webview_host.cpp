#include "webview_host.h"

#include <QApplication>
#include <QCloseEvent>
#include <QCoreApplication>
#include <QDateTime>
#include <QDir>
#include <QEventLoop>
#include <QFile>
#include <QHash>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLocalServer>
#include <QLocalSocket>
#include <QNetworkCookie>
#include <QPointer>
#include <QStandardPaths>
#include <QWebChannel>
#include <QWebEngineCookieStore>
#include <QWebEngineHistory>
#include <QWebEngineHttpRequest>
#include <QWebEngineLoadingInfo>
#include <QWebEnginePage>
#include <QWebEngineProfile>
#include <QWebEngineProfileBuilder>
#include <QWebEngineScript>
#include <QWebEngineScriptCollection>
#include <QWebEngineSettings>
#include <QWebEngineUrlRequestInterceptor>
#include <QWebEngineView>

#include <cstdio>
#include <functional>
#include <map>
#include <utility>

#include "stud/stud_paths.h"

namespace stud::ui::webview {
namespace {

class Connection;

// True in the helper, whose windows draw inside the client's.
bool g_embedded = false;

// One persistent profile, so logins last as in the client's WebKit store.
QWebEngineProfile* profile() {
    static QWebEngineProfile* shared = [] {
        QWebEngineProfileBuilder builder;
        builder.setPersistentStoragePath(QString::fromStdString(stud::paths::data_dir()) +
                                         QStringLiteral("/webview"));
        builder.setCachePath(QString::fromStdString(stud::paths::cache_dir()) + QStringLiteral("/webview"));
        builder.setPersistentCookiesPolicy(QWebEngineProfile::ForcePersistentCookies);
        return builder.createProfile(QStringLiteral("stud"), qApp);
    }();
    return shared;
}

// Chromium only reports cookie changes, so the jar is tracked from the first load.
QHash<QString, QNetworkCookie>& cookie_jar() {
    static QHash<QString, QNetworkCookie> jar;
    static const bool followed = [] {
        auto key = [](const QNetworkCookie& cookie) {
            return QString::fromUtf8(cookie.name()) + QLatin1Char('\n') + cookie.domain() +
                   QLatin1Char('\n') + cookie.path();
        };
        QWebEngineCookieStore* store = profile()->cookieStore();
        QObject::connect(store, &QWebEngineCookieStore::cookieAdded, qApp,
                         [key](const QNetworkCookie& cookie) { jar.insert(key(cookie), cookie); });
        QObject::connect(store, &QWebEngineCookieStore::cookieRemoved, qApp,
                         [key](const QNetworkCookie& cookie) { jar.remove(key(cookie)); });
        store->loadAllCookies();
        return true;
    }();
    (void)followed;
    return jar;
}

QString channel_library() {
    static const QString source = [] {
        QFile file(QStringLiteral(":/qtwebchannel/qwebchannel.js"));
        if (!file.open(QIODevice::ReadOnly)) {
            std::fprintf(stderr, "stud: webview: QtWebChannel's qwebchannel.js is missing\n");
            return QString();
        }
        return QString::fromUtf8(file.readAll());
    }();
    return source;
}

QString quote(const QString& text) {
    const QByteArray json = QJsonDocument(QJsonArray{text}).toJson(QJsonDocument::Compact);
    return QString::fromUtf8(json.mid(1, json.size() - 2));
}

// WebKit's window.webkit.messageHandlers: postMessage(body) sends {"v": body}
// once the channel is up. __studSetHandlers replaces them on a loaded page.
QString handlers_script(const QStringList& names) {
    const QString list = QString::fromUtf8(
        QJsonDocument(QJsonArray::fromStringList(names)).toJson(QJsonDocument::Compact));
    return QStringLiteral(R"((function () {
  if (!window.__studSetHandlers) {
    var queue = [], bridge = null;
    function send(name, body) {
      var text;
      try { text = JSON.stringify({ v: body }); } catch (e) { text = '{}'; }
      if (text === undefined) text = '{}';
      if (bridge) bridge.post(name, text); else queue.push([name, text]);
    }
    Object.defineProperty(window, '__studSetHandlers', { value: function (names) {
      var handlers = {};
      names.forEach(function (name) {
        handlers[name] = { postMessage: function (body) { send(name, body); } };
      });
      window.webkit = window.webkit || {};
      window.webkit.messageHandlers = handlers;
    } });
    if (typeof QWebChannel !== 'undefined' && window.qt && qt.webChannelTransport) {
      new QWebChannel(qt.webChannelTransport, function (channel) {
        bridge = channel.objects.studBridge;
        queue.forEach(function (m) { bridge.post(m[0], m[1]); });
        queue = [];
      });
    }
  }
  window.__studSetHandlers(%1);
})();)")
        .arg(list);
}

// The client's user agent instead of Chromium's.
QString agent_script(const QString& agent) {
    return QStringLiteral(
               "(() => { const agent = %1; for (const [key, value] of [['userAgent', agent], "
               "['appVersion', agent.replace(/^Mozilla\\//, '')]]) "
               "Object.defineProperty(Navigator.prototype, key, { get() { return value; }, "
               "configurable: true }); })();")
        .arg(quote(agent));
}

// Evaluates at global scope and returns JSON: {"v": value}, {"u": true} for
// undefined, {"t": true} for what WebKit can't bridge, {"e": message}.
QString evaluation_script(const QString& source) {
    return QStringLiteral(R"((function () {
  var v;
  try { v = (0, eval)(%1); } catch (e) { return JSON.stringify({ e: String(e) }); }
  if (v === undefined) return JSON.stringify({ u: true });
  if (typeof v === 'function' || typeof v === 'symbol') return JSON.stringify({ t: true });
  try { return JSON.stringify({ v: v }); } catch (e) { return JSON.stringify({ t: true }); }
})())")
        .arg(quote(source));
}

int navigation_type(QWebEnginePage::NavigationType type) {
    switch (type) {
    case QWebEnginePage::NavigationTypeLinkClicked: return 0;
    case QWebEnginePage::NavigationTypeFormSubmitted: return 1;
    case QWebEnginePage::NavigationTypeBackForward: return 2;
    case QWebEnginePage::NavigationTypeReload: return 3;
    default: return -1;
    }
}

struct NsError {
    QString domain;
    int code;
};

NsError ns_error(QWebEngineLoadingInfo::ErrorDomain domain, int code) {
    const QString url_domain = QStringLiteral("NSURLErrorDomain");
    if (domain == QWebEngineLoadingInfo::CertificateErrorDomain) return {url_domain, -1202};
    switch (code) {
    case -3: return {url_domain, -999};                // ERR_ABORTED: cancelled
    case -7: case -118: return {url_domain, -1001};    // timed out
    case -100: case -101: return {url_domain, -1005};  // connection lost
    case -102: case -109: return {url_domain, -1004};  // cannot connect to host
    case -105: case -137: return {url_domain, -1003};  // cannot find host
    case -106: return {url_domain, -1009};             // not connected to the internet
    case -300: return {url_domain, -1000};             // bad URL
    case -301: case -302: return {url_domain, -1002};  // unsupported URL
    case -310: return {url_domain, -1007};             // too many redirects
    default: return {url_domain, -1};
    }
}

class View;

class Bridge : public QObject {
    Q_OBJECT
public:
    explicit Bridge(View* view);
    Q_INVOKABLE void post(const QString& name, const QString& json);

private:
    View* view_;
};

class AgentInterceptor : public QWebEngineUrlRequestInterceptor {
public:
    using QWebEngineUrlRequestInterceptor::QWebEngineUrlRequestInterceptor;
    QByteArray agent;
    void interceptRequest(QWebEngineUrlRequestInfo& info) override {
        if (!agent.isEmpty()) info.setHttpHeader("User-Agent", agent);
    }
};

class Page;

class View : public QWebEngineView {
public:
    View(Connection* connection, const QString& id, const QString& agent, bool developer);
    ~View() override;

    Connection* connection;
    const QString id;
    Page* page_;
    // Set while a call waits on the client; destroying the page waits for it.
    int busy = 0;
    bool dead = false;
    bool loading = false;
    double progress = 0.0;
    bool started = false;
    // A navigation the client cancelled; WebKit wouldn't report its aftermath.
    QUrl rejected;
    QStringList handlers;
    struct UserScript {
        QString source;
        bool at_start;
        bool main_only;
    };
    QList<UserScript> scripts;
    QString agent;
    AgentInterceptor* interceptor;
    Bridge* bridge;

    void rebuild_scripts();
    void send_state();
    void finish_busy() {
        if (--busy == 0 && dead) deleteLater();
    }

protected:
    void closeEvent(QCloseEvent* event) override;
};

class Page : public QWebEnginePage {
public:
    Page(View* view, QWebEngineProfile* profile) : QWebEnginePage(profile, view), view_(view) {}

protected:
    bool acceptNavigationRequest(const QUrl& url, NavigationType type, bool main) override;
    void javaScriptAlert(const QUrl& origin, const QString& message) override {
        dialog(QStringLiteral("alert"), origin, message, QString(), nullptr);
    }
    bool javaScriptConfirm(const QUrl& origin, const QString& message) override {
        return dialog(QStringLiteral("confirm"), origin, message, QString(), nullptr);
    }
    bool javaScriptPrompt(const QUrl& origin, const QString& message, const QString& fallback,
                          QString* result) override {
        return dialog(QStringLiteral("prompt"), origin, message, fallback, result);
    }
    QWebEnginePage* createWindow(WebWindowType type) override;

private:
    bool dialog(const QString& kind, const QUrl& origin, const QString& message, const QString& fallback,
                QString* result);
    View* view_;
};

// Opened for window.open: reports the target URL and closes.
class OpenerPage : public QWebEnginePage {
public:
    OpenerPage(View* opener) : QWebEnginePage(profile(), opener), opener_(opener) {}

protected:
    bool acceptNavigationRequest(const QUrl& url, NavigationType, bool) override;

private:
    QPointer<View> opener_;
};

class Connection : public QObject {
public:
    explicit Connection(QLocalSocket* socket) : QObject(socket), socket_(socket) {
        connect(socket, &QLocalSocket::readyRead, this, [this] { read(); });
        connect(socket, &QLocalSocket::disconnected, this, [this] { closed(); });
    }

    void send(const QStringList& fields) {
        if (closed_) return;
        QByteArray frame;
        auto append = [&frame](quint32 value) {
            for (int shift = 0; shift < 32; shift += 8) frame.append(char((value >> shift) & 0xff));
        };
        append(quint32(fields.size()));
        for (const QString& field : fields) {
            const QByteArray bytes = field.toUtf8();
            append(quint32(bytes.size()));
            frame.append(bytes);
        }
        socket_->write(frame);
        socket_->flush();
    }

    // Runs Qt's events while waiting. Empty if the client disconnected.
    QStringList wait(const QString& call) {
        QEventLoop loop;
        waits_[call] = Wait{&loop, {}};
        loop.exec();
        QStringList answer = waits_[call].answer;
        waits_.erase(call);
        if (closed_ && waits_.empty()) socket_->deleteLater();
        return answer;
    }

    QString next_call() { return QString::number(++calls_); }

private:
    struct Wait {
        QEventLoop* loop;
        QStringList answer;
    };

    void answer(const QString& call, const QStringList& fields) {
        auto it = waits_.find(call);
        if (it == waits_.end()) return;
        it->second.answer = fields;
        it->second.loop->quit();
    }

    void closed() {
        closed_ = true;
        for (auto& [call, wait] : waits_) wait.loop->quit();
        for (const auto& [key, view] : views_) {
            if (view.isNull()) continue;
            view->dead = true;
            view->hide();
            if (view->busy == 0) view->deleteLater();
        }
        views_.clear();
        // A waiting call deletes it instead.
        if (waits_.empty()) socket_->deleteLater();
    }

    void read() {
        buffer_.append(socket_->readAll());
        for (;;) {
            qsizetype at = 0;
            auto take = [this, &at](quint32* value) {
                if (buffer_.size() < at + 4) return false;
                *value = 0;
                for (int i = 0; i < 4; ++i) *value |= quint32(quint8(buffer_[at + i])) << (8 * i);
                at += 4;
                return true;
            };
            quint32 count = 0;
            if (!take(&count)) return;
            QStringList fields;
            bool complete = true;
            for (quint32 i = 0; i < count; ++i) {
                quint32 length = 0;
                if (!take(&length) || buffer_.size() < at + qsizetype(length)) {
                    complete = false;
                    break;
                }
                fields.append(QString::fromUtf8(buffer_.mid(at, length)));
                at += length;
            }
            if (!complete) return;
            buffer_.remove(0, at);
            if (!fields.isEmpty()) handle(fields);
        }
    }

    View* view(const QString& id) {
        auto it = views_.find(id);
        return it == views_.end() || it->second.isNull() || it->second->dead ? nullptr : it->second.data();
    }

    void handle(const QStringList& m);

    QLocalSocket* socket_;
    QByteArray buffer_;
    std::map<QString, QPointer<View>> views_;
    std::map<QString, Wait> waits_;
    quint64 calls_ = 0;
    bool closed_ = false;
};

Bridge::Bridge(View* view) : QObject(view), view_(view) {}

void Bridge::post(const QString& name, const QString& json) {
    view_->connection->send({QStringLiteral("message"), view_->id, name, json, view_->page()->url().toString()});
}

View::View(Connection* connection_, const QString& id_, const QString& agent_, bool developer)
    : connection(connection_), id(id_), agent(agent_) {
    setAttribute(Qt::WA_DeleteOnClose, false);
    setWindowTitle(QStringLiteral("Stud"));
    if (g_embedded) {
        // The client's window is the frame; Rosemary's compositor matches views by title.
        setWindowFlag(Qt::FramelessWindowHint);
        setWindowTitle(QStringLiteral("stud-view-") + id);
    }
    QWebEngineProfile* shared = profile();
    if (shared->httpUserAgent() != agent && !shared->property("stud-agent").isValid()) {
        shared->setHttpUserAgent(agent);
        shared->setProperty("stud-agent", agent);
    }
    cookie_jar();
    page_ = new Page(this, shared);
    interceptor = new AgentInterceptor(page_);
    page_->setUrlRequestInterceptor(interceptor);
    setPage(page_);
    page_->settings()->setAttribute(QWebEngineSettings::JavascriptCanOpenWindows, true);
    page_->settings()->setAttribute(QWebEngineSettings::LocalContentCanAccessFileUrls, true);
    (void)developer;
    bridge = new Bridge(this);
    auto* channel = new QWebChannel(page_);
    channel->registerObject(QStringLiteral("studBridge"), bridge);
    page_->setWebChannel(channel, QWebEngineScript::MainWorld);
    rebuild_scripts();

    connect(page_, &QWebEnginePage::titleChanged, this, [this](const QString& title) {
        if (!g_embedded) setWindowTitle(title.isEmpty() ? QStringLiteral("Stud") : title);
        send_state();
    });
    connect(page_, &QWebEnginePage::urlChanged, this, [this] { send_state(); });
    connect(page_, &QWebEnginePage::loadProgress, this, [this](int percent) {
        progress = percent / 100.0;
        send_state();
    });
    connect(page_, &QWebEnginePage::windowCloseRequested, this, [this] {
        hide();
        connection->send({QStringLiteral("closed"), id});
    });
    connect(page_, &QWebEnginePage::renderProcessTerminated, this,
            [this] { connection->send({QStringLiteral("terminated"), id}); });
    connect(page_, &QWebEnginePage::loadingChanged, this, [this](const QWebEngineLoadingInfo& info) {
        const QString url = info.url().toString();
        const auto status = info.status();
        if (status == QWebEngineLoadingInfo::LoadStartedStatus) {
            loading = true;
            send_state();
            return;
        }
        loading = false;
        const bool was_started = std::exchange(started, false);
        if (!rejected.isEmpty() && info.url() == rejected) {
            rejected.clear();
            send_state();
            return;
        }
        if (!was_started) connection->send({QStringLiteral("started"), id, url});
        const bool http_status = info.errorDomain() == QWebEngineLoadingInfo::HttpStatusCodeDomain;
        if (status == QWebEngineLoadingInfo::LoadSucceededStatus || http_status) {
            const int code = http_status ? info.errorCode() : 200;
            connection->send({QStringLiteral("response"), id, url, QString::number(code), QStringLiteral("1")});
            connection->send({QStringLiteral("committed"), id, url});
            connection->send({QStringLiteral("finished"), id, url});
        } else if (status == QWebEngineLoadingInfo::LoadStoppedStatus) {
            connection->send({QStringLiteral("failed"), id, QStringLiteral("1"), url,
                              QStringLiteral("NSURLErrorDomain"), QStringLiteral("-999"),
                              QStringLiteral("cancelled")});
        } else {
            const NsError error = ns_error(info.errorDomain(), info.errorCode());
            connection->send({QStringLiteral("failed"), id, QStringLiteral("1"), url, error.domain,
                              QString::number(error.code), info.errorString()});
        }
        send_state();
    });
}

View::~View() = default;

void View::rebuild_scripts() {
    QWebEngineScriptCollection& collection = page_->scripts();
    collection.clear();
    auto add = [&collection](const QString& name, const QString& source, bool at_start, bool main_only) {
        QWebEngineScript script;
        script.setName(name);
        script.setSourceCode(source);
        script.setInjectionPoint(at_start ? QWebEngineScript::DocumentCreation : QWebEngineScript::DocumentReady);
        script.setWorldId(QWebEngineScript::MainWorld);
        script.setRunsOnSubFrames(!main_only);
        collection.insert(script);
    };
    // Before any page or client script: the user agent, then the bridge.
    if (agent != profile()->httpUserAgent()) add(QStringLiteral("stud-agent"), agent_script(agent), true, false);
    add(QStringLiteral("stud-channel"), channel_library(), true, true);
    add(QStringLiteral("stud-handlers"), handlers_script(handlers), true, true);
    int index = 0;
    for (const UserScript& script : scripts) {
        add(QStringLiteral("stud-user-%1").arg(index++), script.source, script.at_start, script.main_only);
    }
}

void View::send_state() {
    if (dead) return;
    QWebEngineHistory* history = page_->history();
    connection->send({QStringLiteral("state"), id, page_->url().toString(), page_->title(),
                      loading ? QStringLiteral("1") : QStringLiteral("0"),
                      QString::number(loading ? progress : 1.0),
                      history->canGoBack() ? QStringLiteral("1") : QStringLiteral("0"),
                      history->canGoForward() ? QStringLiteral("1") : QStringLiteral("0")});
}

void View::closeEvent(QCloseEvent* event) {
    // Hidden, not destroyed: the client decides when the view goes.
    event->ignore();
    hide();
    // Closes Roblox's overlay through the page's own bridge, like its close button.
    page_->runJavaScript(
        QStringLiteral("window.Roblox && Roblox.Hybrid && Roblox.Hybrid.Overlay && "
                       "typeof Roblox.Hybrid.Overlay.close === 'function' && "
                       "(Roblox.Hybrid.Overlay.close(), true)"),
        QWebEngineScript::MainWorld);
    connection->send({QStringLiteral("closed"), id});
}

bool Page::acceptNavigationRequest(const QUrl& url, NavigationType type, bool main) {
    View* view = view_;
    if (view->dead) return false;
    const QString decision = view->connection->next_call();
    const bool redirect = type == NavigationTypeRedirect;
    view->connection->send({QStringLiteral("policy"), view->id, decision, url.toString(),
                            QString::number(navigation_type(type)), main ? QStringLiteral("1") : QStringLiteral("0"),
                            QStringLiteral("0")});
    ++view->busy;
    const QStringList answer = view->connection->wait(decision);
    const bool alive = !view->dead;
    view->finish_busy();
    if (!alive) return false;
    const bool allowed = !answer.isEmpty() && answer.first() == QLatin1String("1");
    if (!main) return allowed;
    if (!allowed) {
        view->rejected = url;
        return false;
    }
    view->rejected.clear();
    if (redirect && view->started) {
        view->connection->send({QStringLiteral("redirect"), view->id, url.toString()});
    } else if (!view->started) {
        view->started = true;
        view->connection->send({QStringLiteral("started"), view->id, url.toString()});
    }
    return true;
}

bool Page::dialog(const QString& kind, const QUrl& origin, const QString& message, const QString& fallback,
                  QString* result) {
    View* view = view_;
    if (view->dead) return false;
    const QString call = view->connection->next_call();
    view->connection->send({QStringLiteral("dialog"), view->id, call, kind, message, fallback, origin.toString()});
    ++view->busy;
    const QStringList answer = view->connection->wait(call);
    view->finish_busy();
    const bool ok = !answer.isEmpty() && answer.first() == QLatin1String("1");
    if (ok && result != nullptr) *result = answer.value(1);
    return ok;
}

QWebEnginePage* Page::createWindow(WebWindowType) { return new OpenerPage(view_); }

bool OpenerPage::acceptNavigationRequest(const QUrl& url, NavigationType, bool) {
    if (!opener_.isNull() && !opener_->dead) {
        opener_->connection->send({QStringLiteral("new_window"), opener_->id, url.toString()});
    }
    deleteLater();
    return false;
}

void Connection::handle(const QStringList& m) {
    const QString& kind = m.first();
    if (kind == QLatin1String("decide") || kind == QLatin1String("dialog_reply")) {
        answer(m.value(1), m.mid(2));
        return;
    }
    if (kind == QLatin1String("create")) {
        if (View* old = view(m.value(1))) {
            old->dead = true;
            old->hide();
            if (old->busy == 0) old->deleteLater();
        }
        views_[m.value(1)] = new View(this, m.value(1), m.value(2), m.value(3) == QLatin1String("1"));
        return;
    }
    if (kind == QLatin1String("cookies_get")) {
        QJsonArray list;
        for (const QNetworkCookie& cookie : cookie_jar()) {
            QJsonObject entry{{QStringLiteral("name"), QString::fromUtf8(cookie.name())},
                              {QStringLiteral("value"), QString::fromUtf8(cookie.value())},
                              {QStringLiteral("domain"), cookie.domain()},
                              {QStringLiteral("path"), cookie.path()},
                              {QStringLiteral("secure"), cookie.isSecure()}};
            if (!cookie.isSessionCookie()) {
                entry.insert(QStringLiteral("expires"), double(cookie.expirationDate().toMSecsSinceEpoch()) / 1000.0);
            }
            list.append(entry);
        }
        send({QStringLiteral("cookies"), m.value(1),
              QString::fromUtf8(QJsonDocument(list).toJson(QJsonDocument::Compact))});
        return;
    }
    if (kind == QLatin1String("cookie_set") || kind == QLatin1String("cookie_delete")) {
        const bool set = kind == QLatin1String("cookie_set");
        QNetworkCookie cookie(m.value(2).toUtf8(), set ? m.value(3).toUtf8() : QByteArray());
        cookie.setDomain(m.value(set ? 4 : 3));
        cookie.setPath(m.value(set ? 5 : 4));
        if (set) {
            cookie.setSecure(m.value(6) == QLatin1String("1"));
            bool timed = false;
            const double expires = m.value(7).toDouble(&timed);
            if (timed) cookie.setExpirationDate(QDateTime::fromMSecsSinceEpoch(qint64(expires * 1000.0)));
            profile()->cookieStore()->setCookie(cookie);
        } else {
            profile()->cookieStore()->deleteCookie(cookie);
        }
        send({QStringLiteral("cookie_done"), m.value(1)});
        return;
    }
    View* target = view(m.value(1));
    if (target == nullptr) return;
    if (kind == QLatin1String("destroy")) {
        views_.erase(m.value(1));
        target->dead = true;
        target->hide();
        if (target->busy == 0) target->deleteLater();
    } else if (kind == QLatin1String("scripts")) {
        target->scripts.clear();
        const int count = m.value(2).toInt();
        for (int i = 0; i < count; ++i) {
            const int at = 3 + i * 3;
            target->scripts.append({m.value(at), m.value(at + 1) == QLatin1String("1"),
                                    m.value(at + 2) == QLatin1String("1")});
        }
        target->rebuild_scripts();
    } else if (kind == QLatin1String("handlers")) {
        target->handlers = m.mid(2);
        target->rebuild_scripts();
        target->page_->runJavaScript(handlers_script(target->handlers), QWebEngineScript::MainWorld);
    } else if (kind == QLatin1String("agent")) {
        target->agent = m.value(2);
        target->interceptor->agent =
            target->agent == profile()->httpUserAgent() ? QByteArray() : target->agent.toUtf8();
        target->rebuild_scripts();
    } else if (kind == QLatin1String("show")) {
        if (g_embedded) {
            const int width = m.value(2).toInt();
            const int height = m.value(3).toInt();
            if (width > 0 && height > 0) target->resize(width, height);
            target->show();
            return;
        }
        // Default window size, not the view's, which fills the client window.
        target->show();
        target->raise();
        target->activateWindow();
    } else if (kind == QLatin1String("hide")) {
        target->hide();
    } else if (kind == QLatin1String("load")) {
        QWebEngineHttpRequest request(QUrl(m.value(2)));
        const QByteArray method = m.value(3).toUtf8();
        if (method == "POST") request.setMethod(QWebEngineHttpRequest::Post);
        const QJsonObject headers = QJsonDocument::fromJson(m.value(4).toUtf8()).object();
        for (auto it = headers.begin(); it != headers.end(); ++it) {
            request.setHeader(it.key().toUtf8(), it.value().toString().toUtf8());
        }
        target->page_->load(request);
    } else if (kind == QLatin1String("html")) {
        target->page_->setHtml(m.value(2), QUrl(m.value(3)));
    } else if (kind == QLatin1String("back")) {
        target->page_->triggerAction(QWebEnginePage::Back);
    } else if (kind == QLatin1String("forward")) {
        target->page_->triggerAction(QWebEnginePage::Forward);
    } else if (kind == QLatin1String("reload")) {
        target->page_->triggerAction(QWebEnginePage::Reload);
    } else if (kind == QLatin1String("stop")) {
        target->page_->triggerAction(QWebEnginePage::Stop);
    } else if (kind == QLatin1String("edit")) {
        static const QHash<QString, QWebEnginePage::WebAction> actions{
            {QStringLiteral("Copy"), QWebEnginePage::Copy},       {QStringLiteral("Cut"), QWebEnginePage::Cut},
            {QStringLiteral("Paste"), QWebEnginePage::Paste},     {QStringLiteral("SelectAll"), QWebEnginePage::SelectAll},
            {QStringLiteral("Undo"), QWebEnginePage::Undo},       {QStringLiteral("Redo"), QWebEnginePage::Redo}};
        if (actions.contains(m.value(2))) target->page_->triggerAction(actions.value(m.value(2)));
    } else if (kind == QLatin1String("eval")) {
        const QString call = m.value(2);
        QPointer<Connection> self(this);
        target->page_->runJavaScript(evaluation_script(m.value(3)), QWebEngineScript::MainWorld,
                                     [self, call](const QVariant& result) {
                                         if (self.isNull()) return;
                                         self->send({QStringLiteral("eval_result"), call, result.toString()});
                                     });
    }
}

QLocalServer* g_server = nullptr;

}  // namespace

int run_helper(int argc, char** argv) {
    g_embedded = true;
    QApplication app(argc, argv);
    app.setQuitOnLastWindowClosed(false);
    bool ok = false;
    const int fd = qEnvironmentVariable("STUD_WEBVIEW_FD").toInt(&ok);
    auto* socket = new QLocalSocket(&app);
    if (!ok || !socket->setSocketDescriptor(fd)) {
        std::fprintf(stderr, "stud: webview helper: no socket from Rosemary\n");
        return 2;
    }
    new Connection(socket);
    QObject::connect(socket, &QLocalSocket::disconnected, &app, &QCoreApplication::quit);
    return app.exec();
}

QString listen() {
    if (g_server != nullptr) return g_server->fullServerName();
    const QString dir = QStandardPaths::writableLocation(QStandardPaths::RuntimeLocation);
    const QString path = QDir(dir.isEmpty() ? QDir::tempPath() : dir)
                             .filePath(QStringLiteral("stud-webview-%1").arg(QCoreApplication::applicationPid()));
    QLocalServer::removeServer(path);
    auto* server = new QLocalServer(qApp);
    server->setSocketOptions(QLocalServer::UserAccessOption);
    if (!server->listen(path)) {
        std::fprintf(stderr, "stud: webview: could not listen on %s: %s\n", qPrintable(path),
                     qPrintable(server->errorString()));
        delete server;
        return QString();
    }
    QObject::connect(server, &QLocalServer::newConnection, server, [server] {
        while (QLocalSocket* socket = server->nextPendingConnection()) new Connection(socket);
    });
    g_server = server;
    return server->fullServerName();
}

}  // namespace stud::ui::webview

#include "webview_host.moc"
