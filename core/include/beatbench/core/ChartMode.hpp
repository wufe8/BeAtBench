// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "beatbench/core/Lane.hpp"

namespace beatbench {

/// 游玩模式：lane 集合 + 展示信息（格式无关配置表）。
/// 视图布局与默认键位由 app 层按 mode_id 查表；异型布局（如 EZ2）走同机制扩展。
/// 模式与格式正交：一个格式（如 bms）可有多个模式（sp7k/dp/battle/pms9k）；
/// 通道映射表按格式 × 模式索引（bms 的通道表见 codec 层，此处只描述抽象模式）。
struct ChartMode {
    std::string id;            ///< 稳定标识，如 "sp7k"
    std::string display_name;  ///< 展示名，如 "SP 7 Keys"
    std::vector<Lane> lanes;
};

/// 按 id 查内置模式；未知 id 返回 nullopt。
/// 内置：sp7k（默认，含 5k 呈现）/ dp / battle / pms9k。
/// 其他模式（sp5k 等）按需追加；bmson 的 mode_line 由 codec 映射到这些 id。
std::optional<ChartMode> chart_mode_by_id(std::string_view id);

/// 模式的 1P 键轨数（sp7k/dp/battle=7、pms9k=9）：lanes 中 player 0 的 Key lane
/// 最大 index。未知 id 返回 nullopt（无表可查；调用方显式决定降级，core 不静默兜底）。
/// pms9k-bmesp 写回变体按 pms9k 归一。拖动/变换等键号上限的单一事实源。
std::optional<int> chart_mode_key_count(std::string_view id);

/// lane 是否在该模式的 lanes 表内（定位/显示合法性；单一事实源 = 模式配置表）。
/// 未知 id 返回 false（无表可查）。pms9k-bmesp 变体按家族归一。
bool chart_mode_has_lane(std::string_view id, const Lane& lane);

/// lane 是否为可摆音的游玩轨（表内且 kind 为 Key/Scratch；player 范围由表保证）。
/// 编辑策略谓词：键盘快速摆音与配置重排共用的目标轨判定。踏板/BGM/元轨不在此列。
inline bool is_playable_lane(std::string_view id, const Lane& lane) {
    if (lane.kind != LaneKind::Key && lane.kind != LaneKind::Scratch) return false;
    return chart_mode_has_lane(id, lane);
}

}  // namespace beatbench
