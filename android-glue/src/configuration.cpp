#include "stud/ndk_types.h"

#include "stud/system_locale.h"

#include <dlfcn.h>

#include <algorithm>

// AConfiguration is populated with Stud's desktop-spoof defaults,
// deliberately consistent with PlatformParams/DeviceParams (jni-bridge)
// rather than an independent set of spoof values: same "desktop, no touch,
// no nav bar" story, told once, not twice.
struct AConfiguration {
    // The real system language, not a hardcoded "en". A real device
    // populates this from its own configuration; this is the desktop's
    // equivalent. Both fields are exactly two characters and are NOT
    // NUL-terminated. That is the NDK's own contract for
    // AConfiguration_getLanguage/getCountry, whose callers pass a
    // two-byte buffer. An unknown country stays empty, as it does on a
    // device with no region set.
    char country[2] = {'\0', '\0'};
    char language[2] = {'e', 'n'};

    AConfiguration() {
        const auto& locale = stud::android_glue::system_locale();
        if (locale.language.size() >= 2) {
            language[0] = locale.language[0];
            language[1] = locale.language[1];
        }
        if (locale.country.size() >= 2) {
            country[0] = locale.country[0];
            country[1] = locale.country[1];
        }
    }

    // Filled from the real window by fill_from_window(). These were a
    // fixed 1920x1080 and LARGE whatever the window or monitor was; 0 is
    // Android's own "undefined" (ACONFIGURATION_SCREEN_WIDTH_DP_ANY) for
    // the time before a window exists.
    int32_t screenWidthDp = 0;
    int32_t screenHeightDp = 0;
    int32_t navHidden = ACONFIGURATION_NAVHIDDEN_YES;
    int32_t screenSize = ACONFIGURATION_SCREENSIZE_ANY;
};

namespace {

// dp = pixels / density, with the density the engine lays out with
// (stud_engine_layout_density, exported by Process B's runtime), and the
// size class from Android's own thresholds on the smaller and larger
// dimension. Where either number is not available, the fields stay
// undefined rather than guessed.
void fill_from_window(AConfiguration* config) {
    using DensityFn = float (*)();
    static const auto density_fn =
        reinterpret_cast<DensityFn>(::dlsym(RTLD_DEFAULT, "stud_engine_layout_density"));
    const float density = density_fn != nullptr ? density_fn() : 0.0f;
    const int32_t width = ANativeWindow_getWidth(nullptr);
    const int32_t height = ANativeWindow_getHeight(nullptr);
    if (density <= 0.0f || width <= 0 || height <= 0) return;
    config->screenWidthDp = static_cast<int32_t>(static_cast<float>(width) / density);
    config->screenHeightDp = static_cast<int32_t>(static_cast<float>(height) / density);
    const int32_t small = std::min(config->screenWidthDp, config->screenHeightDp);
    const int32_t large = std::max(config->screenWidthDp, config->screenHeightDp);
    if (small >= 720 && large >= 960) {
        config->screenSize = ACONFIGURATION_SCREENSIZE_XLARGE;
    } else if (small >= 480 && large >= 640) {
        config->screenSize = ACONFIGURATION_SCREENSIZE_LARGE;
    } else if (small >= 320 && large >= 470) {
        config->screenSize = ACONFIGURATION_SCREENSIZE_NORMAL;
    } else {
        config->screenSize = ACONFIGURATION_SCREENSIZE_SMALL;
    }
}

}  // namespace

extern "C" {

AConfiguration* AConfiguration_new() { return new AConfiguration(); }

void AConfiguration_delete(AConfiguration* config) { delete config; }

void AConfiguration_fromAssetManager(AConfiguration* out, AAssetManager* /*am*/) {
    // Real Android fills this from the device's current configuration.
    // Stud's is the window's: the locale is set at construction, and the
    // screen is measured here, each time the engine asks.
    if (out != nullptr) fill_from_window(out);
}

void AConfiguration_getCountry(AConfiguration* config, char* outCountry) {
    outCountry[0] = config->country[0];
    outCountry[1] = config->country[1];
}

void AConfiguration_getLanguage(AConfiguration* config, char* outLanguage) {
    outLanguage[0] = config->language[0];
    outLanguage[1] = config->language[1];
}

int32_t AConfiguration_getScreenSize(AConfiguration* config) { return config->screenSize; }

int32_t AConfiguration_getScreenWidthDp(AConfiguration* config) { return config->screenWidthDp; }

int32_t AConfiguration_getScreenHeightDp(AConfiguration* config) { return config->screenHeightDp; }

int32_t AConfiguration_getNavHidden(AConfiguration* config) { return config->navHidden; }

}  // extern "C"
