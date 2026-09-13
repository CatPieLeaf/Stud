#pragma once

#include <fake-jni/fake-jni.h>

#include <string>
#include <unordered_map>
#include <variant>

#include "stud/linker.h"

// FFlag overrides: the mechanism behind Stud's "respect FFlags, unlike
// Sober" locked decision (see the engineering notes) -- this is also how
// graphics-API selection is actually controlled. Roblox's engine gates
// Vulkan with its own internal device blacklist and two real FFlags,
// confirmed present as strings in libroblox.so:
// `DebugGraphicsDisableVulkan` and `DebugGraphicsPreferVulkan` (plus
// `GraphicsMode`/`GraphicsQualityLevel` for the broader quality/backend
// choice) -- none of this lives in PlatformParams/DeviceParams/InitParams
// (confirmed by re-checking their own code: zero graphics-related
// fields in either). Whatever Stud does for graphics-mode control has to
// go through this flag system, not the params objects.
//
// The injection point is `MainGameActivity`'s native method
// `nativePreloadFlagOverrides(String)`, called before engine init. This
// class builds that payload from Stud's user-editable JSON config file
// (per the locked decision: FFlag overrides live *only* in the raw JSON
// file, never the Qt settings UI).
//
// Wire format is grounded in the widely-documented public convention used
// across the Roblox FFlag/client-modding community for years (a flat JSON
// object, flag name -> value, values serialized as JSON strings
// regardless of the flag's actual bool/int/string type), not invented
// from nothing. `nativePreloadFlagOverrides` has since been called many
// times against the real, patched libroblox.so with no crash (see
// the engineering notes) -- confirms the call itself is well-formed, but
// whether Roblox's own flag-parsing logic actually reads/applies the
// overridden values correctly (not just tolerates receiving the string)
// remains unconfirmed.
//
// See the engineering notes, milestone M4.

namespace stud::jni_bridge {

using FlagValue = std::variant<bool, int, std::string>;

class FlagOverrides {
public:
    void set(const std::string& name, FlagValue value);
    bool has(const std::string& name) const;
    size_t size() const;

    // Loads overrides from a Stud config JSON file: flat
    // {"FlagName": <bool|int|string>, ...}. Throws std::runtime_error on a
    // malformed file (missing file is not an error -- returns empty
    // overrides, since having no override file is the common case).
    static FlagOverrides load_from_file(const std::string& path);

    // The payload for nativePreloadFlagOverrides -- see wire-format note
    // above.
    std::string to_wire_format() const;

    // Read-only view, so a caller can ask the engine about each one.
    const std::unordered_map<std::string, FlagValue>& entries() const { return overrides_; }

private:
    std::unordered_map<std::string, FlagValue> overrides_;
};

// Asks the engine, through its own exported getters, what value each
// override actually has, and prints the answer next to what was asked
// for. This project spent a long time treating "the call did not trap" as
// evidence an override applied; it never was. `when` labels the moment,
// since a flag can be right at one point in boot and replaced later.
void report_flag_override_state(FakeJni::Jvm& jvm, const stud::linker::LoadedLibrary& lib,
                                const FlagOverrides& overrides, const char* when);

}  // namespace stud::jni_bridge
