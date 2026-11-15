# Writes OUT, a header defining STUD_COMMIT: the short commit of the source
# tree at SOURCE, or the COMMIT given, or empty when neither is known (a
# source archive with no .git). Run on every build, so the commit a log names
# is the one the binary was built from, and rewritten only when it changes,
# so an unchanged commit recompiles nothing.
set(commit "${COMMIT}")
if(commit STREQUAL "")
    execute_process(COMMAND git -C "${SOURCE}" rev-parse --short HEAD
                    OUTPUT_VARIABLE commit OUTPUT_STRIP_TRAILING_WHITESPACE
                    ERROR_QUIET RESULT_VARIABLE failed)
    if(failed)
        set(commit "")
    endif()
endif()
set(content "#pragma once\n#define STUD_COMMIT \"${commit}\"\n")
if(EXISTS "${OUT}")
    file(READ "${OUT}" existing)
endif()
if(NOT existing STREQUAL content)
    file(WRITE "${OUT}" "${content}")
endif()
