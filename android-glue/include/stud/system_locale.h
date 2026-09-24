#pragma once

// The language the user actually runs their desktop in.
//
// Stud used to answer every locale question with English: the Roblox
// locale interface returned a literal "en_us", the Java Locale class
// returned "en"/"US", and AConfiguration defaulted to "en". So the app
// came up in English on a machine running in any other language, with
// nothing the user could do about it.
//
// A real Android device answers this from its own system settings. A
// desktop's equivalent is the locale environment, which is what this
// reads.
//
// Header-only so the mapping can be tested directly (tests/locale_test.cpp)
// rather than only by launching the app in a different language.

#include <cstdlib>
#include <string>
#include <vector>

namespace stud::android_glue {

struct SystemLocale {
    std::string language;   // "pt", ISO 639, lower case
    std::string country;    // "BR", ISO 3166, upper case; may be empty
    // java.util.Locale.toString(): "pt_BR", or just "pt" with no country.
    // This is exactly what the real implementation of
    // NativeLocaleJavaInterface.getLocale() returns
    // (Configuration.getLocales().get(0).toString()).
    std::string java_tag;
    // The Roblox locale id: "pt_br". Roblox supports a fixed set and
    // falls back to English for anything else, so this is never a locale
    // the app does not know.
    std::string roblox;
};

namespace locale_detail {

// Roblox's own supported-locale table, transcribed from the real app's
// own code rather than guessed. Each entry is the locale id the app uses
// and the key it matches an Android locale against.
struct RobloxLocale {
    const char* id;     // what getRobloxLocale() answers
    const char* match;  // compared against "<language>_<COUNTRY>" and "<language>"
};

inline const std::vector<RobloxLocale>& roblox_locales() {
    // Order matters only in that the first match wins, which is the real
    // implementation's behaviour too (it walks the list).
    //
    // The real table has one conditional entry: Simplified Chinese is
    // "zh_cjv" on some builds and "zh_cn" otherwise, chosen by a flag
    // Stud has no equivalent of. "zh_cn" is the ordinary branch.
    static const std::vector<RobloxLocale> kLocales = {
        {"en_us", "en"},   {"es_es", "es"},      {"fr_fr", "fr"},
        {"it_it", "it"},   {"de_de", "de"},      {"id_id", "id"},
        {"ja_jp", "ja"},   {"ko_kr", "ko"},      {"pt_br", "pt"},
        {"ru_ru", "ru"},   {"th_th", "th"},      {"tr_tr", "tr"},
        {"vi_vn", "vi"},   {"zh_tw", "zh_TW"},   {"zh_cn", "zh_CN"},
    };
    return kLocales;
}

// "pt_BR.UTF-8@euro" -> language "pt", country "BR". Also accepts the
// BCP 47 spelling with a hyphen, since a hand-set override may use it.
inline void split_locale(const std::string& raw, std::string* language, std::string* country) {
    language->clear();
    country->clear();
    // Everything from a codeset or modifier marker on is not part of the
    // language or the country.
    std::string value = raw.substr(0, raw.find_first_of(".@"));
    const std::size_t sep = value.find_first_of("_-");
    if (sep == std::string::npos) {
        *language = value;
    } else {
        *language = value.substr(0, sep);
        *country = value.substr(sep + 1);
    }
    for (char& c : *language) {
        if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
    }
    for (char& c : *country) {
        if (c >= 'a' && c <= 'z') c = static_cast<char>(c - 'a' + 'A');
    }
}

inline const char* env_or_null(const char* name) {
    const char* value = std::getenv(name);
    return (value != nullptr && *value != '\0') ? value : nullptr;
}

// Which locale string the environment actually names, following the rule
// gettext documents and every desktop toolkit implements: the POSIX
// chain decides, except that an explicitly-set LANGUAGE takes precedence
// over it, unless the POSIX chain says "C" or "POSIX", which means
// "no localisation" and where LANGUAGE is deliberately ignored.
inline std::string locale_from_environment() {
    // Stud's own override comes first, so a language can be tried
    // without changing the whole desktop's.
    if (const char* forced = env_or_null("STUD_LOCALE")) return forced;

    const char* base = env_or_null("LC_ALL");
    if (base == nullptr) base = env_or_null("LC_MESSAGES");
    if (base == nullptr) base = env_or_null("LANG");
    if (base == nullptr) return "";

    const std::string posix = base;
    if (posix == "C" || posix == "POSIX" || posix == "C.UTF-8") return "";

    if (const char* language_list = env_or_null("LANGUAGE")) {
        // A colon-separated list in preference order; the first entry is
        // the one the user most wants.
        const std::string list = language_list;
        const std::size_t end = list.find(':');
        const std::string first = end == std::string::npos ? list : list.substr(0, end);
        if (!first.empty()) return first;
    }
    return posix;
}

}  // namespace locale_detail

// The Roblox locale id for a language and country, falling back to
// English exactly as the real app does for anything unsupported.
//
// The matching rule is the real one: an entry matches when its key equals
// the whole "<language>_<COUNTRY>" or just the language part. That is what
// makes "pt_BR" and "pt_PT" both Brazilian Portuguese (the only
// Portuguese Roblox ships) while "zh_TW" and "zh_CN" stay distinct, since
// their keys carry the country.
inline std::string roblox_locale_for(const std::string& language, const std::string& country) {
    if (language.empty()) return "en_us";
    const std::string full = country.empty() ? language : language + "_" + country;
    for (const auto& entry : locale_detail::roblox_locales()) {
        if (full == entry.match || language == entry.match) return entry.id;
    }
    return "en_us";
}

// Reads the environment once and answers every locale question from it.
inline const SystemLocale& system_locale() {
    static const SystemLocale kLocale = [] {
        SystemLocale out;
        locale_detail::split_locale(locale_detail::locale_from_environment(), &out.language,
                                    &out.country);
        if (out.language.empty()) {
            // Nothing set, or a deliberately unlocalised environment.
            // English is what the app would have fallen back to anyway.
            out.language = "en";
            out.country = "US";
        }
        out.java_tag = out.country.empty() ? out.language : out.language + "_" + out.country;
        out.roblox = roblox_locale_for(out.language, out.country);
        return out;
    }();
    return kLocale;
}

}  // namespace stud::android_glue
