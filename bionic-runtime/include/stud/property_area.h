#pragma once

#include <string>
#include <utility>
#include <vector>

// Real Android bionic system-property area, the legacy single-file
// ("ContextsPreSplit") format -- see the engineering notes' own "no real DNS"
// section for the real investigation that motivated this (a real syscall trace
// capture showed bionic's DNS resolver falling back to a broken 0.0.0.0
// nameserver because __system_property_get("net.dns1", ...) has nothing
// real to read: no property service exists in Process B's sandbox at
// all).
//
// Real, ground-truth-confirmed mechanism (this project's own static analysis
// reading of the actual extracted third_party/android-bionic/libc.so,
// not assumed from a public AOSP mirror -- confirmed the mirror's struct
// layout AND magic/version constants are byte-for-byte correct for this
// specific binary): __system_properties_init() calls
// SystemProperties::Init(..., "/dev/__properties__"), which picks a real
// mode based on whether that path is a directory:
//   - a directory containing a real PROP_TREE_FILE -> ContextsSerialized
//     (modern, SELinux-context-split, this binary's own real strings
//     confirm it supports this -- "/dev/__properties__/property_info",
//     "plat_property_contexts")
//   - a directory without one -> ContextsSplit
//   - NOT a directory at all -> ContextsPreSplit (the real legacy mode:
///    one flat prop_area file, no SELinux-context routing needed)
// Process B's sandbox shares the host's real /dev (--dev-bind /dev /dev)
// and the host (a real, non-Android Linux desktop) has no
// /dev/__properties__ at all -- so bind-mounting a single real, regular
// file at that exact path is what real bionic already, naturally falls
// back to using, with zero extra tricks needed to force ContextsPreSplit.
//
// Real struct layout (confirmed in the engine against the field writes in
// prop_area::map_prop_area_rw, cross-checked against AOSP's own current
// prop_area.h/prop_info.h -- see the engineering notes for the full real reference):
//   prop_area header, 128 bytes: bytes_used_(4) serial_(4) magic_(4)
//     version_(4) reserved_[28*4=112]. magic_=0x504f5250,
//     version_=0xfc6ed0ab (confirmed live: written together as one real
//     8-byte immediate, 0xfc6ed0ab504f5250, in map_prop_area_rw).
//   data_ starts immediately after (offset 128) -- the trie root implicit
//   at data_+0.
//   prop_trie_node, 20 bytes + name: namelen(4) prop(4) left(4) right(4)
//     children(4), then the name bytes (NOT null-terminated in the file;
//     namelen is authoritative), 4-byte aligned after.
//   prop_info, 96 bytes total (real, AOSP-enforced size): serial(4)
//     value[92], then name bytes, 4-byte aligned after. Values longer
//     than 91 real chars need the "long property" union variant --
//     genuinely not needed for what Stud uses this for (DNS server
//     addresses), so not implemented here; write_property_area() clamps
//     to 91 chars rather than silently corrupting the format.
// All prop/left/right/children/offset fields are OFFSETS from data_,
// not absolute file offsets or pointers -- 0 means "absent/null".
namespace stud::bionic_runtime {

// Writes a real, valid, minimal bionic legacy prop_area file to
// `output_path` (creating/truncating it), containing exactly the given
// name -> value properties (dotted names get a real, correct multi-level
// trie, matching how bionic's own resolver actually looks them up).
// Returns false (and leaves no file, or removes a partial one) on any
// real I/O failure. Real, honest constraint: `properties` must be
// non-empty and each value must be <= 91 bytes (the legacy short-value
// limit) -- longer values are rejected outright rather than silently
// truncated or corrupted, matching this project's own "honest failure,
// not silent wrong data" standard.
bool write_property_area(const std::string& output_path,
                          const std::vector<std::pair<std::string, std::string>>& properties);

}  // namespace stud::bionic_runtime
