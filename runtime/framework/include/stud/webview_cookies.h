#pragma once

#include <functional>
#include <string>
#include <vector>

namespace stud::jni_bridge {

// The session cookies the engine is using RIGHT NOW.
//
// The engine's own HTTP layer calls
// `JNICookieProtocol.OnSetCookieHandler.onSetCookie(cookies[], url)`
// every time it sees a Set-Cookie it wants a web view's jar to know
// about (CookieProtocol, where a real device forwards them
// into Android's WebView CookieManager). Stud has no CookieManager, so
// it keeps the two that matter and nothing else:
//
//   .ROBLOSECURITY  the signed-in session
//   rbxas           the account switcher's own cookie, which is what
//                   carries the SET of signed-in accounts, observed
//                   live on apis.roblox.com/account-switcher/v1/
//                   getLoggedInUsersMetadata and reissued by
//                   auth.roblox.com/v2/login on every switch
//
// This is NOT the login-time snapshot that was removed earlier: it is
// overwritten on every Set-Cookie, so after switching accounts it holds
// the account the user is now on, not the one they started with. That
// distinction is the whole point; Roblox supports several signed-in
// accounts and a panel must open as the current one.
//
// Why it is needed at all, when the engine can be asked directly: after
// a switch, `nativeGetCookiesForDomain("https://www.roblox.com")`
// returns only `GuestData`, the engine files the new session under the
// auth/apis domains instead, so reading one domain's jar is not enough.
// Live-confirmed, and it is why web-view panels opened after a switch
// were signed out.
//
// These are real credentials. Nothing here logs a value, and callers
// must not either, name and count only.
void set_session_cookie_sink(std::function<void(const std::string&)> sink);

// Where a refreshed `rbxas` is sent so every signed-in account, not just
// the active one, survives a restart.
void set_account_list_cookie_sink(std::function<void(const std::string&)> sink);

// Called from the engine's own callback thread with the raw Set-Cookie
// headers it just saw.
void note_engine_cookies(const std::string& url, const std::vector<std::string>& headers);

// The current session's cookies as Set-Cookie style headers, newest
// values, for handing to a web view. Empty before the engine has issued
// any.
std::vector<std::string> current_session_cookies();

// Seeds the current values at bring-up from whatever was restored from
// safe storage, so the first panel of a session is signed in even before
// the engine reissues anything.
void seed_current_session_cookie(const std::string& name, const std::string& value);

// The names in `headers`, for logging. Never returns a value.
std::string cookie_names(const std::vector<std::string>& headers);

}  // namespace stud::jni_bridge
