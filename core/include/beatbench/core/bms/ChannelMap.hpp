// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

#include "beatbench/core/Lane.hpp"
#include "beatbench/core/Payloads.hpp"

namespace beatbench::bms {

/// 通道语义（格式无关）：BMS 数据行 `#mmmcc:…` 的通道号字符串
/// 经映射表得到本语义；其他格式（bmson/osu/JSON…）用各自的映射规则
/// 直接产出 Lane 与事件，不经过本表——这就是「换表即可支持新格式」的落点。
enum class ChannelSemantics : std::uint8_t {
    KeepRaw,     ///< 未结构化（ch01 背景铺底、ch07 扩展文本、未知通道），写回原样
    MeasureLen,  ///< ch02：小节拍数
    BpmInline,   ///< ch03：BPM（直接数值或 #BPMxx 引用）
    Bga,         ///< ch04：BGA（#BMPxx 引用，base 层）
    BgaPoor,     ///< ch06：判负 BGA（poor 层）
    BpmRef,      ///< ch08：BPM 变化（#BPMxx 引用）
    StopRef,     ///< ch09：STOP（#STOPxx 引用）
    Note,        ///< 11-19/21-29/51-59/61-69/D1-D9/E1-E9：需 Lane 的物件
};

/// 通道规则（映射表条目）。
/// 查找只看通道字符串（大小写不敏感）；模式差异（SP/PMS/DP）收敛在表内，
/// 未来按模式注册不同表即可，解析逻辑不变。
struct BmsChannelRule {
    ChannelSemantics semantics = ChannelSemantics::KeepRaw;
    Lane lane;                    ///< semantics==Note 时有效
    NoteKind note_kind = NoteKind::Normal;  ///< 地雷等
    bool ln_head = false;         ///< 51-59：LN 头
    bool ln_tail = false;         ///< 61-69：LN 尾（LNTYPE 2 时值==LNOBJ 才算尾）
};

/// 内置 BMS 通道映射表（beatoraja 共识）：
///   11-15=1P 键1-5  16=1P 皿  17=1P 键6  18=1P 键7  19=1P 踏板
///   21-29=2P 同构；51-59=LN 头、61-69=LN 尾（对应 11-19）；
///   D1-D9/E1-E9=地雷（对应 11-19 键位）
/// 未知通道返回 nullopt（调用方按 KeepRaw 处理）。
std::optional<BmsChannelRule> bms_channel_rule(std::string_view channel);

/// 反向映射：Lane + 物件属性 → BMS 通道字符串（写回重建/统计用）。
/// 无法表示（未知 LaneKind/键号）时返回空串。
std::string bms_channel_for(const Lane& lane, bool ln_head, bool ln_tail, NoteKind kind);

}  // namespace beatbench::bms
