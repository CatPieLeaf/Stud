// libbase's property getters: every property answers with its default.
#pragma once

#include <string>

namespace android::base {

inline std::string GetProperty(const std::string& /*key*/, const std::string& default_value) {
    return default_value;
}

template <typename T>
inline T GetUintProperty(const std::string& /*key*/, T default_value,
                         T /*max*/ = static_cast<T>(-1)) {
    return default_value;
}

inline bool GetBoolProperty(const std::string& /*key*/, bool default_value) {
    return default_value;
}

}  // namespace android::base
