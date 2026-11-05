#include "stud/start_game_params.h"
#include "stud/app_java_classes.h"

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

// The engine resolves a join ticket itself, so Stud doesn't call
// PlaceLauncher.ashx: it hands over the launch request's fields and the engine
// does the rest.
int join_request_type_for(const DeepLinkJoinInfo& join) {
    const bool place = join.place_id > 0;
    if (join.user_id > 0) return 1;
    if (place && join.conversation_id > 0) return 6;
    if (!place) return -1;
    if (!join.link_code.empty() || !join.access_code.empty()) return 2;
    if (!join.game_instance_id.empty()) return 3;
    if (!join.reserved_server_access_code.empty()) return 8;
    return 0;
}

std::shared_ptr<StartGameParams> build_desktop_start_game_params(
    std::shared_ptr<PlatformParams> platform_params, std::shared_ptr<DeviceParams> device_params,
    std::shared_ptr<SurfaceJava> surface, const DeepLinkJoinInfo& deep_link) {
    auto params = std::make_shared<StartGameParams>();
    params->accessCode_ = std::make_shared<FakeJni::JString>(deep_link.access_code);
    params->callId_ = std::make_shared<FakeJni::JString>(deep_link.call_id);
    params->conversationId_ = deep_link.conversation_id;
    params->deviceParams_ = std::move(device_params);
    params->eventId_ = std::make_shared<FakeJni::JString>(deep_link.event_id);
    params->gameId_ = std::make_shared<FakeJni::JString>(deep_link.game_instance_id);
    params->gameIdToExclude_ = std::make_shared<FakeJni::JString>(deep_link.game_id_to_exclude);
    params->gameJoinContext_ = std::make_shared<FakeJni::JString>(deep_link.game_join_context);
    params->isUnder13_ = NativeUserJavaInterfaceJava::getIsUnder13();
    params->isoContext_ = std::make_shared<FakeJni::JString>(deep_link.iso_context);
    params->joinAttemptId_ = std::make_shared<FakeJni::JString>(deep_link.join_attempt_id);
    params->joinAttemptOrigin_ = std::make_shared<FakeJni::JString>(deep_link.join_attempt_origin);
    params->joinRequestType_ = join_request_type_for(deep_link);
    params->launchData_ = std::make_shared<FakeJni::JString>(deep_link.launch_data);
    params->linkCode_ = std::make_shared<FakeJni::JString>(deep_link.link_code);
    params->placeId_ = deep_link.place_id;
    params->platformParams_ = std::move(platform_params);
    params->referralPage_ = std::make_shared<FakeJni::JString>(deep_link.referral_page);
    params->referredByPlayerId_ = deep_link.referred_by_player_id;
    params->reservedServerAccessCode_ =
        std::make_shared<FakeJni::JString>(deep_link.reserved_server_access_code);
    params->surface_ = surface;
    // userId is the user being FOLLOWED, as the app sets it from the
    // launch request (0 for any other join). It used to carry the
    // signed-in account's id, which on a follow join named the wrong user
    // and on any other join asked to follow oneself. The signed-in
    // account goes in username, as the app fills it from its session;
    // Process A fetched it (users.roblox.com/v1/users/authenticated) and
    // set_native_user_identity() holds it.
    params->userId_ = deep_link.user_id;
    params->username_ = std::make_shared<FakeJni::JString>(native_username());
    params->vrContext_ = std::make_shared<ActivityJava>();
    return params;
}

}  // namespace stud::jni_bridge
