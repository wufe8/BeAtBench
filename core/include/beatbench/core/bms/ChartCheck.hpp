// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include <filesystem>
#include <map>
#include <string>
#include <vector>

#include "beatbench/core/Chart.hpp"

namespace beatbench::bms {

/// 事件统计（info 命令/CLI 展示共用；通道分布按反向映射归回 BMS 通道字符串，
/// 与旧解析器对照口径一致）。模型层只有事件容器，不负责此类展示统计。
struct EventStats {
    std::size_t notes = 0;
    std::size_t ln_pairs = 0;
    std::size_t bpm = 0;
    std::size_t stop = 0;
    std::size_t measure = 0;
    std::size_t bga = 0;
    std::size_t raw_lines = 0;
    std::map<std::string, std::size_t> channels;  ///< 通道名 → note 数
};

EventStats collect_event_stats(const Chart& chart);

/// lint 单条结论：code = 稳定机器码（"missing_wav" 等，协议用），
/// message = 中文展示文本。line 0 = 无行号。
/// missing_wav 额外携带 id（槽位）/ file（相对路径），供机器消费。
struct LintIssue {
    std::string code;
    std::string message;
    int line = 0;
    std::string id;    ///< 采样槽位（仅 missing_wav）
    std::string file;  ///< 相对路径（仅 missing_wav）
};

/// 最小 lint（M1 范围）：缺失 #WAV 引用文件 / 缺 #RANK / 缺 #TOTAL / 空谱面。
/// base_dir = 谱面所在目录（#WAV 相对路径的解析基准）。
std::vector<LintIssue> lint_chart(const Chart& chart, const std::filesystem::path& base_dir);

}  // namespace beatbench::bms
