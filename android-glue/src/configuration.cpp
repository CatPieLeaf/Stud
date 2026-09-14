#include "stud/ndk_types.h"

#include "stud/system_locale.h"

// AConfiguration is populated with Stud's desktop-spoof defaults --
// deliberately consistent with PlatformParams/DeviceParams (jni-bridge)
// rather than an independent set of spoof values: same "desktop, no touch,
// no nav bar" story, told once, not twice.
struct AConfiguration {
    // The real system language, not a hardcoded "en". A real device
    // populates this from its own configuration; this is the desktop's
    // equivalent. Both fields are exactly two characters and are NOT
    // NUL-terminated -- that is the NDK's own contract for
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

    int32_t screenWidthDp = 1920;
    int32_t screenHeightDp = 1080;
    int32_t navHidden = ACONFIGURATION_NAVHIDDEN_YES;
    int32_t screenSize = ACONFIGURATION_SCREENSIZE_LARGE;
};

extern "C" {

AConfiguration* AConfiguration_new() { return new AConfiguration(); }

void AConfiguration_delete(AConfiguration* config) { delete config; }

void AConfiguration_fromAssetManager(AConfiguration* out, AAssetManager* /*am*/) {
    // Real Android populates `out` from the AssetManager's compiled
    // resource table (the device's actual configuration). Stud has no
    // resource table -- `out` already holds sensible desktop defaults from
    // construction, so this is a no-op beyond that.
    (void)out;
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
