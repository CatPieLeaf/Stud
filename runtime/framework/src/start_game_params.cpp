#include "stud/start_game_params.h"
#include "stud/game_activity_stubs.h"

namespace stud::jni_bridge {

BEGIN_NATIVE_DESCRIPTOR(StartGameParams)
{ FakeJni::Function<&StartGameParams::accessCode>{}, "accessCode" },
{ FakeJni::Function<&StartGameParams::callId>{}, "callId" },
{ FakeJni::Function<&StartGameParams::conversationId>{}, "conversationId" },
{ FakeJni::Function<&StartGameParams::deviceParams>{}, "deviceParams" },
{ FakeJni::Function<&StartGameParams::eventId>{}, "eventId" },
{ FakeJni::Function<&StartGameParams::gameId>{}, "gameId" },
{ FakeJni::Function<&StartGameParams::gameIdToExclude>{}, "gameIdToExclude" },
{ FakeJni::Function<&StartGameParams::gameJoinContext>{}, "gameJoinContext" },
{ FakeJni::Function<&StartGameParams::isUnder13>{}, "isUnder13" },
{ FakeJni::Function<&StartGameParams::isoContext>{}, "isoContext" },
{ FakeJni::Function<&StartGameParams::joinAttemptId>{}, "joinAttemptId" },
{ FakeJni::Function<&StartGameParams::joinAttemptOrigin>{}, "joinAttemptOrigin" },
{ FakeJni::Function<&StartGameParams::joinRequestType>{}, "joinRequestType" },
{ FakeJni::Function<&StartGameParams::launchData>{}, "launchData" },
{ FakeJni::Function<&StartGameParams::linkCode>{}, "linkCode" },
{ FakeJni::Function<&StartGameParams::placeId>{}, "placeId" },
{ FakeJni::Function<&StartGameParams::platformParams>{}, "platformParams" },
{ FakeJni::Function<&StartGameParams::referralPage>{}, "referralPage" },
{ FakeJni::Function<&StartGameParams::referredByPlayerId>{}, "referredByPlayerId" },
{ FakeJni::Function<&StartGameParams::reservedServerAccessCode>{}, "reservedServerAccessCode" },
{ FakeJni::Function<&StartGameParams::surface>{}, "surface" },
{ FakeJni::Function<&StartGameParams::userId>{}, "userId" },
{ FakeJni::Function<&StartGameParams::username>{}, "username" },
{ FakeJni::Function<&StartGameParams::vrContext>{}, "vrContext" },
END_NATIVE_DESCRIPTOR

// Real, deliberate departure from the earlier host-side plan of calling
// gamejoin.roblox.com/PlaceLauncher.ashx directly and parsing its
// response: Sober (this project's own real reference, sober-oss/, a
// real, working, open-source Roblox-on-Linux launcher) has ZERO code
// for either endpoint. It runs the real, unmodified engine and hands
// it the deep link; the real engine's own internal code does the join-
// ticket resolution itself. Every real journalctl capture this session
// (four separate real Sober sessions) shows "Joining game ..." firing
// INSIDE libroblox.so, right after StartGameWithParam, with no external
// HTTP call visible at the host/wrapper level, consistent with that.
// Stud runs the same real, unmodified engine, so the architecturally
// consistent thing to try first is feeding it the same real fields a
// plain (non-ticket) deep link already demonstrably works with, not
// reimplementing what the engine already does internally. Honest
// caveat: this project has not yet live-confirmed that placeId/
// joinAttemptId/referredByPlayerId/joinAttemptOrigin ALONE (without the
// opaque gameinfo ticket also going somewhere real) are sufficient for
// a real join to complete. That's the next real thing to test.
std::shared_ptr<StartGameParams> build_desktop_start_game_params(
    std::shared_ptr<PlatformParams> platform_params, std::shared_ptr<DeviceParams> device_params,
    std::shared_ptr<SurfaceStub> surface, const DeepLinkJoinInfo& deep_link) {
    auto params = std::make_shared<StartGameParams>();
    params->accessCode_ = std::make_shared<FakeJni::JString>(deep_link.access_code);
    params->callId_ = std::make_shared<FakeJni::JString>("");
    params->conversationId_ = 0;
    params->deviceParams_ = std::move(device_params);
    params->eventId_ = std::make_shared<FakeJni::JString>(deep_link.event_id);
    params->gameId_ = std::make_shared<FakeJni::JString>(deep_link.game_instance_id);
    params->gameIdToExclude_ = std::make_shared<FakeJni::JString>("");
    params->gameJoinContext_ = std::make_shared<FakeJni::JString>(deep_link.game_join_context);
    params->isUnder13_ = false;
    params->isoContext_ = std::make_shared<FakeJni::JString>("");
    params->joinAttemptId_ = std::make_shared<FakeJni::JString>(deep_link.join_attempt_id);
    params->joinAttemptOrigin_ = std::make_shared<FakeJni::JString>(deep_link.join_attempt_origin);
    params->joinRequestType_ = 0;
    // Real, live-tested-and-failed-once-already follow-up (see
    // DeepLinkJoinInfo::launch_data's own doc comment): placeId/
    // joinAttemptId/referredByPlayerId/joinAttemptOrigin ALONE, without
    // this, did not trigger a real join in a real test this session
    // (clean run, zero traps, but no "Joining game" FLog line either).
    // Trying the raw opaque gameinfo ticket here next.
    params->launchData_ = std::make_shared<FakeJni::JString>(deep_link.launch_data);
    params->linkCode_ = std::make_shared<FakeJni::JString>("");
    // Real, tested (the engineering notes, "proceed" entry): a real, well-
    // known place ID (1818, "Classic: Crossroads") was tried ALONE here
    // as a cheap experiment before deep-link parsing existed,
    // confirmed via a real run's FLog output to change NOTHING
    // (identical behavior to placeId=0), a bare place ID isn't
    // sufficient by itself. `deep_link.place_id` below is the real,
    // structurally-different follow-up: the SAME real place ID plus its
    // own real joinAttemptId/referredByPlayerId/joinAttemptOrigin from
    // an actual deep link, all four together, not yet live-tested
    // (see this function's own top-of-file doc comment).
    params->placeId_ = deep_link.place_id;
    params->platformParams_ = std::move(platform_params);
    params->referralPage_ = std::make_shared<FakeJni::JString>("");
    params->referredByPlayerId_ = deep_link.referred_by_player_id;
    params->reservedServerAccessCode_ = std::make_shared<FakeJni::JString>("");
    params->surface_ = surface;
    // The real, logged-in user. These were 0/""; Stud was asking the
    // engine to join an experience as nobody. A real client fills both
    // from the authenticated session, and the join request is built from
    // them. The identity is already fetched by Process A
    // (users.roblox.com/v1/users/authenticated) and handed to
    // set_native_user_identity() during bring-up, so read it back from
    // the same place rather than plumbing a second copy through.
    params->userId_ = native_user_id();
    params->username_ = std::make_shared<FakeJni::JString>(native_username());
    params->vrContext_ = std::make_shared<ActivityStub>();
    return params;
}

}  // namespace stud::jni_bridge
