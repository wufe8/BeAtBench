// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include <cstdint>
#include <memory>
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
/// 内部：事件索引（measure → BPM 分段/STOP 列表）；编辑期脏区增量失效留待 GUI 阶段，
/// M1 采用全量 rebuild（10 万 note 谱 < 2s 验收线仍有充足余量）。
/// 黄金测试锚点：Doppelganger[Eb] 等真实变速谱（见 tests/bms_codec_test.cpp）。
class TimingEngine {
public:
    TimingEngine();
    ~TimingEngine();
    TimingEngine(TimingEngine&&) noexcept;
    TimingEngine& operator=(TimingEngine&&) noexcept;
    TimingEngine(const TimingEngine&) = delete;
    TimingEngine& operator=(const TimingEngine&) = delete;

    /// 用谱面事件重建索引（图表结构变更后调用）。
    void rebuild(const Chart& chart);

    /// 拍位 → 微秒。
    std::int64_t time_us(Position p) const;

    /// 微秒 → 拍位（时间超出谱面末尾时返回末尾附近位置）。
    std::optional<Position> position_at(std::int64_t time_us) const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace beatbench
