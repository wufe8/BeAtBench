// SPDX-License-Identifier: GPL-3.0-only
// 内置游玩模式配置表（格式无关）。通道映射表（BMS 的 11-19/51-69 等）不在本文件：
// 那是 codec 层按格式 × 模式索引的映射，见 core/src/codec/bms_channel_maps.cpp。
#include "beatbench/core/ChartMode.hpp"

namespace beatbench {
namespace {

// 键 1..7（1P）
constexpr Lane k1p[7] = {
    {0, LaneKind::Key, 1}, {0, LaneKind::Key, 2}, {0, LaneKind::Key, 3},
    {0, LaneKind::Key, 4}, {0, LaneKind::Key, 5}, {0, LaneKind::Key, 6},
    {0, LaneKind::Key, 7},
};
// 键 1..9（1P，PMS 9key：无皿/踏板）
constexpr Lane k1p9[9] = {
    {0, LaneKind::Key, 1}, {0, LaneKind::Key, 2}, {0, LaneKind::Key, 3},
    {0, LaneKind::Key, 4}, {0, LaneKind::Key, 5}, {0, LaneKind::Key, 6},
    {0, LaneKind::Key, 7}, {0, LaneKind::Key, 8}, {0, LaneKind::Key, 9},
};
// 键 1..7（2P）
constexpr Lane k2p[7] = {
    {1, LaneKind::Key, 1}, {1, LaneKind::Key, 2}, {1, LaneKind::Key, 3},
    {1, LaneKind::Key, 4}, {1, LaneKind::Key, 5}, {1, LaneKind::Key, 6},
    {1, LaneKind::Key, 7},
};

// 7key SP（含 5key 呈现）：键1-5 + 皿 + 踏板/保留 + 键6 + 键7（BMS 通道 11-19）
ChartMode make_sp7k() {
    ChartMode m;
    m.id = "sp7k";
    m.display_name = "SP 7 Keys";
    m.lanes = {k1p[0], k1p[1], k1p[2], k1p[3], k1p[4],
               {0, LaneKind::Scratch, 0}, {0, LaneKind::Pedal, 0},
               k1p[5], k1p[6]};
    return m;
}

// DP：两个 7key 盘（BMS 通道 11-19 + 21-29）
ChartMode make_dp() {
    ChartMode m;
    m.id = "dp";
    m.display_name = "Double Play";
    const auto sp = make_sp7k();
    m.lanes = sp.lanes;
    for (const auto& l : k2p) m.lanes.push_back(l);
    m.lanes.push_back({1, LaneKind::Scratch, 0});
    m.lanes.push_back({1, LaneKind::Pedal, 0});
    return m;
}

// Battle：两个玩家各一 7key 盘（同 DP 通道集，展示为对坐布局）
ChartMode make_battle() {
    ChartMode m;
    m.id = "battle";
    m.display_name = "Battle";
    const auto dp = make_dp();
    m.lanes = dp.lanes;
    return m;
}

// PMS 9key：键1-9，无皿/踏板（BMS 通道 11-19 全为键）
ChartMode make_pms9k() {
    ChartMode m;
    m.id = "pms9k";
    m.display_name = "PMS 9 Keys";
    m.lanes = {k1p9[0], k1p9[1], k1p9[2], k1p9[3], k1p9[4],
               k1p9[5], k1p9[6], k1p9[7], k1p9[8]};
    return m;
}

}  // namespace

std::optional<ChartMode> chart_mode_by_id(std::string_view id) {
    if (id == "sp7k") return make_sp7k();
    if (id == "dp") return make_dp();
    if (id == "battle") return make_battle();
    if (id == "pms9k") return make_pms9k();
    return std::nullopt;
}

}  // namespace beatbench
