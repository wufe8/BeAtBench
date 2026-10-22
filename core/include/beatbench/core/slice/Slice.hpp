// SPDX-License-Identifier: GPL-3.0-only
// 切片位置规划（M6.2 切音）：从「位置源」（网格 / MIDI note）推导切片边界。
//
// 定位（doc/core-model.md 三、）：切片位置源可切换——网格（BPM/拍细分，确定性、可单测）先行，
// MIDI 为推荐音乐源（note on/off 天然给边界）。本模块 = 纯计算（零 Qt/零依赖）：
// 输入位置源配置/MidiFile → 输出切片表（start/end 秒）。offset（全局）在此应用：
// 秒 = 原始秒 + offsetSec（正 = MIDI/网格相对音频延后；用户 2026-09 拍板：先全局）。
//
// 与 M6.3 衔接：切片表 → 导出（ReferenceTrack::window + fade）/ #WAV 分配 / 铺放。
// 错误策略：参数非法（bpm≤0、subdivision≤0、非有限值）→ warnings + 空表，不抛异常。
#pragma once

#include <string>
#include <vector>

#include "beatbench/core/midi/MidiFile.hpp"

namespace beatbench::slice {

/// 一个切片（位置源切出的一段音频，[startSec, endSec)）。
struct Slice {
    int index = 0;        ///< 序号（0 起；按 startSec 升序）
    double startSec = 0.0;
    double endSec = 0.0;
    std::string kind;     ///< "grid" | "midi" | "manual"（来源标注；UI 着色/后续放置策略用）
    int note = 0;         ///< midi 音高（grid/manual = 0）
    int noteCount = 0;    ///< 该起始点合并的 midi note 数（和弦合并；1 = 单音，grid/manual = 0）
    int startTick = -1;   ///< midi 绝对 tick（grid/manual = -1；后续拍位换算用）
};

struct SlicePlan {
    std::vector<Slice> slices;      ///< 按 startSec 升序；无切片 = 空
    std::vector<std::string> warnings;  ///< 可恢复的输入问题（人类可读中文）
    double durationSec = 0.0;       ///< 已知的音频时长（0 = 未知/未限制）
};

/// 网格源配置（BPM/每拍细分/offset；beatsPerMeasure 仅随附元数据——M6.3 铺放拍位显示用）。
struct GridConfig {
    double bpm = 120.0;
    int beatsPerMeasure = 4;
    int subdivision = 4;      ///< 每拍几等分（4=16 分、3=三连、2=8 分、6=24 分…）
    double offsetSec = 0.0;   ///< 网格第一线位置（音频秒；负值切片起点夹到 0）
    double durationSec = 0.0; ///< 生成上限（<=0 = 不限时）
};

/// 网格源：offset 起每 (60/bpm/subdivision) 秒一条边界 → 相邻边界成切片。
SlicePlan plan_from_grid(const GridConfig& cfg);

/// MIDI 源：`extendToNextOnset=true`（默认，2026-09 用户）= **与手动切片一致的 A 模式**——
/// 只标起始点，右边界 = 下一个唯一起始点/音频末尾；同一起始多 note（和弦）合并成一片
/// （noteCount 计数）。`false` = 每个 note 一片 [on, off)（历史行为）。
/// durationSec > 0 时末端夹逼（起点超过时长的切片丢弃）。
SlicePlan plan_from_midi(const midi::MidiFile& midi, double offsetSec,
                         double durationSec = 0.0, bool extendToNextOnset = true);

}  // namespace beatbench::slice
