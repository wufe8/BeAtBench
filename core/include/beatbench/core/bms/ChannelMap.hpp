// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

#include "beatbench/core/Lane.hpp"
#include "beatbench/core/Payloads.hpp"
#include "beatbench/core/Chart.hpp"

namespace beatbench::bms {

/// 通道语义（格式无关）：BMS 数据行 `#mmmcc:…` 的通道号字符串
/// 经映射表得到本语义；其他格式（bmson/osu/JSON…）用各自的映射规则
/// 直接产出 Lane 与事件，不经过本表——这就是「换表即可支持新格式」的落点。
///
/// 关于 BGA 层：04/06/07/0A 都是 BGA 图层通道，语义上同属 Bga 家族，
/// 层号由 bga_layer 给出（0=base 1=poor 2=layer 3=layer2，与 Bga 结构体一致）。
enum class ChannelSemantics : std::uint8_t {
    KeepRaw,     ///< 未结构化（未知/控制通道），写回原样
    MeasureLen,  ///< ch02：小节拍数
    BpmInline,   ///< ch03：BPM（直接数值或 #BPMxx 引用）
    Bga,         ///< ch04/07/0A：BGA（#BMPxx 引用；层号见 bga_layer）
    BgaPoor,     ///< ch06：判负 BGA（poor 层）
    BpmRef,      ///< ch08：BPM 变化（#BPMxx 引用）
    StopRef,     ///< ch09：STOP（#STOPxx 引用）
    Note,        ///< 11-19/21-29/51-69/D1-D9/E1-E9：需 Lane 的物件
};

/// 通道规则（映射表条目）。
/// 查找只看通道字符串（大小写不敏感）；模式差异（SP/DP）收敛在表内，
/// 未来按模式注册不同表即可，解析逻辑不变。
struct BmsChannelRule {
    ChannelSemantics semantics = ChannelSemantics::KeepRaw;
    Lane lane;                    ///< semantics==Note 时有效
    NoteKind note_kind = NoteKind::Normal;  ///< 地雷等
    bool ln_channel = false;      ///< 51-59（1P）/ 61-69（2P）：RDM LN 通道
                                  ///< LNTYPE 1 = 同一通道内按时间序交替头尾；
                                  ///< LNTYPE 2 = 该通道不参与（头尾在普通通道，尾=#LNOBJ 值）
    std::uint8_t bga_layer = 0;   ///< Bga/BgaPoor 的层号（0=base 1=poor 2=layer 3=layer2）
};

/// 内置 BMS 通道映射表（BMS 笔记「5/7key SP/DP 模式游玩轨」与 hitkey 通道表一致）：
///   11-15=1P 键1-5  16=1P 皿  17=1P 踏板/保留  18=1P 键6  19=1P 键7
///   21-29=2P 同构（26=皿 27=踏板 28=键6 29=键7）
///   51-59=1P LN 通道  61-69=2P LN 通道（RDM 记法：LNTYPE 1 同通道交替头尾；
///           LNTYPE 2 不用此通道，头尾在普通通道，尾=#LNOBJ 值）
///   D1-D9=1P 地雷  E1-E9=2P 地雷（槽位与 11-19/21-29 同构）
/// 9key（PMS）为另一套定义（见 notes「9key PMS」），本表为 5/7key SP/DP；
/// PMS 支持时按模式注册新表。未知通道返回 nullopt（调用方按 KeepRaw 处理）。
std::optional<BmsChannelRule> bms_channel_rule(std::string_view channel);

/// 反向映射：Lane + 物件属性 → BMS 通道字符串（写回/统计用）。
/// ln 仅用于 RDM LN 通道（LNTYPE 1）：1P 侧→5x，2P 侧→6x；
/// LNTYPE 2 的头尾不在独立通道，调用方应传 ln=false 并与 LNOBJ 文本配合。
/// 无法表示（未知 LaneKind/键号）时返回空串。
std::string bms_channel_for(const Lane& lane, bool ln, NoteKind kind);

}  // namespace beatbench::bms
