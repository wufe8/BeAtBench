// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "beatbench/core/Lane.hpp"

namespace beatbench {

/// 游玩模式：lane 集合 + 展示信息。
/// 视图布局与默认键位由 app 层按 mode_id 查表；异型布局（如 EZ2）走同机制扩展。
struct ChartMode {
    std::string id;           ///< 稳定标识，如 "sp7k"
    std::string display_name; ///< 展示名，如 "SP 7 Keys"
    std::vector<Lane> lanes;
};

/// 按 id 查内置模式；未知 id 返回 nullopt。
/// codec 解析 #PLAYER 后用本函数得到模式，再驱动视图布局。
inline std::optional<ChartMode> chart_mode_by_id(std::string_view /*id*/) {
    return std::nullopt;  // TODO M2：内置 SP7K / SP5K / DP / PMS9K / Battle 配置表
}

}  // namespace beatbench
