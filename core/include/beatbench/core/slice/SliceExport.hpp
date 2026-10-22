// SPDX-License-Identifier: GPL-3.0-only
// M6.3 分片导出布局（纯计算，零 Qt/零音频）：#WAV id 分配 + 切片→拍位 + 可复制 BMS raw。
//
// 定位（doc/core-model.md 三、 M6.3）：切音工作台把参考音频切成 one-shot 分片并导出为 .wav，
// 再按拍位铺进 ch01（BGM）重建谱面。本模块 = 布局层（与文件 I/O / 音频解码解耦）：
// - 分配最低空闲 #WAV id（跳过谱面已占用；支持「导出起始 id」连续分配）；
// - 把每个切片起点（区间秒）换算成拍位（measure + pos）；
// - 文件序号与 #WAV id 独立：首次仍从 000 起；重名时可选覆盖 / 从 max+1 续号 / 拒绝；
// - 生成「可复制 BMS raw」——#WAVxx 定义 + ch01 铺放 note 行（复用 bms::write_bms，
//   正确性由 writer 保证），供粘贴到编辑区。
//
// 文件落盘 window()+fade+write_wav 在 audio 层（导出原语），GUI 工作台组合调用。
#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "beatbench/core/Rational.hpp"
#include "beatbench/core/Chart.hpp"
#include "beatbench/core/slice/Slice.hpp"

namespace beatbench::slice {

/// 一个导出分片的布局结果。
struct SliceExportItem {
    int sliceIndex = 0;       ///< 原切片表序号
    std::uint32_t wavId = 0;  ///< 分配的 #WAV id
    std::string fileName;     ///< 落盘文件名（如 slice_000.wav）
    int fileIndex = -1;       ///< 文件序号（与 #WAV id / 切片表下标独立；未启用 = -1）
    int measure = 0;          ///< 铺放拍位（小节）
    Rational pos;             ///< 节内位置（[0,1)）
    bool enabled = false;     ///< 放置开关（决定是否生成 ch01 note）
};

/// 落盘文件名冲突策略（B2）：文件序号与 #WAV id 独立；无冲突时三种等价于按 layout 原名写。
enum class ExportConflictPolicy {
    Overwrite,  ///< 按 layout 原名写（已存在则覆盖）
    Continue,   ///< 只给启用切片从 max(已有序号)+1 连续编号，旧文件保留
    Error,      ///< 有冲突则拒绝（调用方弹窗 / 调试路径失败零落盘）
};

/// 解析 `{baseName}_{NNN}.wav` 中的序号；格式不符 → -1。
/// baseName 可含子目录（如 `slices/slice`）；匹配时按整段前缀，不把目录当通配。
int parse_export_file_index(const std::string& fileName, const std::string& baseName,
                            int width = 3);

/// 已占用序号的下一续号起点：空集 → 0；否则 max+1（不回填空洞）。
int next_continue_file_index(const std::vector<int>& occupiedIndexes);

/// 为 count 个即将写盘的启用切片分配续号：从 next_continue_file_index(occupied) 起连续 count 个。
std::vector<int> allocate_continue_file_indexes(const std::vector<int>& occupiedIndexes,
                                                int count);

/// 按策略改写 items 的 fileIndex/fileName。Error 且存在冲突名 → false（items 不变）。
/// occupiedIndexes = 目标目录已有同前缀 wav 的序号；collidingNames = layout 原名与磁盘撞车的相对路径。
/// wavId / 拍位不动。
bool apply_export_file_policy(std::vector<SliceExportItem>& items, const std::string& baseName,
                              const std::vector<int>& occupiedIndexes,
                              const std::vector<std::string>& collidingNames,
                              ExportConflictPolicy policy, int width = 3);

/// 为 count 个切片分配最低空闲 #WAV id：从 start_id 起，跳过 occupied + 本次已分配。
/// 超出上限（1295 = ZZ）时截断（count 只计到可分配数）。返回按序的长度 ≤ count。
std::vector<std::uint32_t> allocate_wav_ids(
    const std::vector<std::uint32_t>& occupied, std::uint32_t start_id, int count,
    IdBase idBase = IdBase::Base36);

/// 汇总导出布局：切片 → {id, 文件名, 拍位}。enabled[i] 决定是否铺放。
/// 拍位换算：beat = (startSec - offset) * bpm / 60；measure = floor(beat/beatsPerMeasure)；
/// pos = round(fracBeat*subdivision) / (beatsPerMeasure*subdivision)（吸附到细分网格）。
/// 文件序号只给启用切片从 0 连续编号（未勾选不占号、不写盘）；与切片表下标、#WAV id 都独立。
/// baseName = 文件名前缀（如 "slice"），序号补零到 width 位。
/// startMeasure = 铺放起始小节（1-based：ch01 从第 N 小节起 = 文件 `#(N-1)01:`；
/// 所有切片的 measure 整体平移 startMeasure-1；默认 1 = 从第 1 小节起，即文件 #000）。
std::vector<SliceExportItem> build_export_layout(
    const std::vector<Slice>& slices, const std::vector<bool>& enabled,
    const std::vector<std::uint32_t>& occupied, std::uint32_t start_id,
    const std::string& baseName, double bpm, int beatsPerMeasure,
    int subdivision, double offset, int startMeasure = 1, int width = 3,
    IdBase idBase = IdBase::Base36);

/// 生成「可复制 BMS raw」：把 items 铺进一个临时 Chart（WAV 定义 + ch01 note），
/// 经 bms::write_bms 写出再筛掉杂线，返回 `#WAVxx <file>` 定义 + ch01/ch02 数据行。
/// Base62 导出额外保留 `#BASE 62` 声明行作为来源进制标记（clipboard.paste 忽略 #BASE，
/// 不改变目标谱面进制；GUI 据此在粘贴进 Base36 谱面前提示数值歧义）。
/// beatsPerMeasure 决定 ch02 小节长度（=4 时按默认 4/4，不输出）。
std::string build_placement_raw(const std::vector<SliceExportItem>& items,
                                double bpm, int beatsPerMeasure, IdBase idBase = IdBase::Base36);

}  // namespace beatbench::slice
