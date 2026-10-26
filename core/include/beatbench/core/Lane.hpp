// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include <cstdint>
#include <string>

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
    std::uint8_t index = 1;  // kind==Key 时有效（1..9），其余为 0

    friend bool operator==(const Lane&, const Lane&) = default;
    // 排序（map 键 / 分组用）：player → kind → index
    friend bool operator<(const Lane& a, const Lane& b) {
        if (a.player != b.player) return a.player < b.player;
        if (a.kind != b.kind) return a.kind < b.kind;
        return a.index < b.index;
    }
};

/// LaneKind → 协议字符串（JSON 的 lane.kind / 列 key 格式）：key/scratch/pedal/bgm。
constexpr const char* lane_kind_str(LaneKind k) {
    switch (k) {
        case LaneKind::Scratch:
            return "scratch";
        case LaneKind::Pedal:
            return "pedal";
        case LaneKind::Bgm:
            return "bgm";
        case LaneKind::Key:
            break;
    }
    return "key";
}

/// lane → 稳定列 key："lane:<player>:<kind>:<index>"（如 lane:0:key:1、lane:1:scratch:0）。
/// 协议 clipboard.applyPattern 的 hidden_lanes 元素、视图列头（ChartViewItem）、
/// 快速摆音（QuickPlaceController）三处共用的单一格式源。
inline std::string lane_key_string(const Lane& lane) {
    const char* kind = lane_kind_str(lane.kind);
    return "lane:" + std::to_string(static_cast<int>(lane.player)) + ":" + kind + ":" +
           std::to_string(static_cast<int>(lane.index));
}

}  // namespace beatbench
