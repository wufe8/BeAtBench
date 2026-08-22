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
    std::string id;           ///< 稳定标识，如 "sp7k"
    std::string display_name; ///< 展示名，如 "SP 7 Keys"
    std::vector<Lane> lanes;
};

/// 按 id 查内置模式；未知 id 返回 nullopt。
/// 内置：sp7k（默认，含 5k 呈现）/ dp / battle / pms9k。
/// 其他模式（sp5k 等）按需追加；bmson 的 mode_line 由 codec 映射到这些 id。
std::optional<ChartMode> chart_mode_by_id(std::string_view id);

}  // namespace beatbench
