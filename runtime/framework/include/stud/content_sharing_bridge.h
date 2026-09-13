#pragma once

#include <fake-jni/fake-jni.h>

#include <functional>
#include <string>

#include "stud/linker.h"

// "Copy link" inside an experience.
//
// The invite dialog's Copy button does not touch the clipboard itself:
// the engine publishes `ExternalContentSharing.setClipboardText` with
// the text, and the platform is expected to put it on the system
// clipboard (the real app subscribes with a `ClipboardManager`, see its
// own ContentSharingProtocol). Stud subscribed to nothing, so pressing
// Copy did nothing at all and said nothing either.
//
// The topic and the payload key are literals here, deliberately and
// unusually for this codebase: this protocol exports getters for its
// share ids but NONE for this one, and the real app hardcodes the same
// string. There is nothing to read them from.
namespace stud::jni_bridge {

// `on_text` is handed whatever the engine wants on the clipboard.
bool run_content_sharing_bridge(FakeJni::Jvm& jvm, const stud::linker::LoadedLibrary& lib,
                                std::function<void(const std::string& text)> on_text);

}  // namespace stud::jni_bridge
