// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include <cstdint>

#include "beatbench/core/Lane.hpp"
#include "beatbench/core/Payloads.hpp"

namespace beatbench::bms {
// 本头只定义通道映射的公共类型（ChannelSemantics/BmsChannelRule）；
// 映射表唯一实现在 codec/BmsChannelMaps.hpp（按游玩模式索引）。

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
/// 查找只看通道字符串（大小写不敏感）；模式差异（SP/DP/PMS）收敛在表内，
/// 由 codec 层按游玩模式选表，解析逻辑不变。
struct BmsChannelRule {
    ChannelSemantics semantics = ChannelSemantics::KeepRaw;
    Lane lane;                              ///< semantics==Note 时有效
    NoteKind note_kind = NoteKind::Normal;  ///< 地雷等
    bool ln_channel = false;                ///< 51-59（1P）/ 61-69（2P）：RDM LN 通道
                                            ///< LNTYPE 1 = 同一通道内按时间序交替头尾；
    ///< LNTYPE 2 = 该通道不参与（头尾在普通通道，尾=#LNOBJ 值）
    std::uint8_t bga_layer = 0;  ///< Bga/BgaPoor 的层号（0=base 1=poor 2=layer 3=layer2）
    /// 该通道是否允许「同小节多行 = 子行」（sub_line；2026-09 泛化）。默认 false = 不应有子行；
    /// 仅 ch01（BGM 背景音）显式 true。驱动：解析（为重复行赋 FIFO 行号）/ 写回（分组多行）/
    /// lint（多行是否合法）/ 编辑器（该通道可展开成子列）。⚠️ 磁盘上无"子通道"——就是重复行。
    bool allow_sub_lines = false;
};

}  // namespace beatbench::bms
