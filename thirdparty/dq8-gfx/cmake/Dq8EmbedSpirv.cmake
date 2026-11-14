# Wraps a SPIR-V module as a constexpr uint32_t array in a generated header.
# Run in script mode by dq8_add_shader; expects INPUT, OUTPUT and SYMBOL.

if(NOT DEFINED INPUT OR NOT DEFINED OUTPUT OR NOT DEFINED SYMBOL)
    message(FATAL_ERROR "Dq8EmbedSpirv.cmake requires INPUT, OUTPUT and SYMBOL")
endif()

file(SIZE "${INPUT}" SPIRV_SIZE)
if(SPIRV_SIZE EQUAL 0)
    message(FATAL_ERROR "Dq8EmbedSpirv.cmake: ${INPUT} is empty")
endif()
math(EXPR SPIRV_REMAINDER "${SPIRV_SIZE} % 4")
if(NOT SPIRV_REMAINDER EQUAL 0)
    message(FATAL_ERROR "Dq8EmbedSpirv.cmake: ${INPUT} is ${SPIRV_SIZE} bytes, not a whole number of SPIR-V words")
endif()

file(READ "${INPUT}" SPIRV_HEX HEX)

# SPIR-V is a little-endian word stream; emit words rather than bytes so the
# array is correctly aligned for the API without a memcpy at load time.
# Whole-string regexes, linear in the module's size (Rogue Galaxy: changed;
# a list(GET) a byte was quadratic, 47 s for the largest shader on its own):
# each word's bytes reversed, then a line break after every eighth word.
set(H "[0-9a-f][0-9a-f]")
string(REGEX REPLACE "(${H})(${H})(${H})(${H})" "0x\\4\\3\\2\\1u, " WORDS "${SPIRV_HEX}")
set(W "0x[0-9a-f]+u, ")
string(REGEX REPLACE "(${W}${W}${W}${W}${W}${W}${W}0x[0-9a-f]+u,) " "\\1\n    " WORDS "${WORDS}")

get_filename_component(INPUT_NAME "${INPUT}" NAME)
set(CONTENT "// Generated from ${INPUT_NAME}. Do not edit.\n")
string(APPEND CONTENT "#pragma once\n\n#include <cstdint>\n\n")
string(APPEND CONTENT "inline constexpr uint32_t ${SYMBOL}[] = {\n    ${WORDS}\n};\n")
file(WRITE "${OUTPUT}" "${CONTENT}")
