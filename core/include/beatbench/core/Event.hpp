// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include <cstdint>

#include "beatbench/core/Rational.hpp"

namespace beatbench {

/// 统一事件：所有可定位对象（note/BPM/STOP/节拍/BGA…）都用它承载。
/// measure = 小节号（BMS 为 000-999；模型不设上限），pos = 节内位置（0 ≤ pos < 1）。
/// 约定：容器内按 (measure, pos) 升序存放；绝对时间(μs) 是派生值，由 timing 引擎计算。
template <typename T>
struct Event {
    std::uint32_t measure = 0;
    Rational pos;
    T value;

    friend bool operator<(const Event& a, const Event& b) {
        if (a.measure != b.measure) return a.measure < b.measure;
        return a.pos < b.pos;
    }
    friend bool operator==(const Event& a, const Event& b) {
        return a.measure == b.measure && a.pos == b.pos && a.value == b.value;
    }
};

}  // namespace beatbench
