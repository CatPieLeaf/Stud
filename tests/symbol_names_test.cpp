// What Stud points at when an engine entry point it calls has gone: the name
// Roblox would have bumped it to.

#include "stud/linker.h"
#include "check.h"

#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

int main() {
    using Names = std::vector<std::string>;
    using stud::linker::LoadedLibrary;
    check(LoadedLibrary::next_names("Java_a_b_call3") == Names{"Java_a_b_call4"},
          "a trailing number is bumped");
    check(LoadedLibrary::next_names("Java_a_b_startV2Game") == Names{"Java_a_b_startV3Game"},
          "a number inside the method's name is bumped");
    check(LoadedLibrary::next_names("Java_a_b_call") == Names{"Java_a_b_call2", "Java_a_b_callV2"},
          "a name with no number gets a first one");
    check(LoadedLibrary::next_names("Java_a_b_call__Ljava_lang_String_2").empty(),
          "an overload's mangled name has no successor");
    return 0;
}
