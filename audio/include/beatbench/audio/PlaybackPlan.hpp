// SPDX-License-Identifier: GPL-3.0-only
// M5 播放计划：谱面 → 每个 note 的（绝对触发秒 + 采样路径）映射（零 Qt）。
//
// 定位：将来 keysound 实时调度/试玩 autoplay 的公共底座（Phase D 预留）；
// M5.1 先建（成本低：每 note 一次 timing.time_us），单测 + 复用 M4 的
// 倒推衔接/变速/9999xxx 测试设施。当前 PCM 播放路线不依赖它（PcmPlayback
// 播渲染混音——倒推/变速已在渲染器内做完）。
//
// 顶点：10 万 note 构建 ≈ 10ms（time_us 内部 map 查找;测量后若 > 50ms 再优化）。
#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "beatbench/audio/AudioPath.hpp"
#include "beatbench/core/Chart.hpp"
#include "beatbench/core/timing/TimingEngine.hpp"

namespace beatbench::audio {

/// 单个 note 的播放计划项。
struct PlaybackNote {
    std::uint32_t measure = 0;
    double pos = 0.0;             ///< 小节内位置（0..1；Rational → double）
    std::uint32_t sampleId = 0;   ///< 引用的 #WAV id（0 = 空音）
    double triggerSec = 0.0;      ///< 绝对触发秒（TimingEngine 口径）
    std::string file;             ///< 采样文件相对路径（samples.file；空 = 未定义）
    std::string resolvedPath;     ///< 解析后的绝对路径（扩展名回退后；空 = 文件缺失）
};

/// 播放计划（有序按触发秒升序；+ triggerSec 单调）。
class PlaybackPlan {
public:
    /// 构建：chart + timing（外部持有）→ 全部 note 的播放计划。
    /// sourceDir = 谱面目录（相对路径解析基准；空 = 路径原样 → 当作绝对）。
    /// return = 计划（可能为空 = 无 note / 出错——调用方按空处理）。
    static PlaybackPlan build(const beatbench::Chart& chart,
                              const beatbench::TimingEngine& timing,
                              const std::string& sourceDir = "");

    /// 构建（宽字符目录版；Windows 日文路径用）。
    static PlaybackPlan build_w(const beatbench::Chart& chart,
                                const beatbench::TimingEngine& timing,
                                const std::wstring& sourceDir = L"");

    /// 时间 t 的调度位置：第一个 triggerSec >= t 的下标（二分；无 → size()）。
    /// 用于 keysound 调度器游标初始定位。
    std::size_t firstIndexAt(double t) const;

    /// 时间 t 前（含）的最后下标（倒推窗口起点；无 → npos）。
    std::size_t lastIndexBefore(double t) const;

    const std::vector<PlaybackNote>& notes() const { return m_notes; }
    std::size_t size() const { return m_notes.size(); }
    bool empty() const { return m_notes.empty(); }

private:
    std::vector<PlaybackNote> m_notes;
};

}  // namespace beatbench::audio
