# Writes IN as a C++ byte array named NAME into OUT.
file(READ "${IN}" hex HEX)
string(REGEX REPLACE "([0-9a-f][0-9a-f])" "0x\\1," bytes "${hex}")
file(WRITE "${OUT}.tmp" "#pragma once\n// Generated from ${IN}.\ninline constexpr unsigned char ${NAME}[] = {${bytes}};\n")
file(COPY_FILE "${OUT}.tmp" "${OUT}" ONLY_IF_DIFFERENT)
