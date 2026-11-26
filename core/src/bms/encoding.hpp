// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include <optional>
#include <string>
#include <string_view>

#include "beatbench/core/bms/BmsCodec.hpp"

namespace beatbench::bms {

struct ByteEncoding {
    DetectedEncoding encoding = DetectedEncoding::Unknown;
    bool utf8_bom = false;
    bool ambiguous = false;
    std::string_view error;
};

/// BOM、真实 #ENCODING 和显式选择必须一致；没有证据时严格校验两种编码。
ByteEncoding inspect_encoding(std::string_view bytes, BmsEncoding requested);
/// 提取行首声明参数；维护块注释状态以同时供检测和写出使用。
std::optional<std::string_view> encoding_declaration(std::string_view line, bool& block_comment);

inline constexpr std::string_view kUtf8Bom = "\xEF\xBB\xBF";

struct TextLine {
    std::string_view text;
    std::string_view ending;
};

/// 消费一行，同时识别 LF、CRLF 和裸 CR；最后无行尾时 ending 为空。
TextLine take_text_line(std::string_view& text);

struct LineEndings {
    std::string_view kind;
    std::string_view preferred;
};

LineEndings inspect_line_endings(std::string_view bytes);

bool is_ascii(std::string_view in);
bool is_valid_utf8(std::string_view in);
bool is_valid_cp932(std::string_view in);

/// SJIS(CP932) → UTF-8。通常先验证；无效序列降级为 U+FFFD。
std::string shiftjis_to_utf8(std::string_view in);

/// UTF-8 → SJIS(CP932)。strict 时拒绝无效 UTF-8 或不可映射字符；否则保留旧回退。
std::string utf8_to_shiftjis(std::string_view in, bool strict = false);

}  // namespace beatbench::bms
