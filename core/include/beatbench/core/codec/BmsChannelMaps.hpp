// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include <optional>
#include <string>
#include <string_view>

#include "beatbench/core/bms/ChannelMap.hpp"

namespace beatbench::bms {

/// 按游玩模式查 BMS 通道映射表（正向：通道字符串 → 语义/Lane）。
/// mode：sp7k（默认，含 5k 呈现）/ dp / battle（三者共用 7key 表）/ pms9k（9key 表）。
/// 未知 mode → 按 7key 表处理（兼容默认）。未知通道返回 nullopt（调用方按 KeepRaw）。
///
/// 与 bms_channel_rule（默认 7key，M2 会话 ChartViewItem 在用）等价：
/// bms_channel_rule_for("sp7k", ch) 与 bms_channel_rule(ch) 结果一致。
std::optional<BmsChannelRule> bms_channel_rule_for(std::string_view mode,
                                                   std::string_view channel);

/// 按游玩模式反向映射（Lane + 物件属性 → BMS 通道字符串；写回/统计用）。
/// 语义与 bms_channel_for 相同，mode 决定用哪张表（pms9k → 9key，其余 → 7key）。
std::string bms_channel_for_mode(std::string_view mode, const Lane& lane, bool ln,
                                 NoteKind kind);

}  // namespace beatbench::bms
