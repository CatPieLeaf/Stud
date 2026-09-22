#pragma once

#include "stud/bionic_jvm.h"
#include "stud/linker.h"

namespace stud::jni_bridge {

// Android's runtime permissions, answered for a machine that has no such
// thing.
//
// The engine asks the platform, over the MessageBus, whether it holds a
// permission before it uses the hardware behind it, and voice chat is
// the case that matters: `RBX::Voice::RobloxAudioDevice` calls
// `CheckMicrophonePermissionAsync` / `RequestMicrophonePermissionAsync`,
// both of which end in `PermissionsProtocolCore`, and with nothing
// answering they resolve as ACCESS_DENIED. The engine's own log says so
// in those words. So voice chat could never start in Stud, whatever the
// audio path did.
//
// Five real request handlers, read from the app's own permissions
// protocol implementation: PermissionsRequest, HasPermissions,
// SupportsPermissions, ShouldShowPermissionUpsell and
// ShouldShowRequestPermissionRationale. Unlike every other protocol Stud
// implements, this one has NO exported id getters, the app registers
// them with plain string literals, so the literals here come from that
// decompile and are the only source there is. They are checked at
// startup the only way they can be: a handler that never fires says so.
//
// What Stud answers, and why it is honest rather than convenient:
//
//   MICROPHONE_ACCESS , authorized. A desktop grants microphone access
//                         to a running program without asking, and
//                         render-host really does open a capture stream
//                         on the host. Whether a microphone exists
//                         is a different question, answered by the device
//                         itself when the stream opens.
//   everything else   , not supported, and therefore missing. Stud has
//                         no camera, no contacts, no media store and no
//                         notification permission model. Claiming those
//                         would make the engine offer features that
//                         cannot work.
bool run_permissions_bridge(FakeJni::Jvm& jvm, const stud::linker::LoadedLibrary& lib);

}  // namespace stud::jni_bridge
