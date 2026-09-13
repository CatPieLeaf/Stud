#pragma once

#include <jni.h>

#include <cstdint>
#include <sys/types.h>

// Real Android NDK API declarations (asset_manager.h, asset_manager_jni.h,
// configuration.h, looper.h, native_window.h, native_window_jni.h) --
// signatures as documented/stable in the actual NDK, reproduced here
// because the real NDK headers aren't installed in this environment. Not
// copied from NDK source (which is Apache 2.0 and wouldn't need clean-room
// treatment anyway) -- these are standard, unchanged-for-years public API
// signatures, reconstructed from documented behavior.
//
// See the engineering notes, "M2 rendering architecture"/M4 sections for how
// these get used.

extern "C" {

// --- asset_manager.h / asset_manager_jni.h ---------------------------

struct AAssetManager;
struct AAsset;

enum {
    AASSET_MODE_UNKNOWN = 0,
    AASSET_MODE_RANDOM = 1,
    AASSET_MODE_STREAMING = 2,
    AASSET_MODE_BUFFER = 3,
};

AAssetManager* AAssetManager_fromJava(JNIEnv* env, jobject assetManager);
AAsset* AAssetManager_open(AAssetManager* mgr, const char* filename, int mode);
void AAsset_close(AAsset* asset);
const void* AAsset_getBuffer(AAsset* asset);
off_t AAsset_getLength(AAsset* asset);
int AAsset_openFileDescriptor(AAsset* asset, off_t* outStart, off_t* outLength);

// --- configuration.h ---------------------------------------------------

struct AConfiguration;

enum {
    ACONFIGURATION_SCREENSIZE_ANY = 0x00,
    ACONFIGURATION_SCREENSIZE_SMALL = 0x01,
    ACONFIGURATION_SCREENSIZE_NORMAL = 0x02,
    ACONFIGURATION_SCREENSIZE_LARGE = 0x03,
    ACONFIGURATION_SCREENSIZE_XLARGE = 0x04,
};

enum {
    ACONFIGURATION_NAVHIDDEN_ANY = 0x0000,
    ACONFIGURATION_NAVHIDDEN_NO = 0x0001,
    ACONFIGURATION_NAVHIDDEN_YES = 0x0002,
};

AConfiguration* AConfiguration_new();
void AConfiguration_delete(AConfiguration* config);
void AConfiguration_fromAssetManager(AConfiguration* out, AAssetManager* am);
void AConfiguration_getCountry(AConfiguration* config, char* outCountry);
void AConfiguration_getLanguage(AConfiguration* config, char* outLanguage);
int32_t AConfiguration_getScreenSize(AConfiguration* config);
int32_t AConfiguration_getScreenWidthDp(AConfiguration* config);
int32_t AConfiguration_getScreenHeightDp(AConfiguration* config);
int32_t AConfiguration_getNavHidden(AConfiguration* config);

// --- looper.h ------------------------------------------------------------

struct ALooper;

enum {
    ALOOPER_PREPARE_ALLOW_NON_CALLBACKS = 1 << 0,
};

enum {
    ALOOPER_POLL_WAKE = -1,
    ALOOPER_POLL_CALLBACK = -2,
    ALOOPER_POLL_TIMEOUT = -3,
    ALOOPER_POLL_ERROR = -4,
};

enum {
    ALOOPER_EVENT_INPUT = 1 << 0,
    ALOOPER_EVENT_OUTPUT = 1 << 1,
    ALOOPER_EVENT_ERROR = 1 << 2,
    ALOOPER_EVENT_HANGUP = 1 << 3,
    ALOOPER_EVENT_INVALID = 1 << 4,
};

using ALooper_callbackFunc = int (*)(int fd, int events, void* data);

ALooper* ALooper_forThread();
ALooper* ALooper_prepare(int opts);
void ALooper_acquire(ALooper* looper);
void ALooper_release(ALooper* looper);
int ALooper_pollOnce(int timeoutMillis, int* outFd, int* outEvents, void** outData);
int ALooper_addFd(ALooper* looper, int fd, int ident, int events, ALooper_callbackFunc callback,
                  void* data);
int ALooper_removeFd(ALooper* looper, int fd);

// --- native_window.h / native_window_jni.h (declared, not implemented --
// see NotYetSupported in android_glue.h) --------------------------------

struct ANativeWindow;

int32_t ANativeWindow_getWidth(ANativeWindow* window);
int32_t ANativeWindow_getHeight(ANativeWindow* window);
void ANativeWindow_acquire(ANativeWindow* window);
void ANativeWindow_release(ANativeWindow* window);
ANativeWindow* ANativeWindow_fromSurface(JNIEnv* env, jobject surface);

}  // extern "C"
