#pragma once

#include <fake-jni/fake-jni.h>

#include <memory>
#include <string>

#include "stud/device_params.h"
#include "stud/game_activity_stubs.h"
#include "stud/platform_params.h"

// com.roblox.engine.jni.autovalue.StartGameParams, reimplemented as a real
// FakeJni object -- same approach as StartAppParams/InitParams. Field
// layout is ground-truth, traced directly from the app's own code of
// the class itself (24 real AutoValue getter methods -- see
// the engineering notes, the "V2 app-bridge API" entry). Real getter methods,
// not plain fields, same already-confirmed reason as InitParams.

namespace stud::jni_bridge {

class StartGameParams : public FakeJni::JObject {
public:
    DEFINE_CLASS_NAME("com/roblox/engine/jni/autovalue/StartGameParams")

    std::shared_ptr<FakeJni::JString> accessCode_;
    std::shared_ptr<FakeJni::JString> callId_;
    FakeJni::JLong conversationId_ = 0;
    std::shared_ptr<DeviceParams> deviceParams_;
    std::shared_ptr<FakeJni::JString> eventId_;
    std::shared_ptr<FakeJni::JString> gameId_;
    std::shared_ptr<FakeJni::JString> gameIdToExclude_;
    std::shared_ptr<FakeJni::JString> gameJoinContext_;
    FakeJni::JBoolean isUnder13_ = false;
    std::shared_ptr<FakeJni::JString> isoContext_;
    std::shared_ptr<FakeJni::JString> joinAttemptId_;
    std::shared_ptr<FakeJni::JString> joinAttemptOrigin_;
    FakeJni::JInt joinRequestType_ = 0;
    std::shared_ptr<FakeJni::JString> launchData_;
    std::shared_ptr<FakeJni::JString> linkCode_;
    FakeJni::JLong placeId_ = 0;
    std::shared_ptr<PlatformParams> platformParams_;
    std::shared_ptr<FakeJni::JString> referralPage_;
    FakeJni::JLong referredByPlayerId_ = 0;
    std::shared_ptr<FakeJni::JString> reservedServerAccessCode_;
    std::shared_ptr<SurfaceStub> surface_;
    FakeJni::JLong userId_ = 0;
    std::shared_ptr<FakeJni::JString> username_;
    std::shared_ptr<ActivityStub> vrContext_;

    std::shared_ptr<FakeJni::JString> accessCode() { return accessCode_; }
    std::shared_ptr<FakeJni::JString> callId() { return callId_; }
    FakeJni::JLong conversationId() { return conversationId_; }
    std::shared_ptr<DeviceParams> deviceParams() { return deviceParams_; }
    std::shared_ptr<FakeJni::JString> eventId() { return eventId_; }
    std::shared_ptr<FakeJni::JString> gameId() { return gameId_; }
    std::shared_ptr<FakeJni::JString> gameIdToExclude() { return gameIdToExclude_; }
    std::shared_ptr<FakeJni::JString> gameJoinContext() { return gameJoinContext_; }
    FakeJni::JBoolean isUnder13() { return isUnder13_; }
    std::shared_ptr<FakeJni::JString> isoContext() { return isoContext_; }
    std::shared_ptr<FakeJni::JString> joinAttemptId() { return joinAttemptId_; }
    std::shared_ptr<FakeJni::JString> joinAttemptOrigin() { return joinAttemptOrigin_; }
    FakeJni::JInt joinRequestType() { return joinRequestType_; }
    std::shared_ptr<FakeJni::JString> launchData() { return launchData_; }
    std::shared_ptr<FakeJni::JString> linkCode() { return linkCode_; }
    FakeJni::JLong placeId() { return placeId_; }
    std::shared_ptr<PlatformParams> platformParams() { return platformParams_; }
    std::shared_ptr<FakeJni::JString> referralPage() { return referralPage_; }
    FakeJni::JLong referredByPlayerId() { return referredByPlayerId_; }
    std::shared_ptr<FakeJni::JString> reservedServerAccessCode() { return reservedServerAccessCode_; }
    std::shared_ptr<SurfaceStub> surface() { return surface_; }
    FakeJni::JLong userId() { return userId_; }
    std::shared_ptr<FakeJni::JString> username() { return username_; }
    std::shared_ptr<ActivityStub> vrContext() { return vrContext_; }
};

// Real fields parsed out of a real roblox-player:// deep link's own
// place_launcher_url query string (see ui/src/launch_uri.h's own doc
// comment) -- passed in explicitly, rather than this file taking a
// stud-ipc dependency, since jni-bridge doesn't otherwise depend on it.
// Zero/empty fields (the default -- e.g. a bare, non-deep-link launch)
// are passed straight through to StartGameParams' own matching fields
// as honest zero/empty placeholders, same as before this existed.
struct DeepLinkJoinInfo {
    long long place_id = 0;
    std::string join_attempt_id;
    long long referred_by_player_id = 0;
    std::string join_attempt_origin;
    // The real, opaque `gameinfo:` ticket from the deep link itself
    // (LaunchUri::game_info / LaunchPayload::game_info) -- a real,
    // structurally plausible try for StartGameParams' own launchData_
    // field (matching AutoValue's real naming convention for "extra
    // launch payload data"), not yet live-confirmed correct. The
    // plaintext fields above (place_id etc.) alone were already live-
    // tested this session and did NOT trigger a real join -- this is
    // the next real, cheap, honest thing to try, not fabricated schema
    // guessing (a wrong string here is a no-op, same as the empty
    // placeholder it replaces, not something that could corrupt a real
    // join attempt).
    std::string launch_data;
    // The rest of the real launch request. the app's own launch-request parser reads every one of
    // these out of the same JSON and the app's own app-shell helper feeds them to
    // StartGameParams -- sending only a place id is what a "not authorized
    // to join this experience" answer looks like from the other side.
    std::string game_join_context;
    std::string event_id;
    std::string access_code;
    std::string game_instance_id;
};

// Builds a real, honest StartGameParams. `launch_data`/`game_join_context`
// are the two real fields AutoValue_StartGameParams.Builder() itself
// pre-seeds with "" on a real device (see StartGameParams's own
// builder() factory). `deep_link` (see DeepLinkJoinInfo above) feeds
// placeId/joinAttemptId/referredByPlayerId/joinAttemptOrigin when a
// real deep link supplied them -- real, structurally-confirmed fields,
// not guessed (this project has NOT yet live-confirmed these alone are
// sufficient for the real engine to complete a real join; see this
// function's own .cpp doc comment for the reasoning behind trying this
// over reimplementing Roblox's own internal join-ticket resolution
// host-side). accessCode_/reservedServerAccessCode_ remain honest
// empty placeholders -- no real source for those yet. `surface` must be
// the SAME Surface object already handed to GameActivity's own
// lifecycle (see GameActivityLifecycleResult::surface's doc comment),
// not a fresh one -- avoids a second, real, independently-mapped window.
std::shared_ptr<StartGameParams> build_desktop_start_game_params(
    std::shared_ptr<PlatformParams> platform_params, std::shared_ptr<DeviceParams> device_params,
    std::shared_ptr<SurfaceStub> surface, const DeepLinkJoinInfo& deep_link = {});

}  // namespace stud::jni_bridge
