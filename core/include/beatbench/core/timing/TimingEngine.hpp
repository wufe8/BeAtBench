// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include <cstdint>
#include <optional>

#include "beatbench/core/Chart.hpp"
#include "beatbench/core/Rational.hpp"

namespace beatbench {

/// 拍位（小节 + 节内位置）。
struct Position {
    std::uint32_t measure = 0;
    Rational pos;
};

/// 小节↔时间 双向换算（对齐稿 02 §4 的时序核心）。
/// 正算：小节时长 = 节拍值 × 4 × (60/BPM)，逐段累加 + STOP 间隙；
/// 逆算：落在 STOP 间隙内的时间映射回 STOP 起点。
/// 内部：事件索引 + 脏区增量失效（BPM/STOP 编辑后局部重算）。
/// 实现计划（M1）；黄金测试锚点：Doppelganger[Eb] 等真实变速谱。
class TimingEngine {
public:
    /// 用谱面事件重建索引（图表结构变更后调用；引擎自行做脏检查）。
    void rebuild(const Chart& chart);

    /// 拍位 → 微秒。
    std::int64_t time_us(Position p) const;

    /// 微秒 → 拍位。
    std::optional<Position> position_at(std::int64_t time_us) const;
};

}  // namespace beatbench
