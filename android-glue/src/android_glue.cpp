#include "stud/android_glue.h"
#include "stud/media_ndk_types.h"
#include "stud/ndk_types.h"

#include <unordered_map>
#include <unordered_set>

namespace stud::android_glue {

namespace {

const std::unordered_map<std::string_view, void*>& implemented() {
    static const std::unordered_map<std::string_view, void*> table = {
        {"AAssetManager_fromJava", reinterpret_cast<void*>(&AAssetManager_fromJava)},
        {"AAssetManager_open", reinterpret_cast<void*>(&AAssetManager_open)},
        {"AAsset_close", reinterpret_cast<void*>(&AAsset_close)},
        {"AAsset_getBuffer", reinterpret_cast<void*>(&AAsset_getBuffer)},
        {"AAsset_getLength", reinterpret_cast<void*>(&AAsset_getLength)},
        {"AAsset_openFileDescriptor", reinterpret_cast<void*>(&AAsset_openFileDescriptor)},

        {"AConfiguration_new", reinterpret_cast<void*>(&AConfiguration_new)},
        {"AConfiguration_delete", reinterpret_cast<void*>(&AConfiguration_delete)},
        {"AConfiguration_fromAssetManager", reinterpret_cast<void*>(&AConfiguration_fromAssetManager)},
        {"AConfiguration_getCountry", reinterpret_cast<void*>(&AConfiguration_getCountry)},
        {"AConfiguration_getLanguage", reinterpret_cast<void*>(&AConfiguration_getLanguage)},
        {"AConfiguration_getScreenSize", reinterpret_cast<void*>(&AConfiguration_getScreenSize)},
        {"AConfiguration_getScreenWidthDp", reinterpret_cast<void*>(&AConfiguration_getScreenWidthDp)},
        {"AConfiguration_getScreenHeightDp", reinterpret_cast<void*>(&AConfiguration_getScreenHeightDp)},
        {"AConfiguration_getNavHidden", reinterpret_cast<void*>(&AConfiguration_getNavHidden)},

        {"ALooper_forThread", reinterpret_cast<void*>(&ALooper_forThread)},
        {"ALooper_prepare", reinterpret_cast<void*>(&ALooper_prepare)},
        {"ALooper_acquire", reinterpret_cast<void*>(&ALooper_acquire)},
        {"ALooper_release", reinterpret_cast<void*>(&ALooper_release)},
        {"ALooper_pollOnce", reinterpret_cast<void*>(&ALooper_pollOnce)},
        {"ALooper_addFd", reinterpret_cast<void*>(&ALooper_addFd)},
        {"ALooper_removeFd", reinterpret_cast<void*>(&ALooper_removeFd)},

        {"ANativeWindow_fromSurface", reinterpret_cast<void*>(&ANativeWindow_fromSurface)},
        {"ANativeWindow_getWidth", reinterpret_cast<void*>(&ANativeWindow_getWidth)},
        {"ANativeWindow_getHeight", reinterpret_cast<void*>(&ANativeWindow_getHeight)},
        {"ANativeWindow_acquire", reinterpret_cast<void*>(&ANativeWindow_acquire)},
        {"ANativeWindow_release", reinterpret_cast<void*>(&ANativeWindow_release)},

        // AMediaCodec is a deliberate stub (real video decode is out of
        // scope for the prototype); AMediaFormat is a real implementation.
        // See media_codec.cpp.
        {"AMediaCodec_createDecoderByType", reinterpret_cast<void*>(&AMediaCodec_createDecoderByType)},
        {"AMediaCodec_createEncoderByType", reinterpret_cast<void*>(&AMediaCodec_createEncoderByType)},
        {"AMediaCodec_delete", reinterpret_cast<void*>(&AMediaCodec_delete)},
        {"AMediaCodec_configure", reinterpret_cast<void*>(&AMediaCodec_configure)},
        {"AMediaCodec_start", reinterpret_cast<void*>(&AMediaCodec_start)},
        {"AMediaCodec_stop", reinterpret_cast<void*>(&AMediaCodec_stop)},
        {"AMediaCodec_flush", reinterpret_cast<void*>(&AMediaCodec_flush)},
        {"AMediaCodec_dequeueInputBuffer", reinterpret_cast<void*>(&AMediaCodec_dequeueInputBuffer)},
        {"AMediaCodec_getInputBuffer", reinterpret_cast<void*>(&AMediaCodec_getInputBuffer)},
        {"AMediaCodec_getOutputBuffer", reinterpret_cast<void*>(&AMediaCodec_getOutputBuffer)},
        {"AMediaCodec_queueInputBuffer", reinterpret_cast<void*>(&AMediaCodec_queueInputBuffer)},
        {"AMediaCodec_dequeueOutputBuffer", reinterpret_cast<void*>(&AMediaCodec_dequeueOutputBuffer)},
        {"AMediaCodec_releaseOutputBuffer", reinterpret_cast<void*>(&AMediaCodec_releaseOutputBuffer)},
        {"AMediaCodec_getOutputFormat", reinterpret_cast<void*>(&AMediaCodec_getOutputFormat)},

        {"AMEDIAFORMAT_KEY_MIME", reinterpret_cast<void*>(&AMEDIAFORMAT_KEY_MIME)},
        {"AMEDIAFORMAT_KEY_WIDTH", reinterpret_cast<void*>(&AMEDIAFORMAT_KEY_WIDTH)},
        {"AMEDIAFORMAT_KEY_HEIGHT", reinterpret_cast<void*>(&AMEDIAFORMAT_KEY_HEIGHT)},
        {"AMEDIAFORMAT_KEY_BIT_RATE", reinterpret_cast<void*>(&AMEDIAFORMAT_KEY_BIT_RATE)},
        {"AMEDIAFORMAT_KEY_CHANNEL_COUNT", reinterpret_cast<void*>(&AMEDIAFORMAT_KEY_CHANNEL_COUNT)},
        {"AMEDIAFORMAT_KEY_COLOR_FORMAT", reinterpret_cast<void*>(&AMEDIAFORMAT_KEY_COLOR_FORMAT)},
        {"AMEDIAFORMAT_KEY_FRAME_RATE", reinterpret_cast<void*>(&AMEDIAFORMAT_KEY_FRAME_RATE)},
        {"AMEDIAFORMAT_KEY_I_FRAME_INTERVAL", reinterpret_cast<void*>(&AMEDIAFORMAT_KEY_I_FRAME_INTERVAL)},
        {"AMEDIAFORMAT_KEY_SAMPLE_RATE", reinterpret_cast<void*>(&AMEDIAFORMAT_KEY_SAMPLE_RATE)},
        {"AMEDIAFORMAT_KEY_STRIDE", reinterpret_cast<void*>(&AMEDIAFORMAT_KEY_STRIDE)},

        {"AMediaFormat_new", reinterpret_cast<void*>(&AMediaFormat_new)},
        {"AMediaFormat_delete", reinterpret_cast<void*>(&AMediaFormat_delete)},
        {"AMediaFormat_toString", reinterpret_cast<void*>(&AMediaFormat_toString)},
        {"AMediaFormat_getInt32", reinterpret_cast<void*>(&AMediaFormat_getInt32)},
        {"AMediaFormat_setInt32", reinterpret_cast<void*>(&AMediaFormat_setInt32)},
        {"AMediaFormat_setFloat", reinterpret_cast<void*>(&AMediaFormat_setFloat)},
        {"AMediaFormat_setString", reinterpret_cast<void*>(&AMediaFormat_setString)},
        {"AMediaFormat_getBuffer", reinterpret_cast<void*>(&AMediaFormat_getBuffer)},
        {"AMediaFormat_setBuffer", reinterpret_cast<void*>(&AMediaFormat_setBuffer)},
    };
    return table;
}

const std::unordered_set<std::string_view>& data_symbols() {
    static const std::unordered_set<std::string_view> names = {
        "AMEDIAFORMAT_KEY_MIME",       "AMEDIAFORMAT_KEY_WIDTH",
        "AMEDIAFORMAT_KEY_HEIGHT",     "AMEDIAFORMAT_KEY_BIT_RATE",
        "AMEDIAFORMAT_KEY_CHANNEL_COUNT", "AMEDIAFORMAT_KEY_COLOR_FORMAT",
        "AMEDIAFORMAT_KEY_FRAME_RATE", "AMEDIAFORMAT_KEY_I_FRAME_INTERVAL",
        "AMEDIAFORMAT_KEY_SAMPLE_RATE", "AMEDIAFORMAT_KEY_STRIDE",
    };
    return names;
}

}  // namespace

bool is_data_symbol(std::string_view name) { return data_symbols().count(name) != 0; }

void* resolve(std::string_view name) {
    if (auto it = implemented().find(name); it != implemented().end()) {
        return it->second;
    }
    return nullptr;
}

}  // namespace stud::android_glue
