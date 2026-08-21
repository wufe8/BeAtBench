// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include <string>
#include <string_view>

#include "beatbench/core/bms/BmsCodec.hpp"

namespace beatbench::bms {

/// 检测字节流编码：BOM → #ENCODING 声明 → 严格 UTF-8 校验启发式。
DetectedEncoding detect_encoding(std::string_view bytes);

/// 严格 UTF-8 校验（整个输入必须合法；纯 ASCII 视为合法）。
bool is_valid_utf8(std::string_view in);

/// SJIS(CP932) → UTF-8。无效序列 → U+FFFD。
/// 表见 sjis_table.hpp（tools/gen_sjis_table.ps1 生成，三平台一致）。
std::string shiftjis_to_utf8(std::string_view in);

/// UTF-8 → SJIS(CP932)。不可映射码点 → '?'（BMSE 写回惯例）。
std::string utf8_to_shiftjis(std::string_view in);

}  // namespace beatbench::bms
