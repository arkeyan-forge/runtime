# cmake -DINPUT=<file.dxil> -DOUTPUT=<header> -DNAME=<identifier> -P embed_dxil.cmake
# Like embed.cmake, for a DXIL container from dxc: also fills in the container hash. D3D12 rejects a container whose
# hash does not match ("not signed"), and dxc only writes it when it finds the validator library (dxil.dll,
# libdxil.so), which the macOS build lacks. The hash is MD5 over the bytes after the hash field, with a different
# final block: the bit count first and (byte count * 2) | 1 last (DXC's ComputeHashRetail). Checked against
# containers that dxil.dll signed (the precompiled shaders in SDL's D3D12 backends).

file(READ "${INPUT}" HEX HEX)
if(NOT HEX MATCHES "^44584243") # "DXBC"
    message(FATAL_ERROR "${INPUT} is not a DXIL container")
endif()

set(MD5_K
    0xd76aa478 0xe8c7b756 0x242070db 0xc1bdceee 0xf57c0faf 0x4787c62a 0xa8304613 0xfd469501
    0x698098d8 0x8b44f7af 0xffff5bb1 0x895cd7be 0x6b901122 0xfd987193 0xa679438e 0x49b40821
    0xf61e2562 0xc040b340 0x265e5a51 0xe9b6c7aa 0xd62f105d 0x02441453 0xd8a1e681 0xe7d3fbc8
    0x21e1cde6 0xc33707d6 0xf4d50d87 0x455a14ed 0xa9e3e905 0xfcefa3f8 0x676f02d9 0x8d2a4c8a
    0xfffa3942 0x8771f681 0x6d9d6122 0xfde5380c 0xa4beea44 0x4bdecfa9 0xf6bb4b60 0xbebfbc70
    0x289b7ec6 0xeaa127fa 0xd4ef3085 0x04881d05 0xd9d4d039 0xe6db99e5 0x1fa27cf8 0xc4ac5665
    0xf4292244 0x432aff97 0xab9423a7 0xfc93a039 0x655b59c3 0x8f0ccc92 0xffeff47d 0x85845dd1
    0x6fa87e4f 0xfe2ce6e0 0xa3014314 0x4e0811a1 0xf7537e82 0xbd3af235 0x2ad7d2bb 0xeb86d391)
set(MD5_SHIFT 7 12 17 22 7 12 17 22 7 12 17 22 7 12 17 22 5 9 14 20 5 9 14 20 5 9 14 20 5 9 14 20
    4 11 16 23 4 11 16 23 4 11 16 23 4 11 16 23 6 10 15 21 6 10 15 21 6 10 15 21 6 10 15 21)
set(MD5_WORD 0 1 2 3 4 5 6 7 8 9 10 11 12 13 14 15 1 6 11 0 5 10 15 4 9 14 3 8 13 2 7 12
    5 8 11 14 1 4 7 10 13 0 3 6 9 12 15 2 0 7 14 5 12 3 10 1 8 15 6 13 4 11 2 9)

# Little-endian 32-bit words of whole 64-byte blocks (hex text), as hex literals for math().
function(md5_blocks hex)
    string(REGEX REPLACE "(..)(..)(..)(..)" "0x\\4\\3\\2\\1;" words "${hex}")
    list(LENGTH words count)
    math(EXPR last "${count} / 16 - 1")
    foreach(block RANGE ${last})
        set(a ${STATE_A})
        set(b ${STATE_B})
        set(c ${STATE_C})
        set(d ${STATE_D})
        foreach(i RANGE 63)
            if(i LESS 16)
                math(EXPR f "(${b} & ${c}) | (~${b} & ${d})")
            elseif(i LESS 32)
                math(EXPR f "(${b} & ${d}) | (${c} & ~${d})")
            elseif(i LESS 48)
                math(EXPR f "${b} ^ ${c} ^ ${d}")
            else()
                math(EXPR f "${c} ^ (${b} | ~${d})")
            endif()
            list(GET MD5_WORD ${i} index)
            math(EXPR index "${block} * 16 + ${index}")
            list(GET words ${index} x)
            list(GET MD5_K ${i} k)
            list(GET MD5_SHIFT ${i} s)
            math(EXPR f "(${f} + ${a} + ${k} + ${x}) & 0xFFFFFFFF")
            set(a ${d})
            set(d ${c})
            set(c ${b})
            math(EXPR b "(${b} + ((${f} << ${s}) | (${f} >> (32 - ${s})))) & 0xFFFFFFFF")
        endforeach()
        math(EXPR STATE_A "(${STATE_A} + ${a}) & 0xFFFFFFFF")
        math(EXPR STATE_B "(${STATE_B} + ${b}) & 0xFFFFFFFF")
        math(EXPR STATE_C "(${STATE_C} + ${c}) & 0xFFFFFFFF")
        math(EXPR STATE_D "(${STATE_D} + ${d}) & 0xFFFFFFFF")
    endforeach()
    set(STATE_A ${STATE_A} PARENT_SCOPE)
    set(STATE_B ${STATE_B} PARENT_SCOPE)
    set(STATE_C ${STATE_C} PARENT_SCOPE)
    set(STATE_D ${STATE_D} PARENT_SCOPE)
endfunction()

# 32-bit value as 8 hex digits in little-endian byte order.
function(le32 out value)
    math(EXPR value "(${value}) | 0x100000000" OUTPUT_FORMAT HEXADECIMAL)
    string(TOLOWER "${value}" value)
    string(REGEX REPLACE "^0x1(..)(..)(..)(..)$" "\\4\\3\\2\\1" value "${value}")
    set(${out} ${value} PARENT_SCOPE)
endfunction()

string(SUBSTRING "${HEX}" 40 -1 DATA)
string(LENGTH "${DATA}" DATA_LENGTH)
math(EXPR SIZE "${DATA_LENGTH} / 2")
math(EXPR FULL_LENGTH "${SIZE} / 64 * 128")
math(EXPR REST "${SIZE} % 64")
string(SUBSTRING "${DATA}" 0 ${FULL_LENGTH} FULL)
string(SUBSTRING "${DATA}" ${FULL_LENGTH} -1 TAIL)
le32(BIT_COUNT "(${SIZE} * 8) & 0xFFFFFFFF")
le32(LAST_WORD "((${SIZE} * 2) | 1) & 0xFFFFFFFF")
if(REST LESS 56)
    math(EXPR PADDING "55 - ${REST}")
    string(REPEAT "00" ${PADDING} ZEROS)
    set(TAIL "${BIT_COUNT}${TAIL}80${ZEROS}${LAST_WORD}")
else()
    math(EXPR PADDING "63 - ${REST}")
    string(REPEAT "00" ${PADDING} ZEROS)
    string(REPEAT "00" 56 ZEROS56)
    set(TAIL "${TAIL}80${ZEROS}${BIT_COUNT}${ZEROS56}${LAST_WORD}")
endif()

set(STATE_A 0x67452301)
set(STATE_B 0xefcdab89)
set(STATE_C 0x98badcfe)
set(STATE_D 0x10325476)
if(FULL_LENGTH GREATER 0)
    md5_blocks("${FULL}")
endif()
md5_blocks("${TAIL}")

le32(A ${STATE_A})
le32(B ${STATE_B})
le32(C ${STATE_C})
le32(D ${STATE_D})
string(SUBSTRING "${HEX}" 0 8 MAGIC)
set(HEX "${MAGIC}${A}${B}${C}${D}${DATA}")

string(REGEX REPLACE "([0-9a-f][0-9a-f])" "0x\\1," BYTES "${HEX}")
string(REGEX REPLACE "((0x[0-9a-f][0-9a-f],){32})" "\\1\n    " BYTES "${BYTES}")
file(WRITE "${OUTPUT}" "#pragma once\n#include <cstdint>\n\ninline constexpr uint8_t ${NAME}[] = {\n    ${BYTES}\n};\n")
