// SPDX-License-Identifier: GPL-3.0-only
// WaveformPyramid 单测（M4.3c 波形显示数据源）：构建/查询语义。
// 纯内存合成数据，无磁盘/设备依赖，全确定性。
#include <gtest/gtest.h>

#include "beatbench/audio/WaveformPyramid.hpp"

using beatbench::audio::WaveformPyramid;

namespace {

/// 立体声交错 PCM（正弦包络，非全零——min/max 非平凡）。
std::vector<float> makeStereo(std::size_t frames) {
    std::vector<float> pcm(2 * frames);
    for (std::size_t f = 0; f < frames; ++f) {
        const float v = static_cast<float>(std::sin(f * 0.01) * 0.5);
        pcm[2 * f] = v;
        pcm[2 * f + 1] = -v;  // 反相：mono 混音 = 0（验证 (L+R)/2 语义）
    }
    return pcm;
}

}  // namespace

TEST(WaveformPyramid, InvalidInput) {
    WaveformPyramid p;
    EXPECT_FALSE(p.valid());
    p.build(nullptr, 0, 2, 44100.0);
    EXPECT_FALSE(p.valid());
    p.build(nullptr, 100, 2, 44100.0);
    EXPECT_FALSE(p.valid());
    EXPECT_EQ(p.frameCount(), 0u);
}

TEST(WaveformPyramid, BuildAndLevels) {
    const std::size_t frames = 4 * WaveformPyramid::kBaseBucket;  // 4 桶
    WaveformPyramid p;
    p.build(makeStereo(frames).data(), frames, 2, 44100.0);
    ASSERT_TRUE(p.valid());
    EXPECT_EQ(p.frameCount(), frames);
    EXPECT_EQ(p.channels(), 2u);
    EXPECT_DOUBLE_EQ(p.sampleRate(), 44100.0);

    // level 0 = 4 桶；level 1 = 2 桶；level 2 = 1 桶 → 3 级
    ASSERT_GE(p.levelCount(), 3u);
    EXPECT_EQ(p.bucketCount(0), frames / WaveformPyramid::kBaseBucket);
    EXPECT_EQ(p.bucketCount(1), 2u);
    EXPECT_EQ(p.bucketCount(2), 1u);
    EXPECT_EQ(p.bucketSize(0), WaveformPyramid::kBaseBucket);
    EXPECT_EQ(p.bucketSize(1), WaveformPyramid::kBaseBucket * 2);
    // 越界保护
    EXPECT_EQ(p.bucketCount(99), 0u);
    EXPECT_EQ(p.bucketAt(0, 9999).min, 0.0f);
    EXPECT_EQ(p.bucketAt(99, 0).min, 0.0f);
}

TEST(WaveformPyramid, MonoMixSemantics) {
    // 反相立体声：mono = (L+R)/2 = 0 → 桶 {0,0}（静音判定）
    const std::size_t frames = WaveformPyramid::kBaseBucket;
    WaveformPyramid p;
    p.build(makeStereo(frames).data(), frames, 2, 44100.0);
    ASSERT_TRUE(p.valid());
    for (std::size_t i = 0; i < p.bucketCount(0); ++i) {
        const auto r = p.bucketAt(0, i);
        EXPECT_FLOAT_EQ(r.min, 0.0f);
        EXPECT_FLOAT_EQ(r.max, 0.0f);
    }
}

TEST(WaveformPyramid, RangeQueries) {
    // 构造已知桶：frame 区段 0..256 = 常数 +0.4/+0.8；256..512 = -0.6；之后静音
    const std::size_t frames = 4 * WaveformPyramid::kBaseBucket;
    std::vector<float> pcm(2 * frames, 0.0f);
    const auto fill = [&](std::size_t from, std::size_t to, float v) {
        for (std::size_t f = from; f < to; ++f) {
            pcm[2 * f] = v;
            pcm[2 * f + 1] = v;
        }
    };
    fill(0, WaveformPyramid::kBaseBucket, 0.4f);
    fill(WaveformPyramid::kBaseBucket, 2 * WaveformPyramid::kBaseBucket, -0.6f);
    fill(2 * WaveformPyramid::kBaseBucket, 3 * WaveformPyramid::kBaseBucket, 0.8f);

    WaveformPyramid p;
    p.build(pcm.data(), frames, 2, 44100.0);
    ASSERT_TRUE(p.valid());

    // 桶级查询（level 0）
    auto r0 = p.range(0, WaveformPyramid::kBaseBucket);
    EXPECT_FLOAT_EQ(r0.min, 0.4f);
    EXPECT_FLOAT_EQ(r0.max, 0.4f);
    auto r1 = p.range(WaveformPyramid::kBaseBucket, 2 * WaveformPyramid::kBaseBucket);
    EXPECT_FLOAT_EQ(r1.min, -0.6f);
    EXPECT_FLOAT_EQ(r1.max, -0.6f);

    // 跨桶合并（level 0 → 2 桶）
    auto rX = p.range(0, 2 * WaveformPyramid::kBaseBucket);
    EXPECT_FLOAT_EQ(rX.min, -0.6f);
    EXPECT_FLOAT_EQ(rX.max, 0.4f);

    // 高层查询（全曲 → 顶层单桶）
    auto rAll = p.range(0, frames);
    EXPECT_FLOAT_EQ(rAll.min, -0.6f);
    EXPECT_FLOAT_EQ(rAll.max, 0.8f);

    // 静音区间 → {0,0}
    auto rSil = p.range(3 * WaveformPyramid::kBaseBucket, frames);
    EXPECT_FLOAT_EQ(rSil.min, 0.0f);
    EXPECT_FLOAT_EQ(rSil.max, 0.0f);

    // 空区间 / 夹逼
    auto rEmpty = p.range(100, 100);
    EXPECT_FLOAT_EQ(rEmpty.min, 0.0f);
    EXPECT_FLOAT_EQ(rEmpty.max, 0.0f);
    auto rClamped = p.range(frames - 10, frames + 1000);
    EXPECT_FLOAT_EQ(rClamped.min, 0.0f);  // 尾部静音段
    EXPECT_FLOAT_EQ(rClamped.max, 0.0f);
}

TEST(WaveformPyramid, SingleChannel) {
    const std::size_t frames = WaveformPyramid::kBaseBucket * 2;
    std::vector<float> pcm(frames, 0.0f);
    for (std::size_t f = 0; f < frames; ++f) pcm[f] = static_cast<float>(f) / frames - 0.5f;
    WaveformPyramid p;
    p.build(pcm.data(), frames, 1, 48000.0);
    ASSERT_TRUE(p.valid());
    EXPECT_EQ(p.channels(), 1u);
    EXPECT_DOUBLE_EQ(p.sampleRate(), 48000.0);
    auto r = p.range(0, frames);
    // 线性 -0.5..+0.5（两端含端点近似）
    EXPECT_NEAR(r.min, -0.5f, 1e-3f);
    EXPECT_NEAR(r.max, 0.5f - 1.0f / frames, 1e-3f);
}

// M4.3c 增量重渲染：rebuild_range 局部重建（PCM 元素替换 → 桶重扫 + 上级传播）。
TEST(WaveformPyramid, RebuildRange) {
    const std::size_t frames = 8 * WaveformPyramid::kBaseBucket;  // 8 桶
    auto pcm = std::make_shared<std::vector<float>>(2 * frames, 0.0f);
    // 初始：全 0.2（静音判定阈值下非零，但 display 可见）
    for (std::size_t f = 0; f < frames; ++f) { (*pcm)[2 * f] = 0.2f; (*pcm)[2 * f + 1] = 0.2f; }
    WaveformPyramid p;
    p.build(pcm, 2, 44100.0);
    ASSERT_TRUE(p.valid());
    EXPECT_EQ(p.frameCount(), frames);

    auto rAll = p.range(0, frames);
    EXPECT_FLOAT_EQ(rAll.min, 0.2f);
    EXPECT_FLOAT_EQ(rAll.max, 0.2f);

    // 修改第 4 桶（帧 [4*256, 5*256)）→ 0.9
    for (std::size_t f = 4 * WaveformPyramid::kBaseBucket;
         f < 5 * WaveformPyramid::kBaseBucket; ++f) {
        (*pcm)[2 * f] = 0.9f;
        (*pcm)[2 * f + 1] = 0.9f;
    }
    p.rebuild_range(4 * WaveformPyramid::kBaseBucket, 5 * WaveformPyramid::kBaseBucket);

    // 该桶 min/max 更新
    auto rDirty = p.range(4 * WaveformPyramid::kBaseBucket, 5 * WaveformPyramid::kBaseBucket);
    EXPECT_FLOAT_EQ(rDirty.min, 0.9f);
    EXPECT_FLOAT_EQ(rDirty.max, 0.9f);
    // 全曲 max 传播
    rAll = p.range(0, frames);
    EXPECT_FLOAT_EQ(rAll.min, 0.2f);
    EXPECT_FLOAT_EQ(rAll.max, 0.9f);
    // 干净区间不受影响（桶 0）
    auto rClean = p.range(0, WaveformPyramid::kBaseBucket);
    EXPECT_FLOAT_EQ(rClean.min, 0.2f);
    EXPECT_FLOAT_EQ(rClean.max, 0.2f);
    // 空区间 no-op / 越界夹逼
    p.rebuild_range(100, 100);
    p.rebuild_range(frames - 10, frames + 1000);
    EXPECT_TRUE(p.valid());
}

// M4.3c 增量重渲染：无 PCM self 持有（span 版 build 后 rebuild_range no-op 保护）。
TEST(WaveformPyramid, RebuildRangeWithoutSelfPcm) {
    const std::size_t frames = WaveformPyramid::kBaseBucket * 4;
    std::vector<float> pcm(2 * frames, 0.3f);
    WaveformPyramid p;
    p.build(pcm.data(), frames, 2, 44100.0);
    ASSERT_TRUE(p.valid());
    // span 版（无自持）→ rebuild_range 应安全 no-op（指针不可用）
    p.rebuild_range(0, frames);
    auto r = p.range(0, frames);
    EXPECT_FLOAT_EQ(r.min, 0.3f);
    EXPECT_FLOAT_EQ(r.max, 0.3f);
}

// M4.3c 波形显示：真实谱面场景——前 60% 有音（0.64）、后 40% 静音（0.0）。
// 验证 range 分层查询正确（各段 min/max + 静音段 {0,0}）。
TEST(WaveformPyramid, DemoLikePartialAudio) {
    const std::size_t frames = 297675;  // 与 render-demo 同规模
    std::vector<float> pcm(2 * frames, 0.0f);
    for (std::size_t i = 0; i < frames * 6 / 10; ++i) {
        pcm[2 * i] = 0.64f;
        pcm[2 * i + 1] = 0.64f;
    }
    auto sp = std::make_shared<std::vector<float>>(pcm);
    WaveformPyramid p;
    p.build(sp, 2, 44100.0);
    ASSERT_TRUE(p.valid());
    // 全曲：max = 0.64，min = 0.0（有静音段）
    auto rAll = p.range(0, frames);
    EXPECT_FLOAT_EQ(rAll.max, 0.64f);
    EXPECT_FLOAT_EQ(rAll.min, 0.0f);
    // 前 6/10：0.64
    auto r1 = p.range(0, frames * 6 / 10);
    EXPECT_FLOAT_EQ(r1.max, 0.64f);
    // 每 1/8 段：0-4 含音频边界（音频段 [0,0.6)；seg4=[0.5,0.625) 部分有音 → max=0.64）、
    // 5-7 静音
    for (int k = 0; k < 8; ++k) {
        auto r = p.range(k * frames / 8, (k + 1) * frames / 8);
        if (k <= 4) {
            EXPECT_FLOAT_EQ(r.max, 0.64f) << "seg" << k;
        } else {
            EXPECT_FLOAT_EQ(r.min, 0.0f) << "seg" << k;
            EXPECT_FLOAT_EQ(r.max, 0.0f) << "seg" << k;
        }
    }
}
