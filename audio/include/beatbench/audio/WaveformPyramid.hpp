// SPDX-License-Identifier: GPL-3.0-only
// 波形金字塔（M4.3c）：渲染 PCM → min/max 多级降采样，波形显示的数据源。
//
// 定位：把 RenderedAudio（float 交错立体声，可能数千万帧）压缩成
// 「按帧区间查 min/max」的只读结构——编辑器 BGM 轨波形铺底 / 底部总览条
// 每像素列都只做 O(1) 合并查询，不碰原始 PCM（内存 O(n/bucket)）。
//
// 结构：level 0 = kBaseBucket(256) 帧/桶；level L = 2^L × base（逐级 2 桶合并）。
// 每桶存该桶内 **mono 混音**（(L+R)/2，与渲染器求和语义无关——仅显示用）
// 的 min/max。静音段 = {0,0}（调用方跳过绘制）。零 Qt、零依赖、可单测。
#pragma once

#include <cstddef>
#include <memory>
#include <vector>

namespace beatbench::audio {

/// PCM → min/max 金字塔（见文件头注释）。
class WaveformPyramid {
public:
    /// 基础桶大小（帧数；2 的幂——上层按 2 桶合并）。
    static constexpr std::size_t kBaseBucket = 256;

    struct Range {
        float min = 0.0f;
        float max = 0.0f;
    };

    /// 从交错 PCM 构建（channels ∈ {1,2}；立体声 mono 混音 = (L+R)/2）。
    /// sampleRate 仅随附元数据（总览条换算帧→时间用）。幂等：多次调用重建。
    /// frameCount == 0 / nullptr → valid()==false。
    void build(const float* interleaved, std::size_t frameCount, std::size_t channels,
               double sampleRate = 0.0);

    /// 自持 PCM 版（M4.3c 增量重渲染用）：金字塔持 shared_ptr 保活 PCM 缓冲区，
    /// rebuild_range 局部重扫时指针恒有效；调用方替换缓冲区**元素**（不 realloc）
    /// 后调 rebuild_range 即正确。⚠️ 元素赋值不改变缓冲区地址（vector 语义）。
    void build(std::shared_ptr<const std::vector<float>> interleaved,
               std::size_t channels, double sampleRate = 0.0);

    [[nodiscard]] bool valid() const noexcept { return m_frameCount > 0 && !m_levels.empty(); }
    [[nodiscard]] std::size_t frameCount() const noexcept { return m_frameCount; }
    [[nodiscard]] std::size_t channels() const noexcept { return m_channels; }
    [[nodiscard]] double sampleRate() const noexcept { return m_sampleRate; }
    [[nodiscard]] std::size_t levelCount() const noexcept { return m_levels.size(); }
    std::size_t baseBucket() const noexcept { return kBaseBucket; }
    /// level L 的桶大小（帧数）。
    std::size_t bucketSize(std::size_t level) const noexcept { return kBaseBucket << level; }
    /// level L 的桶数（越界 → 0）。
    std::size_t bucketCount(std::size_t level) const noexcept;
    /// level L 第 index 桶（越界/空 → {0,0}）。
    Range bucketAt(std::size_t level, std::size_t index) const noexcept;

    /// [frameLo, frameHi) 的合并 min/max（夹逼到 [0, frameCount]；空区间 → {0,0}）。
    /// 自动选最粗合适层级：期望合并 ≤ ~5 个桶（短区间用 level 0，长区间用高层）。
    /// 显示用近似（桶粒度 = 256 帧 ≈ 5.8ms@44.1k）——波形铺底/总览条足够。
    Range range(std::size_t frameLo, std::size_t frameHi) const noexcept;

    /// M4.3c 增量重渲染：帧区间内 PCM 被替换后，**局部重建**金字塔——
    /// 只重扫受影响桶（level 0）+ 向上逐级合并重算父桶。比全量 rebuild 快
    /// O(1 - dirty/总帧数)（脏区间小则几乎零成本；UI 线程可承受）。
    /// ⚠️ 前提 = 总帧数不变（调用方保证；总长变化的编辑走全量 rebuild）。
    /// frameHi ≤ frameLo = no-op。
    void rebuild_range(std::size_t frameLo, std::size_t frameHi) noexcept;

private:
    /// 内部：无状态重建（指针版核心逻辑；self 版先设 m_pcmSelf 再调用）。
    void build_impl(const float* interleaved, std::size_t frameCount, std::size_t channels,
                    double sampleRate);

    std::size_t m_frameCount = 0;
    std::size_t m_channels = 1;
    double m_sampleRate = 0.0;
    // m_levels[0] = base（256 帧/桶）；m_levels[i+1] 由 m_levels[i] 两桶合并。
    std::vector<std::vector<Range>> m_levels;
    /// 原始 PCM 指针/持有（rebuild_range 局部重扫需要；span 版 non-owning——
    /// 调用方保证存活到下次 build；self 版 shared_ptr 保活）。
    std::shared_ptr<const std::vector<float>> m_pcmSelf;
    std::size_t m_pcmFrames = 0;
    std::size_t m_pcmChannels = 1;
};

}  // namespace beatbench::audio
