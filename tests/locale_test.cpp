// Which language Stud tells Roblox the user runs in.
//
// Every locale answer used to be a hardcoded English literal -- "en_us"
// from the Roblox locale interface, "en"/"US" from the Java Locale stub,
// "en" from AConfiguration -- so the app came up in English whatever the
// desktop was set to. The mapping below is transcribed from the real
// app's own supported-locale table, and these checks pin both halves of
// it: reading the environment, and turning that into a locale id Roblox
// actually ships.

#include "stud/system_locale.h"

#include <cstdio>
#include <cstdlib>
#include <string>

namespace {

int g_failures = 0;

void check(bool ok, const std::string& what) {
    if (!ok) {
        std::printf("FAIL: %s\n", what.c_str());
        ++g_failures;
    }
}

void check_eq(const std::string& got, const std::string& want, const std::string& what) {
    if (got != want) {
        std::printf("FAIL: %s (got \"%s\", wanted \"%s\")\n", what.c_str(), got.c_str(),
                    want.c_str());
        ++g_failures;
    }
}

void clear_locale_env() {
    ::unsetenv("STUD_LOCALE");
    ::unsetenv("LC_ALL");
    ::unsetenv("LC_MESSAGES");
    ::unsetenv("LANG");
    ::unsetenv("LANGUAGE");
}

std::string split_language(const std::string& raw) {
    std::string language, country;
    stud::android_glue::locale_detail::split_locale(raw, &language, &country);
    return language;
}

std::string split_country(const std::string& raw) {
    std::string language, country;
    stud::android_glue::locale_detail::split_locale(raw, &language, &country);
    return country;
}

}  // namespace

int main() {
    using stud::android_glue::roblox_locale_for;
    using stud::android_glue::locale_detail::locale_from_environment;

    // Parsing a POSIX locale string. The codeset and any modifier are not
    // part of the language or the country.
    check_eq(split_language("pt_BR.UTF-8"), "pt", "language out of a POSIX locale");
    check_eq(split_country("pt_BR.UTF-8"), "BR", "country out of a POSIX locale");
    check_eq(split_country("de_DE.UTF-8@euro"), "DE", "a modifier is not part of the country");
    check_eq(split_language("pt"), "pt", "a bare language is a language");
    check_eq(split_country("pt"), "", "a bare language has no country");
    // A hand-set override may well be typed the BCP 47 way.
    check_eq(split_language("pt-BR"), "pt", "a hyphen separates just as well");
    check_eq(split_country("pt-BR"), "BR", "a hyphen separates just as well (country)");
    // Case is normalised the way Java's own Locale does it.
    check_eq(split_language("PT_br"), "pt", "the language is lower case");
    check_eq(split_country("PT_br"), "BR", "the country is upper case");

    // The supported-locale table, and its real matching rule: an entry
    // matches the whole "<language>_<COUNTRY>" or just the language.
    check_eq(roblox_locale_for("pt", "BR"), "pt_br", "Brazilian Portuguese");
    // Roblox ships one Portuguese, so European Portuguese resolves to it
    // rather than falling back to English -- the language alone matches.
    check_eq(roblox_locale_for("pt", "PT"), "pt_br", "European Portuguese gets the one Portuguese");
    check_eq(roblox_locale_for("fr", "CA"), "fr_fr", "Canadian French gets the one French");
    check_eq(roblox_locale_for("de", "AT"), "de_de", "Austrian German gets the one German");
    check_eq(roblox_locale_for("en", "GB"), "en_us", "British English gets the one English");
    check_eq(roblox_locale_for("ja", "JP"), "ja_jp", "Japanese");
    check_eq(roblox_locale_for("ru", "RU"), "ru_ru", "Russian");

    // The two Chinese entries carry their country in the key, so they
    // stay distinct instead of collapsing into one another.
    check_eq(roblox_locale_for("zh", "TW"), "zh_tw", "Traditional Chinese");
    check_eq(roblox_locale_for("zh", "CN"), "zh_cn", "Simplified Chinese");
    // ...and a Chinese region Roblox has no entry for matches neither,
    // which is the real table's own behaviour, not a gap in this one.
    check_eq(roblox_locale_for("zh", "HK"), "en_us", "an unlisted Chinese region falls back");

    // Anything unsupported falls back to English rather than being sent
    // a locale id the app does not know.
    check_eq(roblox_locale_for("cy", "GB"), "en_us", "an unsupported language falls back");
    check_eq(roblox_locale_for("", ""), "en_us", "no language at all falls back");

    // Reading the environment.
    clear_locale_env();
    check_eq(locale_from_environment(), "", "an empty environment names no locale");

    ::setenv("LANG", "pt_BR.UTF-8", 1);
    check_eq(locale_from_environment(), "pt_BR.UTF-8", "LANG is used when nothing else is set");

    ::setenv("LC_MESSAGES", "fr_FR.UTF-8", 1);
    check_eq(locale_from_environment(), "fr_FR.UTF-8", "LC_MESSAGES beats LANG");

    ::setenv("LC_ALL", "de_DE.UTF-8", 1);
    check_eq(locale_from_environment(), "de_DE.UTF-8", "LC_ALL beats everything POSIX");

    // LANGUAGE is the list a desktop sets when the user picks a UI
    // language, and its first entry is what they most want.
    ::setenv("LANGUAGE", "ja_JP:ja:en", 1);
    check_eq(locale_from_environment(), "ja_JP", "LANGUAGE's first entry wins");

    // ...except where the POSIX chain says localisation is off, which is
    // exactly where gettext ignores LANGUAGE too.
    ::setenv("LC_ALL", "C", 1);
    check_eq(locale_from_environment(), "", "C means no localisation, LANGUAGE notwithstanding");
    ::setenv("LC_ALL", "POSIX", 1);
    check_eq(locale_from_environment(), "", "POSIX means the same");

    // Stud's own override beats the lot, so a language can be tried
    // without changing the whole desktop's.
    ::setenv("STUD_LOCALE", "ko_KR", 1);
    check_eq(locale_from_environment(), "ko_KR", "STUD_LOCALE overrides everything");
    clear_locale_env();

    // End to end, the way the engine asks: a Brazilian desktop.
    ::setenv("STUD_LOCALE", "pt_BR.UTF-8", 1);
    {
        std::string language, country;
        stud::android_glue::locale_detail::split_locale(locale_from_environment(), &language,
                                                        &country);
        check_eq(language + "_" + country, "pt_BR", "getLocale() reports the system locale");
        check_eq(roblox_locale_for(language, country), "pt_br",
                 "getRobloxLocale() reports a locale Roblox ships");
    }
    clear_locale_env();

    if (g_failures != 0) {
        std::printf("%d check(s) failed\n", g_failures);
        return 1;
    }
    std::printf("locale_test: all checks passed\n");
    return 0;
}
