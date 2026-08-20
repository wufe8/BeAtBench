// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include <string>
#include <string_view>
#include <vector>

#include "beatbench/core/Chart.hpp"

namespace beatbench::bms {

/// BMS 文本编码。传统 Shift-JIS；现代工具渐用 UTF-8（#ENCODING 注释）。读取建议 Auto。
enum class BmsEncoding { Auto, ShiftJis, Utf8 };

struct BmsReadOptions {
    BmsEncoding encoding = BmsEncoding::Auto;
    bool preserve_comments = true;  ///< 保留注释块以便写回
};

struct BmsWriteOptions {
    BmsEncoding encoding = BmsEncoding::Utf8;  ///< 写回默认 UTF-8，可切换 SJIS
    bool preserve_comments = true;
    bool normalize = true;  ///< 槽位最小化 + 通道按行聚合
};

enum class Severity { Error, Warning, Info };

struct Diagnostic {
    Severity severity = Severity::Info;
    std::string message;
    int line = 0;  ///< 0 = 无行号
};

struct BmsReadResult {
    Chart chart;
    std::vector<Diagnostic> diagnostics;
};

/// 解析 BMS 文本。
/// 实现计划（M1）：纯标准库手写 parser——大小写不敏感、// 与 /* */ 注释、
/// #RANDOM/#IF/#SWITCH 预处理、36 进制 ID 映射、通道号→Lane 映射（对齐稿 02 §4）。
BmsReadResult read_bms(std::string_view text, const BmsReadOptions& opts = {});

/// 读取 .bms 文件（含编码检测：BOM / #ENCODING / SJIS 启发式）。
BmsReadResult read_bms_file(const std::string& path, const BmsReadOptions& opts = {});

/// 写出 BMS 文本（M1 实现；归一化策略见对齐稿 02 §4.3）。
std::string write_bms(const Chart& chart, const BmsWriteOptions& opts = {});

}  // namespace beatbench::bms
