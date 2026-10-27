# cmake -DINPUT=<file> -DOUTPUT=<header> -DNAME=<identifier> -P embed.cmake
# Writes the input file as a byte array in a C++ header.
file(READ "${INPUT}" HEX HEX)
string(REGEX REPLACE "([0-9a-f][0-9a-f])" "0x\\1," BYTES "${HEX}")
string(REGEX REPLACE "((0x[0-9a-f][0-9a-f],){32})" "\\1\n    " BYTES "${BYTES}")
file(WRITE "${OUTPUT}" "#pragma once\n#include <cstdint>\n\ninline constexpr uint8_t ${NAME}[] = {\n    ${BYTES}\n};\n")
