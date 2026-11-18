# Writes OUT defining STUD_COMMIT: SOURCE's short commit, else COMMIT, else empty.
# Runs every build, rewriting only on change so nothing recompiles needlessly.
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
