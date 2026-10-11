// SPDX-License-Identifier: GPL-3.0-only
// OnsetDetector 实现（见头文件注释）。管线：
// 单声道混音 → 分帧（Hann 窗）→ radix-2 FFT → 幅度谱 → 谱通量（半波整流差分）
// → 候选峰（相对阈值 + 绝对底线 + 局部极大）→ 最小间隔过滤 → onset 秒列表。
//
// 阈值口径（两道门，缺一不可）：
// - 相对门：median(flux) + delta·mean|flux - median|——适应整体响度/密度；
// - 绝对底线：0.1·max(flux) 与 kFluxAbsFloor 取大——拒绝静音段/稳态长音的
//   数值噪声峰（相对门在「整段近似稳态」时会退化到噪声量级，必须由绝对门兜底）。
#include "beatbench/audio/OnsetDetector.hpp"

#include <algorithm>
#include <cmath>
#include <complex>
#include <cstdint>
#include <limits>
#include <numeric>

namespace beatbench::audio {
namespace {

constexpr double kPi = 3.14159265358979323846;
/// 谱通量绝对底线（Hann 窗归一化量级下，可听 onset 的 flux 远高于此；
/// 静音底/稳态数值噪声远低于此）。
constexpr double kFluxAbsFloor = 1e-4;
/// 局部极大判定半径（帧）：onset 帧须为 ±kPeakRadius 帧内的最大通量。
constexpr std::size_t kPeakRadius = 3;

/// 迭代 radix-2 复数 FFT（in-place；n 须为 2 的幂）。
void fft_inplace(std::vector<std::complex<double>>& a) {
    const std::size_t n = a.size();
    // 位反转重排
    for (std::size_t i = 1, j = 0; i < n; ++i) {
        std::size_t bit = n >> 1;
        for (; j & bit; bit >>= 1) j ^= bit;
        j ^= bit;
        if (i < j) std::swap(a[i], a[j]);
    }
    for (std::size_t len = 2; len <= n; len <<= 1) {
        const double ang = -2.0 * kPi / static_cast<double>(len);
        const std::complex<double> wlen(std::cos(ang), std::sin(ang));
        for (std::size_t i = 0; i < n; i += len) {
            std::complex<double> w(1.0, 0.0);
            for (std::size_t k = 0; k < len / 2; ++k) {
                const auto u = a[i + k];
                const auto v = a[i + k + len / 2] * w;
                a[i + k] = u + v;
                a[i + k + len / 2] = u - v;
                w *= wlen;
            }
        }
    }
}

bool is_pow2(int v) { return v > 0 && (v & (v - 1)) == 0; }

/// 中位数（副本排序；空 → 0）。
double median_of(std::vector<double> v) {
    if (v.empty()) return 0.0;
    std::sort(v.begin(), v.end());
    const std::size_t n = v.size();
    if (n % 2 == 1) return v[n / 2];
    return 0.5 * (v[n / 2 - 1] + v[n / 2]);
}

}  // namespace

std::vector<double> detect_onsets(const float* interleaved, std::size_t frameCount,
                                  int channels, double sampleRate,
                                  const OnsetConfig& cfg) {
    if (interleaved == nullptr || frameCount == 0 || channels <= 0 || sampleRate <= 0.0)
        return {};
    if (!is_pow2(cfg.windowSize) || !is_pow2(cfg.hopSize) || cfg.hopSize > cfg.windowSize)
        return {};

    const std::size_t win = static_cast<std::size_t>(cfg.windowSize);
    const std::size_t hop = static_cast<std::size_t>(cfg.hopSize);
    if (frameCount < win) return {};  // 不足一窗：无从谈瞬态

    // Hann 窗（预计算）
    std::vector<double> hann(win);
    for (std::size_t n = 0; n < win; ++n)
        hann[n] = 0.5 * (1.0 - std::cos(2.0 * kPi * static_cast<double>(n) /
                                        static_cast<double>(win - 1)));

    // 分帧 STFT → complex-domain 谱通量（相位预测 + 半波整流）。
    // 为什么不用 bin/频带幅度差分：hop 非信号周期整数倍时，截断正弦的窗内起点相位
    // 逐帧滑移，sin/cos 分量混比改变，整个泄漏模式重排——幅度谱伪峰与真 onset 同
    // 量级（实测 325 vs 492），任何阈值都分不开。相位预测利用「稳态相位线性推进」：
    // 预测偏差 dev = R_i·cos(φ_i - φ̂_i) - R_{i-1}，稳态归零、瞬态大，这是标准
    // complex-domain flux（Dixon 2006）。
    const std::size_t bins = win / 2 + 1;
    const std::size_t frameTotal = (frameCount - win) / hop + 1;
    std::vector<double> flux(frameTotal, 0.0);
    std::vector<std::complex<double>> prev1(bins), prev2(bins);  // 前两帧半谱（相位预测）
    std::vector<std::complex<double>> spec(win);

    for (std::size_t f = 0; f < frameTotal; ++f) {
        const float* base = interleaved + f * hop * static_cast<std::size_t>(channels);
        for (std::size_t n = 0; n < win; ++n) {
            double mono = 0.0;
            for (int c = 0; c < channels; ++c)
                mono += static_cast<double>(base[n * static_cast<std::size_t>(channels) +
                                                 static_cast<std::size_t>(c)]);
            mono /= static_cast<double>(channels);
            spec[n] = std::complex<double>(mono * hann[n], 0.0);
        }
        fft_inplace(spec);
        double fMagSum = 0.0;
        if (f >= 2) {
            for (std::size_t k = 0; k < bins; ++k) {
                const std::complex<double> x = spec[k];
                const double rx = std::abs(x);
                // 相位外推 φ̂ = 2φ_{i-1} - φ_{i-2}；幅度外推 R̂ = 2R_{i-1} - R_{i-2}
                const double dphi = std::remainder(
                    2.0 * std::arg(prev1[k]) - std::arg(prev2[k]) - std::arg(x),
                    2.0 * kPi);
                const double rPred = 2.0 * std::abs(prev1[k]) - std::abs(prev2[k]);
                // 预测偏差 O = X_i - X̂_i 的实部（半波：只累积正向偏离）
                const double dev = rx * std::cos(dphi) - rPred;
                if (dev > 0.0) fMagSum += dev;
            }
        }
        flux[f] = fMagSum;
        prev2 = prev1;
        for (std::size_t k = 0; k < bins; ++k) prev1[k] = spec[k];
    }

    // 阈值：相对门（median + delta·mean 绝对偏差）与绝对底线取大
    const double med = median_of(flux);
    double dev = 0.0;
    for (const double v : flux) dev += std::abs(v - med);
    dev /= static_cast<double>(flux.size());
    double delta = 11.0 - cfg.sensitivity;
    delta = std::clamp(delta, 0.5, 10.0);
    double relative = med + delta * dev;
    const double maxFlux = *std::max_element(flux.begin(), flux.end());
    const double floorLine = std::max(0.1 * maxFlux, kFluxAbsFloor);
    const double threshold = std::max(relative, floorLine);

    // 峰值拾取：过阈值 + 局部极大 → 候选集；再按 flux 降序贪心接受（minGap 内拒绝弱峰）。
    // 顺序接受会让早到的弱峰抢位（相位预测常提前报警）；贪心保证强峰优先。
    struct Candidate {
        std::size_t frame;
        double value;
    };
    std::vector<Candidate> candidates;
    for (std::size_t f = 0; f < frameTotal; ++f) {
        if (flux[f] < threshold) continue;
        bool isPeak = true;
        for (std::size_t k = 1; k <= kPeakRadius && isPeak; ++k) {
            if ((f >= k && flux[f - k] > flux[f]) ||
                (f + k < frameTotal && flux[f + k] > flux[f]))
                isPeak = false;
        }
        if (isPeak) candidates.push_back({f, flux[f]});
    }
    // ceil：用户语义「间隔 < minGapSec 的合并」——帧距 d·hop/sr < minGapSec 的都要拒，
    // 即保留 d ≥ ceil(minGapSec·sr/hop)（round 会在 X=5.17 时放行 5 帧差=58.3ms<60ms）。
    const std::size_t minGapFrames = std::max<std::size_t>(
        1, static_cast<std::size_t>(
               std::ceil(cfg.minGapSec * sampleRate / static_cast<double>(hop) - 1e-9)));
    std::vector<std::size_t> accepted;
    std::sort(candidates.begin(), candidates.end(),
              [](const Candidate& a, const Candidate& b) { return a.value > b.value; });
    for (const auto& c : candidates) {
        bool ok = true;
        for (const std::size_t a : accepted) {
            const std::size_t dist = a > c.frame ? a - c.frame : c.frame - a;
            if (dist < minGapFrames) {
                ok = false;
                break;
            }
        }
        if (ok) accepted.push_back(c.frame);
    }
    std::sort(accepted.begin(), accepted.end());
    std::vector<double> onsets;
    onsets.reserve(accepted.size());
    for (const std::size_t f : accepted)
        onsets.push_back(static_cast<double>(f * hop) / sampleRate);
    return onsets;
}

}  // namespace beatbench::audio
