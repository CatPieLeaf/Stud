// stud-webview -- the web-view panel the Roblox app asks for.
//
// The Lua app opens real web pages for parts of the experience that were
// never native (Messages and several menus). On Android those are shown
// in a WebView inside the app; Stud has no WebView, so the app's request
// went unanswered and left a dead grey panel with no way back.
//
// This is a viewer, never a login window. Stud's session is established
// inside the real Roblox app exactly as before -- this process is handed
// the cookies the engine itself produced (see webview_cookies.h) so the
// page opens already signed in as that same session. It never asks for
// credentials and has no way to.
//
// Input arrives on stdin rather than argv: a session cookie is a real
// credential and /proc/<pid>/cmdline is readable by anything running as
// this user. Format, one field per line:
//     url
//     title
//     user agent
//     cookie header      (zero or more, one per line)
#include <QApplication>
#include <QIcon>
#include <QDir>
#include <QFile>
#include <QMainWindow>
#include <QNetworkCookie>
#include <QTextStream>
#include <QUrl>
#include <QWebEngineCookieStore>
#include <QWebEnginePage>
#include <QWebEngineProfile>
#include <QWebEngineScript>
#include <QWebEngineScriptCollection>
#include <QWebEngineView>

#include "stud/webview_user_agent.h"

#include <cstdio>

namespace {

struct Request {
    QString url;
    QString title;
    QString userAgent;  // the whole agent Process B built for this panel
    QList<QNetworkCookie> cookies;
};

Request read_request() {
    Request request;
    QFile input;
    if (!input.open(stdin, QIODevice::ReadOnly | QIODevice::Text)) return request;
    QTextStream stream(&input);
    request.url = stream.readLine();
    request.title = stream.readLine();
    request.userAgent = stream.readLine();
    while (!stream.atEnd()) {
        const QString line = stream.readLine();
        if (line.isEmpty()) continue;
        // Each line is a Set-Cookie style header, which is exactly what
        // QNetworkCookie::parseCookies already understands.
        request.cookies.append(QNetworkCookie::parseCookies(line.toUtf8()));
    }
    return request;
}

}  // namespace

// What the injected bridge prefixes its console lines with, so an
// ordinary console.log from the page is not mistaken for one.
constexpr char kHybridPrefix[] = "__STUD_HYBRID__";

// Carries the page's bridge messages out on stdout, one per line, for
// render-host to hand to the engine. Nothing else about the page
// changes: real console output still goes where it went before.
class BridgePage : public QWebEnginePage {
public:
    using QWebEnginePage::QWebEnginePage;

protected:
    void javaScriptConsoleMessage(JavaScriptConsoleMessageLevel level, const QString& message,
                                  int line, const QString& source) override {
        if (message.startsWith(QLatin1String(kHybridPrefix))) {
            const QString payload = message.mid(int(sizeof(kHybridPrefix)) - 1);
            const QByteArray utf8 = payload.toUtf8();
            // One line, so the reader never has to guess where a message
            // ends. A JSON document has no raw newlines in it.
            std::printf("hybrid %s\n", utf8.constData());
            std::fflush(stdout);
            return;
        }
        // Everything else the page says, on this process's own stdout so
        // it lands in Stud's session log. Qt's default handler routes
        // through QLoggingCategory, which on this desktop goes to
        // journald instead -- so a challenge page failing was invisible
        // in the one file a user can actually be asked for.
        const QByteArray text = message.toUtf8();
        const QByteArray where = source.toUtf8();
        std::printf("stud-webview: js [%d] %s (%s:%d)\n", static_cast<int>(level),
                    text.constData(), where.constData(), line);
        std::fflush(stdout);
    }
};

int main(int argc, char** argv) {
    QApplication app(argc, argv);
    const Request request = read_request();
    if (request.url.isEmpty()) {
        std::fprintf(stderr, "stud-webview: no URL on stdin\n");
        return 2;
    }

    // Off-the-record: the app owns the session, so this window keeps no
    // profile of its own on disk to drift out of sync with it.
    auto* profile = new QWebEngineProfile(&app);

    // Ask Roblox for its mobile layout, which is what makes these panels
    // show the requested thing and nothing else.
    //
    // On a real device these pages open in the app's own Android
    // WebView, so roblox.com serves the mobile view: /my/messages is a
    // message list, with none of the desktop site's navigation, sidebar
    // or footer wrapped around it. QtWebEngine identifies as desktop
    // Chrome by default, so the same URL returns the full website --
    // which is exactly the difference between Stud's panel and Sober's.
    //
    // This is an honest description of the context rather than a spoof:
    // this window really is a mobile application's embedded web view,
    // hosting the real Android app. The Chrome version is QtWebEngine's
    // own, so nothing is invented about the browser's capabilities.
    // The agent Process B built: the real app's own web-view agent,
    // measured from this machine (runtime/framework, app_bridge.h's
    // build_web_view_user_agent). It carries the "ROBLOX Android App"
    // token, which is what makes roblox.com serve a page its in-app
    // version -- the login-challenge page enables its native transport
    // only then, and without it a completed OTP is announced to nobody.
    //
    // The fallback is a plain Android web-view agent (Chrome on
    // Android), for the case this process is run by hand with no agent
    // on its stdin.
    const QString agent = request.userAgent.trimmed().startsWith(QLatin1String("Mozilla/"))
                              ? request.userAgent.trimmed()
                              : QString::fromStdString(stud::webview::user_agent(std::string()));
    profile->setHttpUserAgent(agent);
    std::printf("stud-webview: user agent: %s\n", agent.toUtf8().constData());
    std::fflush(stdout);
    const QUrl url(request.url);
    for (const QNetworkCookie& cookie : request.cookies) {
        profile->cookieStore()->setCookie(cookie, url);
    }
    std::printf("stud-webview: %lld cookie(s) applied\n",
                static_cast<long long>(request.cookies.size()));
    std::fflush(stdout);

    QMainWindow window;
    // Deliberately the one-argument constructor: the overload taking a
    // profile arrived in Qt 6.4, and this builds against 6.2 on the
    // oldest distribution Stud targets. It makes no difference here --
    // the profile reaches the view through the page set below, which is
    // what binds it either way.
    auto* view = new QWebEngineView(&window);
    // A Roblox page talks to whatever is hosting it through a JavaScript
    // bridge, and that is how a login challenge says it is finished: the
    // real Android client exposes `__globalRobloxAndroidBridge__` with a
    // single `executeRoblox(json)` method (the app's own hybrid JS bridge's own
    // addJavascriptInterface call), and the page calls it. Without the
    // object being there at all, an OTP or a captcha completes and
    // nothing is ever told -- the login just stops.
    //
    // The message leaves this process as a console line, picked up by
    // the page subclass below. A console message is a real channel Qt
    // already gives every page, it needs no extra scheme, socket or
    // QWebChannel transport, and it carries a whole JSON document
    // without the length limits a URL has.
    {
        QWebEngineScript bridge;
        bridge.setName(QStringLiteral("stud-hybrid-bridge"));
        bridge.setInjectionPoint(QWebEngineScript::DocumentCreation);
        bridge.setWorldId(QWebEngineScript::MainWorld);
        bridge.setRunsOnSubFrames(true);
        bridge.setSourceCode(QStringLiteral(
            "window.__globalRobloxAndroidBridge__ = window.__globalRobloxAndroidBridge__ || {"
            "  executeRoblox: function (message) {"
            "    console.log('%1' + message);"
            "  }"
            "};").arg(QLatin1String(kHybridPrefix)));
        profile->scripts()->insert(bridge);
    }
    view->setPage(new BridgePage(profile, view));
    window.setCentralWidget(view);
    window.setWindowTitle(request.title.isEmpty() ? QStringLiteral("Roblox") : request.title);
    // Stud's own icon, by theme name, with the installed file as a
    // fallback for environments whose icon theme does not resolve it.
    QIcon icon = QIcon::fromTheme(QStringLiteral("stud"));
    if (icon.isNull()) {
        icon = QIcon(QStringLiteral("/usr/share/icons/hicolor/512x512/apps/stud.png"));
    }
    if (icon.isNull()) {
        icon = QIcon(QDir::homePath() +
                     QStringLiteral("/.local/share/icons/hicolor/512x512/apps/stud.png"));
    }
    if (!icon.isNull()) window.setWindowIcon(icon);
    // These panels are a single page of the site (a message list, one
    // settings section), not a browser -- so they open at a size that
    // suits that rather than filling the screen.
    window.resize(720, 620);
    // The page's own title is better than the one the app guessed, when
    // it has one.
    QObject::connect(view, &QWebEngineView::titleChanged, &window,
                     [&window](const QString& title) {
                         if (!title.isEmpty()) window.setWindowTitle(title);
                     });
    // A challenge page that fails to load at all looks identical, from
    // the outside, to one that loads and then cannot finish -- so say
    // which it is.
    QObject::connect(view, &QWebEngineView::loadFinished, view, [view](bool ok) {
        std::printf("stud-webview: page load %s\n", ok ? "finished" : "FAILED");
        std::fflush(stdout);
        if (!ok) return;
        // What the page has to talk to the app with. A Roblox page picks
        // its transport from what exists on the window: the Android
        // bridge object, or Roblox.Hybrid's own navigation helper. Names
        // and types only -- never any page content.
        view->page()->runJavaScript(
            QStringLiteral(
                "(function(){var r=window.Roblox||{};return JSON.stringify({"
                "bridge:typeof window.__globalRobloxAndroidBridge__,"
                "execute:typeof (window.__globalRobloxAndroidBridge__||{}).executeRoblox,"
                "roblox:typeof window.Roblox,"
                "robloxKeys:Object.keys(r).slice(0,40),"
                "hybrid:typeof r.Hybrid,"
                "hybridKeys:Object.keys(r.Hybrid||{}),"
                "navigation:typeof (r.Hybrid||{}).Navigation,"
                "nativeCallback:typeof ((r.Hybrid||{}).Bridge||{}).nativeCallback"
                "});})()"),
            [](const QVariant& result) {
                std::printf("stud-webview: page bridge state: %s\n",
                            result.toString().toUtf8().constData());
                std::fflush(stdout);
            });
    });
    QObject::connect(view->page(), &QWebEnginePage::renderProcessTerminated, view,
                     [](QWebEnginePage::RenderProcessTerminationStatus status, int code) {
                         std::printf("stud-webview: render process terminated status=%d code=%d\n",
                                     static_cast<int>(status), code);
                         std::fflush(stdout);
                     });
    view->setUrl(url);
    window.show();
    return app.exec();
}
