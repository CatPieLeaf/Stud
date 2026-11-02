// AOSP's property_get: answers with the default. Stud has no Android
// system properties for gfxstream to read.
#pragma once

#include <cstring>

#define PROPERTY_VALUE_MAX 92

static inline int property_get(const char* /*key*/, char* value, const char* default_value) {
    if (default_value == nullptr) {
        value[0] = '\0';
        return 0;
    }
    std::strncpy(value, default_value, PROPERTY_VALUE_MAX - 1);
    value[PROPERTY_VALUE_MAX - 1] = '\0';
    return static_cast<int>(std::strlen(value));
}
