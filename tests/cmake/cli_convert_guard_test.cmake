# SPDX-License-Identifier: GPL-3.0-only
file(MAKE_DIRECTORY "${TEST_DIR}")
set(input "${TEST_DIR}/input.bms")
set(output "${TEST_DIR}/output.bms")
set(marker "unchanged target")

function(check_rejected_conversion)
    file(WRITE "${output}" "${marker}")
    execute_process(COMMAND "${CLI}" convert "${input}" "${output}" ${ARGN}
        RESULT_VARIABLE status OUTPUT_VARIABLE stdout ERROR_VARIABLE stderr)
    file(READ "${output}" actual)
    if(NOT status STREQUAL "1" OR NOT actual STREQUAL marker)
        message(FATAL_ERROR "Expected failure without an overwrite (status=${status})\n${stdout}\n${stderr}")
    endif()
endfunction()

# UTF-16 BOM 是读错误，不得写空谱覆盖现有目标。
string(ASCII 255 254 unsupported_bom)
file(WRITE "${input}" "${unsupported_bom}#TITLE example\n")
check_rejected_conversion()

# UTF-8 能读但 CP932 不可表示；显式转码不得把文件名悄悄写为 ?.wav。
string(ASCII 239 187 191 utf8_bom)
string(ASCII 240 159 152 128 non_cp932)
file(WRITE "${input}" "${utf8_bom}#TITLE example\r\n#WAV01 ${non_cp932}.wav\r\n")
check_rejected_conversion(--encoding sjis)

# 未显式指定输出编码的 CLI 仍写 UTF-8、LF、无 BOM。
execute_process(COMMAND "${CLI}" convert "${input}" "${output}"
    RESULT_VARIABLE status OUTPUT_VARIABLE stdout ERROR_VARIABLE stderr)
file(READ "${output}" written HEX)
string(HEX "${non_cp932}.wav" wav_hex)
if(NOT status STREQUAL "0" OR NOT written MATCHES "${wav_hex}" OR
   written MATCHES "^efbbbf" OR written MATCHES "0d0a")
    message(FATAL_ERROR "Default conversion contract changed (status=${status})\n${stdout}\n${stderr}")
endif()

# 自动判码有歧义时，既不能覆盖不同的现有目标，也不能覆盖输入本身。
string(ASCII 194 169 dual_decodable)
set(ambiguous "#TITLE ${dual_decodable}\n")
file(WRITE "${input}" "${ambiguous}")
check_rejected_conversion()
execute_process(COMMAND "${CLI}" convert "${input}" "${input}"
    RESULT_VARIABLE status OUTPUT_VARIABLE stdout ERROR_VARIABLE stderr)
file(READ "${input}" actual)
if(NOT status STREQUAL "1" OR NOT actual STREQUAL ambiguous)
    message(FATAL_ERROR "Ambiguous input was not protected (status=${status})\n${stdout}\n${stderr}")
endif()

# 明确的源编码允许覆盖，避免把所有可按两种编码解码的正文一律拒绝。
file(WRITE "${input}" "${utf8_bom}${ambiguous}")
execute_process(COMMAND "${CLI}" convert "${input}" "${output}"
    RESULT_VARIABLE status OUTPUT_VARIABLE stdout ERROR_VARIABLE stderr)
file(READ "${output}" written HEX)
if(NOT status STREQUAL "0" OR NOT written MATCHES "c2a9")
    message(FATAL_ERROR "Explicit source encoding was not honored (status=${status})\n${stdout}\n${stderr}")
endif()
