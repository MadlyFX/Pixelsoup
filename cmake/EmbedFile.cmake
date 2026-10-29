# Writes a C++ source holding a file's bytes, so a small asset ships inside the
# binary rather than beside it.
#
#   cmake -DINPUT=<file> -DOUTPUT=<source.cpp> -DNAMESPACE=<ns> -DSYMBOL=<name>
#         -P EmbedFile.cmake
#
# The source defines `const unsigned char <SYMBOL>[]` and
# `const std::size_t <SYMBOL>_size` in <NAMESPACE>. It is rewritten only when
# the bytes change, so an unchanged asset never triggers a rebuild.
foreach(required INPUT OUTPUT NAMESPACE SYMBOL)
    if(NOT DEFINED ${required})
        message(FATAL_ERROR "EmbedFile.cmake needs -D${required}=...")
    endif()
endforeach()

file(READ "${INPUT}" hex HEX)
string(LENGTH "${hex}" digits)
math(EXPR bytes "${digits} / 2")
string(REGEX REPLACE "([0-9a-f][0-9a-f])" "0x\\1," array "${hex}")
string(REGEX REPLACE "((0x[0-9a-f][0-9a-f],){16})" "\\1\n    " array "${array}")

file(WRITE "${OUTPUT}.tmp"
"// Generated from ${INPUT} by cmake/EmbedFile.cmake; do not edit.
#include <cstddef>

namespace ${NAMESPACE} {

extern const unsigned char ${SYMBOL}[];
extern const std::size_t ${SYMBOL}_size;

const unsigned char ${SYMBOL}[] = {
    ${array}
};
const std::size_t ${SYMBOL}_size = ${bytes};

} // namespace ${NAMESPACE}
")
file(COPY_FILE "${OUTPUT}.tmp" "${OUTPUT}" ONLY_IF_DIFFERENT)
file(REMOVE "${OUTPUT}.tmp")
