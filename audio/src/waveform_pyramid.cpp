// SPDX-License-Identifier: GPL-3.0-only
// WaveformPyramid 实现（见头文件注释）。
#include "beatbench/audio/WaveformPyramid.hpp"

#include <algorithm>
#include <limits>

namespace beatbench::audio {

void WaveformPyramid::build(const float* interleaved, std::size_t frameCount,
                            std::size_t channels, double sampleRate) {
    m_pcmSelf.reset();
    m_pcmFrames = frameCount;
    m_pcmChannels = channels ? channels : 1;
    build_impl(interleaved, frameCount, channels, sampleRate);
}

void WaveformPyramid::build_impl(const float* interleaved, std::size_t frameCount,
                                 std::size_t channels, double sampleRate) {
    m_levels.clear();
    m_frameCount = 0;
    m_channels = channels ? channels : 1;
    m_sampleRate = sampleRate;
    if (!interleaved || frameCount == 0) return;

    // —— level 0：base 桶大小直接扫 ——
    const std::size_t bs = kBaseBucket;
    std::vector<Range> base;
    base.reserve((frameCount + bs - 1) / bs);
    for (std::size_t s = 0; s < frameCount; s += bs) {
        const std::size_t e = std::min(s + bs, frameCount);
        float mn = std::numeric_limits<float>::max();
        float mx = std::numeric_limits<float>::lowest();
        if (m_channels >= 2) {
            for (std::size_t f = s; f < e; ++f) {
                const float mono = (interleaved[2 * f] + interleaved[2 * f + 1]) * 0.5f;
                mn = std::min(mn, mono);
                mx = std::max(mx, mono);
            }
        } else {
            for (std::size_t f = s; f < e; ++f) {
                const float mono = interleaved[f];
                mn = std::min(mn, mono);
                mx = std::max(mx, mono);
            }
        }
        // 全零桶 → {0,0}（静音段显示为无波形，调用方跳过）
        base.push_back({mn, mx});
    }
    m_levels.push_back(std::move(base));
    m_frameCount = frameCount;

    // —— 上级：两桶合并 ——
    while (m_levels.back().size() > 1) {
        const auto& lo = m_levels.back();
        std::vector<Range> up;
        up.reserve((lo.size() + 1) / 2);
        for (std::size_t i = 0; i < lo.size(); i += 2) {
            const Range& a = lo[i];
            const Range& b = (i + 1 < lo.size()) ? lo[i + 1] : a;
            up.push_back({std::min(a.min, b.min), std::max(a.max, b.max)});
        }
        m_levels.push_back(std::move(up));
    }
}

void WaveformPyramid::build(std::shared_ptr<const std::vector<float>> interleaved,
                            std::size_t channels, double sampleRate) {
    m_pcmSelf = std::move(interleaved);
    // 自持版：PCM 由 shared_ptr 保活（rebuild_range 局部重扫恒安全）；
    // frameCount = size()/channels（容错：空 → invalid）
    const std::size_t ch = channels ? channels : 1;
    const std::size_t frames = m_pcmSelf ? m_pcmSelf->size() / ch : 0;
    m_pcmFrames = frames;
    m_pcmChannels = ch;
    build_impl(m_pcmSelf ? m_pcmSelf->data() : nullptr, frames, ch, sampleRate);
}

std::size_t WaveformPyramid::bucketCount(std::size_t level) const noexcept {
    return level < m_levels.size() ? m_levels[level].size() : 0;
}

WaveformPyramid::Range WaveformPyramid::bucketAt(std::size_t level,
                                                 std::size_t index) const noexcept {
    if (level >= m_levels.size() || index >= m_levels[level].size()) return {};
    return m_levels[level][index];
}

WaveformPyramid::Range WaveformPyramid::range(std::size_t frameLo,
                                              std::size_t frameHi) const noexcept {
    if (m_frameCount == 0) return {};
    frameLo = std::min(frameLo, m_frameCount);
    frameHi = std::min(frameHi, m_frameCount);
    if (frameLo >= frameHi) return {};
    // 目标：合并 ≤ ~5 桶 → 选最粗 level L（桶 = 256<<L 帧）使 5×桶 ≥ span；
    // 钳到已有层级上限。短区间（≤5 桶）直接用 level 0。
    const std::size_t span = frameHi - frameLo;
    std::size_t level = 0;
    if (span > 5 * kBaseBucket) {
        std::size_t l = 0;
        while (l + 1 < m_levels.size() && span <= 5 * (kBaseBucket << (l + 1))) ++l;
        level = l;
    }
    const std::size_t bs = kBaseBucket << level;
    const std::size_t b0 = frameLo / bs;
    const std::size_t b1 = (frameHi + bs - 1) / bs;
    Range r{};
    bool any = false;
    for (std::size_t b = b0; b < b1 && b < m_levels[level].size(); ++b) {
        const Range& c = m_levels[level][b];
        if (!any) {
            r = c;
            any = true;
        } else {
            r.min = std::min(r.min, c.min);
            r.max = std::max(r.max, c.max);
        }
    }
    return any ? r : Range{};
}

void WaveformPyramid::rebuild_range(std::size_t frameLo, std::size_t frameHi) noexcept {
    if (m_frameCount == 0) return;
    const float* pcm = m_pcmSelf ? m_pcmSelf->data() : nullptr;
    if (!pcm) return;
    frameLo = std::min(frameLo, m_frameCount);
    frameHi = std::min(frameHi, m_frameCount);
    if (frameLo >= frameHi) return;
    // 只重扫被区间覆盖的 level 0 桶（PCM 直接扫：桶网格按 kBaseBucket 对齐）
    const std::size_t bs = kBaseBucket;
    std::size_t b0 = frameLo / bs;
    std::size_t b1 = (frameHi + bs - 1) / bs;
    auto& base = m_levels[0];
    for (std::size_t b = b0; b < b1 && b < base.size(); ++b) {
        const std::size_t s = b * bs;
        const std::size_t e = std::min(s + bs, m_frameCount);
        float mn = std::numeric_limits<float>::max();
        float mx = std::numeric_limits<float>::lowest();
        if (m_channels >= 2) {
            for (std::size_t f = s; f < e; ++f) {
                const float mono = (pcm[2 * f] + pcm[2 * f + 1]) * 0.5f;
                mn = std::min(mn, mono);
                mx = std::max(mx, mono);
            }
        } else {
            for (std::size_t f = s; f < e; ++f) {
                const float mono = pcm[f];
                mn = std::min(mn, mono);
                mx = std::max(mx, mono);
            }
        }
        base[b] = {mn, mx};
    }
    // 上级传播：从 level 1 起，受影响桶 = [b0/2, (b1+1)/2)（相邻层级桶网格 2x）
    for (std::size_t level = 1; level < m_levels.size(); ++level) {
        auto& up = m_levels[level];
        const auto& lo = m_levels[level - 1];
        const std::size_t ub0 = b0 / 2;
        const std::size_t ub1 = (b1 + 1) / 2;
        for (std::size_t u = ub0; u < ub1 && u < up.size(); ++u) {
            const Range& a = lo[2 * u];
            const Range& b = (2 * u + 1 < lo.size()) ? lo[2 * u + 1] : a;
            up[u] = {std::min(a.min, b.min), std::max(a.max, b.max)};
        }
        b0 = ub0;
        b1 = ub1;
    }
}

}  // namespace beatbench::audio
