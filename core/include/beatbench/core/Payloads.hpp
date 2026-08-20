// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include <cstdint>
#include <optional>

#include "beatbench/core/SampleRef.hpp"

namespace beatbench {

/// note 类型。特殊属性放这里而非 Lane（Lane 只管轨道身份）。
enum class NoteKind : std::uint8_t {
    Normal,    ///< 普通 note（含 LN 头尾）
    Landmine,  ///< 地雷（BMS D1-D9 / E1-E9）
    // 预留：Hidden（隐形/WW）等按需加入
};

/// 音符。LN：头尾是两条独立 Event，通过 ln_pair 互指（存 notes 容器下标）。
/// 语义与 LNTYPE 1/2 的文本表示解耦——转换由 bms codec 负责（对齐稿 02 §4.1）。
struct Note {
    SampleRef sample;
    NoteKind kind = NoteKind::Normal;
    std::optional<std::uint32_t> ln_pair;  ///< 配对 note 的下标（LN 头<->尾）
};

/// BPM 事件（BMS ch03 内联值或 ch08/#BPMxx 引用解析后的落值）。
struct Bpm {
    double value = 130.0;  ///< 可为小数；负数按规范宽容处理（lint 告警）
};

/// STOP 事件。模型存微秒；BMS 的 #STOPxx n → n/192 秒由 codec 换算。
struct Stop {
    std::int64_t duration_us = 0;
};

/// 节拍事件（BMS ch02：当前小节拍数，默认 1.0 = 4/4，即每小节 4 个 4 分拍）。
struct MeasureLen {
    double beats = 1.0;
};

/// BGA 对象（#BMPxx 引用 + 图层 + 不透明度，最小骨架；ARGB 滤镜后续加）。
struct Bga {
    SampleRef image;
    std::int32_t layer = 0;      ///< 0=base 1=poor 2=layer 3=layer2
    std::uint8_t opacity = 255;  ///< 0-255，255=不透明
};

}  // namespace beatbench
