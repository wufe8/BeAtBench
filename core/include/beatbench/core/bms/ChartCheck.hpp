// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include <filesystem>
#include <map>
#include <string>
#include <vector>

#include "beatbench/core/Chart.hpp"
#include "beatbench/core/bms/BmsCodec.hpp"

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

/// 定义表条目使用统计（采样面板检索/排序用；info 命令附带输出）。
/// wav 来自 notes（键/皿/踏板/地雷引用数 + 首次小节 + 轨道类别，按 player 分）；
/// bmp 来自 bga_events；bpm/stop 的事件落值不保留原始 id，统计为 0（面板忽略）。
struct SampleUsage {
    std::size_t refs = 0;        ///< 引用次数（note/bga 物件数）
    std::uint32_t first_measure = 0;  ///< 首次引用小节（无引用 = 0）
    std::size_t key1 = 0;        ///< 1P 按键轨引用数
    std::size_t scratch1 = 0;    ///< 1P 皿轨引用数
    std::size_t pedal1 = 0;      ///< 1P 踏板轨引用数
    std::size_t key2 = 0;        ///< 2P 按键轨引用数（DP/双人模式；1P 为 0）
    std::size_t scratch2 = 0;    ///< 2P 皿轨
    std::size_t pedal2 = 0;      ///< 2P 踏板轨
    std::size_t bgm = 0;         ///< 背景音轨（ch01）引用数（游戏不可见，到达自动播放）
};

std::map<std::pair<SampleKind, std::uint32_t>, SampleUsage> collect_sample_usage(
    const Chart& chart);

/// lint 单条结论：code = 稳定机器码（"missing_wav" 等，协议用），
/// message = 中文展示文本。line 0 = 无行号。
/// severity：error / warning / info（info = 不阻塞、仅提示；如扩展名不符）。
/// missing_wav 额外携带 id（槽位）/ file（相对路径），供机器消费；
/// wav_ext_mismatch 额外携带 resolved（实际存在的同名异扩展文件）；
/// overlapping_notes 携带 measure（小节）/ pos（num/den）/ lane（轨道文本）供 GUI 定位；
/// dangling_ln 携带 measure / pos / lane / sample（悬挂端 note 位置）。
struct LintIssue {
    std::string code;
    std::string message;
    Severity severity = Severity::Warning;
    int line = 0;
    std::string id;     ///< 采样槽位（missing_wav / wav_ext_mismatch）
    std::string file;   ///< 引用路径（missing_wav / wav_ext_mismatch）
    std::string resolved;  ///< 实际找到的同名文件（仅 wav_ext_mismatch）
    // —— 位置信息（overlapping_notes / dangling_ln；0 = 无） ——
    std::uint32_t measure = 0;
    std::int64_t pos_num = 0;   ///< 节内位置（Rational num/den；den 0 = 无）
    std::int64_t pos_den = 0;
    std::uint8_t lane_player = 0;  ///< 轨道（Lane：player/kind/index；kind 255 = 无）
    std::uint8_t lane_kind = 255;
    std::uint8_t lane_index = 0;
    std::uint32_t sample = 0;   ///< 采样 id（dangling_ln 用）
};

/// 最小 lint（M1 范围）：缺失 #WAV 引用文件 / 缺 #RANK / 缺 #TOTAL / 空谱面 /
/// 重叠 note / 悬挂 LN（M3 扩充）。
/// base_dir = 谱面所在目录（#WAV 相对路径的解析基准）。
std::vector<LintIssue> lint_chart(const Chart& chart, const std::filesystem::path& base_dir);

}  // namespace beatbench::bms
