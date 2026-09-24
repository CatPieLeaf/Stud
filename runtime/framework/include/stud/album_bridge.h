#pragma once

// Screenshots and screen recordings, out of the engine and into the
// user's own Pictures and Videos folders.
//
// The engine writes a capture into its own storage and calls
// NativeHelper.gameActivity_onScreenshotReady(path). On Android the Java
// side copies that file into the gallery (MediaStore: Pictures for images,
// Movies for video) and then tells the engine how it went through
// NativeGLInterface.nativeImageSavedToAlbumFinished(name, ok, error).
// That second call is the part that matters: the engine's capture flow
// waits for it, and nothing in Stud ever made it.

#include <fake-jni/fake-jni.h>

#include <string>

#include "stud/linker.h"

namespace stud::jni_bridge {

struct AlbumFolders {
    std::string pictures;  // images
    std::string videos;    // video/*
};

// Copies `path` into the folder its type belongs in, on a thread of its
// own (the file can be a long recording), then reports the result to the
// engine. Never overwrites: a name that is taken gets " (2)", " (3)"...
void save_capture_to_album(FakeJni::Jvm& jvm, const stud::linker::LoadedLibrary& lib,
                           const AlbumFolders& folders, const std::string& path);

}  // namespace stud::jni_bridge
