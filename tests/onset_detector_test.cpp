// SPDX-License-Identifier: GPL-3.0-only
// OnsetDetector 单测（M6.5 瞬态检测）：合成音频确定性断言。
// 已知位置的短促衰减 burst → 检出数量一致、位置误差 ≤ 容差。相位预测（complex
// domain）在能量刚进窗时即报警 → 检出点常提前于真值（保住 attack 的正确性质），
// 40ms 容差覆盖提前效应 + 帧距（11.6ms @44.1k）；静音/稳态长音 → 零检出。
#define _USE_MATH_DEFINES
#include <gtest/gtest.h>

#include <cmath>
#include <cstddef>
#include <limits>
#include <vector>

#include "beatbench/audio/OnsetDetector.hpp"

namespace {

constexpr double kSr = 44100.0;

/// 在 dst 的 tSec 处叠加一个 50ms 指数衰减正弦 burst（freq Hz；幅度 amp）。
void addBurst(std::vector<float>& dst, double tSec, double freq, double amp) {
    const int start = static_cast<int>(tSec * kSr);
    const int len = static_cast<int>(0.05 * kSr);
    for (int i = 0; i < len; ++i) {
        const double env = std::exp(-static_cast<double>(i) / (0.015 * kSr));
        const double s =
            amp * env *
            std::sin(2.0 * 3.14159265358979323846 * freq * static_cast<double>(i) / kSr);
        const std::size_t idx = static_cast<std::size_t>(start + i);
        if (idx < dst.size()) dst[idx] += static_cast<float>(s);
    }
}

/// 与真值集合的匹配断言：检出数一致，且每个检出距最近真值 ≤ tolSec。
void expectNear(const std::vector<double>& got, const std::vector<double>& truth, double tolSec) {
    ASSERT_EQ(got.size(), truth.size()) << "检出数量不符";
    for (const double g : got) {
        double best = 1e9;
        for (const double t : truth) best = std::min(best, std::abs(g - t));
        EXPECT_LE(best, tolSec) << "检出 " << g << "s 距最近真值超容差";
    }
}

std::vector<float> stereoFromMono(const std::vector<float>& mono) {
    std::vector<float> st(mono.size() * 2);
    for (std::size_t i = 0; i < mono.size(); ++i) {
        st[2 * i] = mono[i];
        st[2 * i + 1] = mono[i];
    }
    return st;
}

TEST(OnsetDetectorTest, DetectsBurstsAtKnownPositions) {
    std::vector<float> pcm(static_cast<std::size_t>(3.0 * kSr), 0.0f);
    const std::vector<double> truth = {0.5, 1.0, 1.5, 2.0, 2.5};
    for (const double t : truth) addBurst(pcm, t, 880.0, 0.4);
    const auto st = stereoFromMono(pcm);

    const auto onsets = beatbench::audio::detect_onsets(st.data(), st.size() / 2, 2, kSr,
                                                        beatbench::audio::OnsetConfig{});
    expectNear(onsets, truth, 0.040);
}

TEST(OnsetDetectorTest, SilenceYieldsNoOnsets) {
    const std::vector<float> pcm(static_cast<std::size_t>(2.0 * kSr), 0.0f);
    const auto st = stereoFromMono(pcm);
    const auto onsets = beatbench::audio::detect_onsets(st.data(), st.size() / 2, 2, kSr,
                                                        beatbench::audio::OnsetConfig{});
    EXPECT_TRUE(onsets.empty());
}

TEST(OnsetDetectorTest, SteadyToneYieldsNoOnsets) {
    // 非整周期稳态正弦（贴近真实长音）：泄漏随窗位波动，但无瞬态 → 应零检出。
    std::vector<float> pcm(static_cast<std::size_t>(3.0 * kSr));
    for (std::size_t i = 0; i < pcm.size(); ++i)
        pcm[i] = static_cast<float>(
            0.3 * std::sin(2.0 * 3.14159265358979323846 * 440.0 * static_cast<double>(i) / kSr));
    const auto st = stereoFromMono(pcm);
    const auto onsets = beatbench::audio::detect_onsets(st.data(), st.size() / 2, 2, kSr,
                                                        beatbench::audio::OnsetConfig{});
    EXPECT_TRUE(onsets.empty());
}

TEST(OnsetDetectorTest, MinGapMergesCloseBursts) {
    // 相距 20ms 的两个 burst（同帧量级）→ minGapSec=0.12 下合并为一个检出。
    // 注：相位预测使两 burst 的 flux 峰距被拉宽到 ~70ms（帧距 6），因此 minGap
    // 取 0.12s 明确覆盖「合并」语义；取 0.06 时 6 帧距 = 69.7ms > 60ms 本就该分开。
    std::vector<float> pcm(static_cast<std::size_t>(2.0 * kSr), 0.0f);
    addBurst(pcm, 1.0, 880.0, 0.4);
    addBurst(pcm, 1.02, 1320.0, 0.4);
    const auto st = stereoFromMono(pcm);

    beatbench::audio::OnsetConfig cfg;
    cfg.minGapSec = 0.12;
    const auto onsets = beatbench::audio::detect_onsets(st.data(), st.size() / 2, 2, kSr, cfg);
    ASSERT_EQ(onsets.size(), 1u);
    EXPECT_NEAR(onsets[0], 1.0, 0.040);
}

TEST(OnsetDetectorTest, MonoInputDetectsToo) {
    std::vector<float> pcm(static_cast<std::size_t>(2.0 * kSr), 0.0f);
    addBurst(pcm, 1.0, 880.0, 0.4);
    const auto onsets = beatbench::audio::detect_onsets(pcm.data(), pcm.size(), 1, kSr,
                                                        beatbench::audio::OnsetConfig{});
    ASSERT_EQ(onsets.size(), 1u);
    EXPECT_NEAR(onsets[0], 1.0, 0.040);
}

TEST(OnsetDetectorTest, EqualFluxPeaksUseDeterministicOrder) {
    // 平局打破（frame 升序）：等幅周期脉冲的谱通量位级相等时，输出仍确定
    //（minGap 贪心按 frame 升序接受，而非不稳定排序的实现定义顺序）。
    std::vector<float> pcm(static_cast<std::size_t>(3.0 * kSr), 0.0f);
    const std::vector<double> truth = {0.5, 1.0, 1.5, 2.0};
    // 同频同幅、严格等间隔（1 hop 对齐）的 4 个脉冲 → flux 逐峰相等
    for (const double t : truth) addBurst(pcm, t, 440.0, 0.3);
    const auto st = stereoFromMono(pcm);

    const beatbench::audio::OnsetConfig cfg;  // minGap 0.06s → 允许全部 4 峰存活
    const auto a = beatbench::audio::detect_onsets(st.data(), st.size() / 2, 2, kSr, cfg);
    const auto b = beatbench::audio::detect_onsets(st.data(), st.size() / 2, 2, kSr, cfg);
    ASSERT_EQ(a.size(), b.size());
    for (std::size_t i = 0; i < a.size(); ++i) EXPECT_EQ(a[i], b[i]);
    // 全部检出、升序、距真值不超过帧距 + 提前量
    ASSERT_EQ(a.size(), 4u);
    expectNear(a, truth, 0.040);
    for (std::size_t i = 1; i < a.size(); ++i) EXPECT_LT(a[i - 1], a[i]);
}

TEST(OnsetDetectorTest, NonFiniteConfigAndSamplesDoNotEscape) {
    std::vector<float> pcm(static_cast<std::size_t>(2.0 * kSr), 0.0f);
    addBurst(pcm, 1.0, 880.0, 0.4);
    pcm[static_cast<std::size_t>(1.2 * kSr)] = std::numeric_limits<float>::quiet_NaN();
    const auto st = stereoFromMono(pcm);
    const auto onsets = beatbench::audio::detect_onsets(st.data(), st.size() / 2, 2, kSr,
                                                        beatbench::audio::OnsetConfig{});
    for (const double t : onsets) EXPECT_TRUE(std::isfinite(t));

    beatbench::audio::OnsetConfig bad;
    bad.sensitivity = std::numeric_limits<double>::quiet_NaN();
    EXPECT_TRUE(beatbench::audio::detect_onsets(st.data(), st.size() / 2, 2, kSr, bad).empty());
    bad = beatbench::audio::OnsetConfig{};
    bad.minGapSec = std::numeric_limits<double>::infinity();
    EXPECT_TRUE(beatbench::audio::detect_onsets(st.data(), st.size() / 2, 2, kSr, bad).empty());
    bad = beatbench::audio::OnsetConfig{};
    bad.windowSize = 1;
    EXPECT_TRUE(beatbench::audio::detect_onsets(st.data(), st.size() / 2, 2, kSr, bad).empty());
    EXPECT_TRUE(beatbench::audio::detect_onsets(st.data(), st.size() / 2, 2,
                                                std::numeric_limits<double>::quiet_NaN())
                    .empty());
}

TEST(OnsetDetectorTest, InvalidInputsYieldEmpty) {
    const std::vector<float> pcm(4096, 0.0f);
    EXPECT_TRUE(beatbench::audio::detect_onsets(nullptr, 100, 2, kSr).empty());
    EXPECT_TRUE(beatbench::audio::detect_onsets(pcm.data(), 0, 2, kSr).empty());
    EXPECT_TRUE(beatbench::audio::detect_onsets(pcm.data(), pcm.size(), 2, 0.0).empty());
    EXPECT_TRUE(beatbench::audio::detect_onsets(pcm.data(), 100, 2, kSr).empty());  // 不足一窗
    beatbench::audio::OnsetConfig bad;
    bad.windowSize = 1000;  // 非 2 的幂
    EXPECT_TRUE(beatbench::audio::detect_onsets(pcm.data(), pcm.size(), 2, kSr, bad).empty());
}

}  // namespace
