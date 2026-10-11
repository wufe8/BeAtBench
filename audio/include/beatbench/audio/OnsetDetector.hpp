// SPDX-License-Identifier: GPL-3.0-only
// 瞬态（onset）检测（M6.5 切音工作台 audio 源）：从参考音频 PCM 自动找切分点。
//
// 定位（doc/02 §3 切片位置来源第三条「瞬态检测」）：手头只有音频 stem、没有 MIDI
// 时的自动切分。算法 = 经典管线：STFT（Hann 窗 + radix-2 FFT）→ 谱通量
// （spectral flux，半波整流差分）→ 相对阈值 + 绝对底线 + 局部极大峰值拾取 →
// 最小间隔过滤。输出 onset 时间（秒，升序），交桥层落手动切分点集合。
//
// 零 Qt、零依赖（FFT 内聚实现）；纯函数、线程安全（无全局状态）；不抛异常，
// 参数非法 → 空表。确定性：同输入必同输出（单测以合成音频断言位置误差）。
#pragma once

#include <cstddef>
#include <vector>

namespace beatbench::audio {

/// 检测配置（UI 只暴露 sensitivity / minGapSec；窗参数内部固定、留扩展）。
struct OnsetConfig {
    /// 敏感度 1-10：越大越敏感（阈值倍数 delta = clamp(11 - sensitivity, 0.5, 10)）。
    double sensitivity = 5.0;
    /// 相邻 onset 最小间隔（秒；音乐 keysound 场景防同一次打击多检出）。
    double minGapSec = 0.06;
    /// STFT 窗长（样本；2 的幂）。44.1kHz 下 2048 ≈ 46ms、频率分辨率 ~21.5Hz。
    int windowSize = 2048;
    /// 帧距（样本）。44.1kHz 下 512 ≈ 11.6ms，决定 onset 时间粒度。
    int hopSize = 512;
};

/// onset 检测：STFT 谱通量 + 峰值拾取。
/// 输入交错 PCM（float32；channels 声道交错；多声道按帧平均混为单声道）。
/// 返回 onset 时间（秒，升序；帧起点对齐，粒度 ≈ hop/sampleRate）。
/// sampleRate <= 0 / frameCount == 0 / 非法窗参数 → 空表。
std::vector<double> detect_onsets(const float* interleaved, std::size_t frameCount,
                                  int channels, double sampleRate,
                                  const OnsetConfig& cfg = {});

}  // namespace beatbench::audio
