// SPDX-License-Identifier: GPL-3.0-only
// iBMSC 风格分割线注释（writer 生成的「结构装饰」）。
// 共享给两处：
// - bms_writer.cpp：写出时生成（HEADER/DEFINITION/MAIN DATA/EXPANSION 四条）；
// - bms_parser.cpp：preserve_comments 时**豁免**——这些行是 writer 的结构化输出，
//   不是用户注释；若按普通注释存入 raw_lines，每次 写→读→写 都会在尾部累积一份
//   （2026-09 审查发现：aa6e8a0 引入分割线后 roundtrip 幂等性破坏）。
#pragma once
#include <string_view>

namespace beatbench::bms::detail {

inline constexpr std::string_view kSectionHeaderLine = "*---------------------- HEADER FIELD";
inline constexpr std::string_view kSectionDefLine    = "*---------------------- DEFINITION FIELD";
inline constexpr std::string_view kSectionDataLine   = "*---------------------- MAIN DATA FIELD";
inline constexpr std::string_view kSectionRawLine    = "*---------------------- EXPANSION FIELD";

/// 是否为 writer 生成的分割线注释（精确匹配；行首无空白）。
inline bool is_section_banner(std::string_view line) {
    return line == kSectionHeaderLine || line == kSectionDefLine ||
           line == kSectionDataLine || line == kSectionRawLine;
}

}  // namespace beatbench::bms::detail
