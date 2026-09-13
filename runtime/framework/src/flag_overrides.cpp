#include "stud/flag_overrides.h"

#include "stud/trap_recovery.h"

#include <nlohmann/json.hpp>

#include <cstdio>
#include <fstream>
#include <stdexcept>

namespace stud::jni_bridge {

void FlagOverrides::set(const std::string& name, FlagValue value) { overrides_[name] = std::move(value); }

bool FlagOverrides::has(const std::string& name) const { return overrides_.count(name) != 0; }

size_t FlagOverrides::size() const { return overrides_.size(); }

FlagOverrides FlagOverrides::load_from_file(const std::string& path) {
    FlagOverrides result;

    std::ifstream file(path);
    if (!file.is_open()) {
        return result;
    }

    nlohmann::json doc;
    try {
        file >> doc;
    } catch (const nlohmann::json::parse_error& e) {
        throw std::runtime_error("stud: malformed FFlag override file '" + path + "': " + e.what());
    }

    if (!doc.is_object()) {
        throw std::runtime_error("stud: FFlag override file '" + path +
                                  "' must be a flat JSON object of flag name to value");
    }

    for (auto& [name, value] : doc.items()) {
        if (value.is_boolean()) {
            result.set(name, value.get<bool>());
        } else if (value.is_number_integer()) {
            result.set(name, value.get<int>());
        } else if (value.is_string()) {
            result.set(name, value.get<std::string>());
        } else {
            throw std::runtime_error("stud: FFlag override '" + name + "' in '" + path +
                                      "' has an unsupported value type (must be bool, int, or string)");
        }
    }

    return result;
}

std::string FlagOverrides::to_wire_format() const {
    nlohmann::json doc = nlohmann::json::object();
    for (const auto& [name, value] : overrides_) {
        std::visit(
            [&](auto&& v) {
                using T = std::decay_t<decltype(v)>;
                if constexpr (std::is_same_v<T, bool>) {
                    doc[name] = v ? "True" : "False";
                } else if constexpr (std::is_same_v<T, int>) {
                    doc[name] = std::to_string(v);
                } else {
                    doc[name] = v;
                }
            },
            value);
    }
    return doc.dump();
}

namespace {
using GetFFlagFn = jboolean (*)(JNIEnv*, jclass, jstring, jboolean);
using GetFIntFn = jint (*)(JNIEnv*, jclass, jstring, jint);
using GetFStringFn = jstring (*)(JNIEnv*, jclass, jstring, jstring);

// Which name the engine answers to is not uniform: an FFlag is declared
// bare (FFlagX in settings is variable X), while a log level keeps its
// prefix (FLogNetwork really is FLogNetwork). So both spellings are
// tried and whichever the engine actually knows is reported.
std::string bare_name(const std::string& name) {
    static const char* const kPrefixes[] = {"DFFlag", "FFlag", "DFInt", "FInt",
                                            "DFString", "FString", "SFFlag"};
    for (const char* prefix : kPrefixes) {
        const size_t n = std::char_traits<char>::length(prefix);
        if (name.size() > n && name.compare(0, n, prefix) == 0) return name.substr(n);
    }
    return name;
}
}  // namespace

void report_flag_override_state(FakeJni::Jvm& jvm, const stud::linker::LoadedLibrary& lib,
                                const FlagOverrides& overrides, const char* when) {
    if (overrides.size() == 0) return;
    auto* get_flag = reinterpret_cast<GetFFlagFn>(
        lib.find_symbol("Java_com_roblox_client_flags_FlagJniInterface_nativeGetFFlag"));
    auto* get_int = reinterpret_cast<GetFIntFn>(
        lib.find_symbol("Java_com_roblox_client_flags_FlagJniInterface_nativeGetFInt"));
    auto* get_string = reinterpret_cast<GetFStringFn>(
        lib.find_symbol("Java_com_roblox_client_flags_FlagJniInterface_nativeGetFString"));
    if (get_flag == nullptr && get_int == nullptr && get_string == nullptr) return;

    FakeJni::LocalFrame frame(jvm);
    auto& env = frame.getJniEnv();
    auto* jni_env = static_cast<JNIEnv*>(&env);
    for (const auto& entry : overrides.entries()) {
        const std::string name = bare_name(entry.first);
        jstring jname = env.NewStringUTF(name.c_str());
        if (std::holds_alternative<bool>(entry.second) && get_flag != nullptr) {
            const bool wanted = std::get<bool>(entry.second);
            // Asked for with the OPPOSITE default, so the answer says
            // whether the engine holds a real value or just echoed it.
            jboolean got_a = 0, got_b = 0;
            call_trapping_abort_with_result(get_flag, got_a, jni_env, nullptr, jname,
                                            static_cast<jboolean>(JNI_FALSE));
            call_trapping_abort_with_result(get_flag, got_b, jni_env, nullptr, jname,
                                            static_cast<jboolean>(JNI_TRUE));
            const bool known = got_a == got_b;
            std::printf("stud: flag %s (%s): wanted=%s engine=%s%s\n", entry.first.c_str(), when,
                        wanted ? "true" : "false",
                        known ? (got_a ? "true" : "false") : "<unset>",
                        (known && (got_a != 0) == wanted) ? " OK" : " MISMATCH");
        } else if (std::holds_alternative<int>(entry.second) && get_int != nullptr) {
            const int wanted = std::get<int>(entry.second);
            constexpr jint kSentinel = -987654;
            jint got = kSentinel;
            call_trapping_abort_with_result(get_int, got, jni_env, nullptr, jname, kSentinel);
            if (got == kSentinel) {
                // Unknown under the bare name; a log level keeps its own.
                jstring full = env.NewStringUTF(entry.first.c_str());
                call_trapping_abort_with_result(get_int, got, jni_env, nullptr, full, kSentinel);
            }
            // "the engine does not know this flag" and "the engine
            // holds a different value" are different answers, and only
            // the second one is a mismatch. Log-level ints
            // (FLog*/DFLog*) are the known case of the first: they live
            // in a registry nativeGetFInt does not read, which is
            // documented rather than broken, and calling it MISMATCH
            // put a permanent false alarm in every launch's log.
            if (got == kSentinel) {
                std::printf("stud: flag %s (%s): wanted=%d, the engine does not expose this one\n",
                            entry.first.c_str(), when, wanted);
            } else {
                std::printf("stud: flag %s (%s): wanted=%d engine=%d%s\n", entry.first.c_str(),
                            when, wanted, static_cast<int>(got),
                            static_cast<int>(got) == wanted ? " OK" : " MISMATCH");
            }
        } else if (std::holds_alternative<std::string>(entry.second) && get_string != nullptr) {
            jstring got = nullptr;
            call_trapping_abort_with_result(get_string, got, jni_env, nullptr, jname,
                                            env.NewStringUTF(""));
            std::string value;
            if (got != nullptr) {
                auto resolved = env.resolveReference(got);
                if (auto as_string = std::dynamic_pointer_cast<FakeJni::JString>(resolved)) {
                    value = as_string->asStdString();
                }
            }
            std::printf("stud: flag %s (%s): wanted=\"%s\" engine=\"%s\"%s\n",
                        entry.first.c_str(), when, std::get<std::string>(entry.second).c_str(),
                        value.c_str(),
                        value == std::get<std::string>(entry.second) ? " OK" : " MISMATCH");
        }
    }
    std::fflush(stdout);
}

}  // namespace stud::jni_bridge
