// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include <cstdint>

namespace beatbench {

/// 轨道类别。BMS 通道号（11-19/51-59/D1-D9…）只是 codec 层的映射规则，模型层不出现通道号。
enum class LaneKind : std::uint8_t {
    Key,      ///< 普通按键轨，index = 键号（1..9）
    Scratch,  ///< 转盘/皿轨（BMS 16/26）
    Pedal,    ///< 踏板/保留轨（BMS 17/27）
    Bgm,      ///< 背景音轨（BMS ch01）：到达即自动播放，游戏界面不可见、不参与判定
};

/// 抽象轨道：{玩家侧, 类别, 键号}。
/// SP/DP/PMS/Battle 等模式 = ChartMode 配置表中的 Lane 集合（见 ChartMode.hpp）。
struct Lane {
    std::uint8_t player = 0;  // 0=1P, 1=2P, 未来可扩展
    LaneKind kind = LaneKind::Key;
    std::uint8_t index = 1;   // kind==Key 时有效（1..9），其余为 0

    friend bool operator==(const Lane&, const Lane&) = default;
    // 排序（map 键 / 分组用）：player → kind → index
    friend bool operator<(const Lane& a, const Lane& b) {
        if (a.player != b.player) return a.player < b.player;
        if (a.kind != b.kind) return a.kind < b.kind;
        return a.index < b.index;
    }
};

}  // namespace beatbench
