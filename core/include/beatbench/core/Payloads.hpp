// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include <cstdint>
#include <optional>

#include "beatbench/core/Lane.hpp"
#include "beatbench/core/SampleRef.hpp"

namespace beatbench {

/// note 类型。特殊属性放这里而非 Lane（Lane 只管轨道身份）。
enum class NoteKind : std::uint8_t {
    Normal,    ///< 普通 note（含 LN 头尾）
    Landmine,  ///< 地雷（BMS D1-D9 / E1-E9）
    // 预留：Hidden（隐形/WW）等按需加入
};

/// 音符。LN：头尾是两条独立 Event，通过 ln_pair 互指（存 notes 容器下标）。
/// lane = 轨道身份（由 codec 的通道映射表解析得出，格式无关）；
/// 语义与 LNTYPE 1/2 的文本表示解耦——转换由 bms codec 负责（对齐稿 02 §4.1）。
struct Note {
    Lane lane;
    SampleRef sample;
    NoteKind kind = NoteKind::Normal;
    std::optional<std::uint32_t> ln_pair;  ///< 配对 note 的下标（LN 头<->尾）

    friend bool operator==(const Note&, const Note&) = default;
};

/// BPM 事件（BMS ch03 内联值或 ch08/#BPMxx 引用解析后的落值）。
struct Bpm {
    double value = 130.0;  ///< 可为小数；负数按规范宽容处理（lint 告警）

    friend bool operator==(const Bpm&, const Bpm&) = default;
    friend bool operator<(const Bpm& a, const Bpm& b) { return a.value < b.value; }
};

/// STOP 事件。模型存微秒；BMS 的 #STOPxx n → n/192 秒由 codec 换算。
struct Stop {
    std::int64_t duration_us = 0;

    friend bool operator==(const Stop&, const Stop&) = default;
    friend bool operator<(const Stop& a, const Stop& b) {
        return a.duration_us < b.duration_us;
    }
};

/// 节拍事件（BMS ch02：每小节拍数，4/4 = 4；0.5 = 半拍小节）。
/// 小节时长 = beats × 60 / BPM（由 timing 引擎按 BPM 分段积分）。
struct MeasureLen {
    double beats = 4.0;

    friend bool operator==(const MeasureLen&, const MeasureLen&) = default;
    friend bool operator<(const MeasureLen& a, const MeasureLen& b) {
        return a.beats < b.beats;
    }
};

/// BGA 对象（#BMPxx 引用 + 图层 + 不透明度，最小骨架；ARGB 滤镜后续加）。
struct Bga {
    SampleRef image;
    std::int32_t layer = 0;      ///< 0=base 1=poor 2=layer 3=layer2
    std::uint8_t opacity = 255;  ///< 0-255，255=不透明

    friend bool operator==(const Bga&, const Bga&) = default;
    friend bool operator<(const Bga& a, const Bga& b) {
        if (a.image.id != b.image.id) return a.image.id < b.image.id;
        if (a.layer != b.layer) return a.layer < b.layer;
        return a.opacity < b.opacity;
    }
};

}  // namespace beatbench
