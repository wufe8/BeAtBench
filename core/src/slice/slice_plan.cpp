// SPDX-License-Identifier: GPL-3.0-only
// 切片位置规划实现（M6.2）。纯计算；见 Slice.hpp 头注释。
#include "beatbench/core/slice/Slice.hpp"

#include <algorithm>
#include <cmath>
#include <iterator>
#include <limits>
#include <map>

namespace beatbench::slice {

SlicePlan plan_from_grid(const GridConfig& cfg) {
    SlicePlan plan;
    plan.durationSec = cfg.durationSec;

    const double cell = 60.0 / cfg.bpm / static_cast<double>(cfg.subdivision);
    if (!std::isfinite(cell) || cell <= 0.0) {
        plan.warnings.push_back("网格无效（bpm/细分须为正有限值）");
        return plan;
    }

    // 无时长上限：按 1 小时生成（防无界循环；GUI 总会传 durationSec）
    double limit = cfg.durationSec;
    if (limit <= 0.0) {
        limit = 3600.0;
        plan.warnings.push_back("未提供时长，网格按前 3600 秒生成");
    }

    // 边界序列：t = offset, offset+cell, …（负 offset：切片起点夹到 0）
    // 每块边界 = 一个切片 [t, t+cell)（末尾夹逼到 durationSec）
    double t = cfg.offsetSec;
    while (t < limit && plan.slices.size() < 10000000) {
        Slice s;
        s.startSec = std::max(0.0, t);
        s.endSec = std::min(t + cell, limit);
        if (s.endSec > s.startSec) {
            s.kind = "grid";
            s.index = static_cast<int>(plan.slices.size());
            plan.slices.push_back(std::move(s));
        }
        t += cell;
        if (!std::isfinite(t)) break;
    }
    return plan;
}

SlicePlan plan_from_midi(const midi::MidiFile& midi, double offsetSec, // NOLINT(readability-function-cognitive-complexity)——MIDI 切片规划：通道聚合与边界分支
                         double durationSec, bool extendToNextOnset) {
    SlicePlan plan;
    plan.durationSec = durationSec;

    // A 模式（默认，2026-09 用户）：与手动切片一致——只标起始，右边界 = 下一起始/音频末尾。
    // 同一起始多 note（和弦）合并一片（noteCount 计数）；末片 = 最后起始 → durationSec
    // （未知时长 → 最大 note 结束）。湿轨重放无缝（连续重建），且无重复片。
    if (extendToNextOnset) {
        std::map<double, Slice> byOnset;  // 起始（含 offset）→ 聚合片
        double maxEnd = 0.0;
        for (const auto& n : midi.notes) {
            const double on = n.startSec + offsetSec;
            const double off = n.endSec + offsetSec;
            maxEnd = std::max(maxEnd, off);
            auto it = byOnset.find(on);
            if (it == byOnset.end()) {
                Slice s;
                s.startSec = on;
                s.kind = "midi";
                s.note = n.pitch;
                s.startTick = n.startTick;
                it = byOnset.emplace(on, s).first;
            }
            ++it->second.noteCount;
        }
        const double endAll = (durationSec > 0.0) ? durationSec : maxEnd;
        for (auto it = byOnset.begin(); it != byOnset.end(); ++it) {
            Slice s = it->second;
            const auto nx = std::next(it);
            s.endSec = (nx != byOnset.end()) ? nx->first : endAll;
            if (durationSec > 0.0 && s.startSec >= durationSec) continue;  // 起点越界 → 丢弃
            if (durationSec > 0.0 && s.endSec > durationSec) s.endSec = durationSec;
            if (s.endSec <= s.startSec) continue;
            s.index = static_cast<int>(plan.slices.size());
            plan.slices.push_back(std::move(s));
        }
        return plan;
    }

    // 历史行为：每个配对音符 = 一片 [on, off)（和弦产生同起始多片，2026-09 起默认不再走这）
    for (const auto& n : midi.notes) {
        Slice s;
        s.index = static_cast<int>(plan.slices.size());
        s.startSec = n.startSec + offsetSec;
        s.endSec = n.endSec + offsetSec;
        s.kind = "midi";
        s.note = n.pitch;
        s.noteCount = 1;
        s.startTick = n.startTick;
        if (durationSec > 0.0) {
            if (s.startSec >= durationSec) continue;  // 起点已越界 → 丢弃
            if (s.endSec > durationSec) s.endSec = durationSec;
        }
        if (s.endSec <= s.startSec) continue;
        plan.slices.push_back(std::move(s));
    }
    return plan;
}

}  // namespace beatbench::slice
