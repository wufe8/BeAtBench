// SPDX-License-Identifier: GPL-3.0-only
// ReferenceTrack 测试（M6.1 参考音轨）：合成 wav 解码 → 统计/金字塔/窗口截取。
// 无设备/无 Qt：解码（miniaudio）+ 金字塔纯逻辑可独立测试（同 audio_test）。
#define _USE_MATH_DEFINES
#include <gtest/gtest.h>

#include <cmath>
#include <cstdio>
#include <filesystem>
#include <string>

#include "beatbench/audio/ReferenceTrack.hpp"

namespace {

/// 写 16 位 PCM wav（单/多声道；正弦波；与 audio_test 同款）。
std::string writeTestWav(const std::string& dir, const std::string& name, int sampleRate,
                         int channels, int frames) {
    const std::string path = dir + "/" + name;
    FILE* f = std::fopen(path.c_str(), "wb");
    EXPECT_TRUE(f != nullptr);
    auto write16 = [&](std::uint16_t v) { std::fwrite(&v, 2, 1, f); };
    auto write32 = [&](std::uint32_t v) { std::fwrite(&v, 4, 1, f); };
    std::fwrite("RIFF", 1, 4, f);
    write32(36 + static_cast<std::uint32_t>(frames) * channels * 2);
    std::fwrite("WAVE", 1, 4, f);
    std::fwrite("fmt ", 1, 4, f);
    write32(16);
    write16(1);  // PCM
    write16(static_cast<std::uint16_t>(channels));
    write32(static_cast<std::uint32_t>(sampleRate));
    write32(static_cast<std::uint32_t>(sampleRate * channels * 2));
    write16(static_cast<std::uint16_t>(channels * 2));
    write16(16);
    std::fwrite("data", 1, 4, f);
    write32(static_cast<std::uint32_t>(frames * channels * 2));
    for (int i = 0; i < frames; ++i) {
        const auto s = static_cast<std::int16_t>(
            std::lround(12000 * std::sin(2.0 * M_PI * 2.0 * i / sampleRate)));
        for (int c = 0; c < channels; ++c) std::fwrite(&s, 2, 1, f);
    }
    std::fclose(f);
    return path;
}

class ReferenceTrackTest : public ::testing::Test {
   protected:
    void SetUp() override {
        m_tmp = std::filesystem::temp_directory_path() / "beatbench_ref_test";
        std::filesystem::create_directories(m_tmp);
    }
    std::filesystem::path m_tmp;
};

TEST_F(ReferenceTrackTest, DecodeAndStats) {
    const std::string path = writeTestWav(m_tmp.string(), "stem.wav", 44100, 2, 44100 * 2);
    beatbench::audio::ReferenceTrack ref;
    ASSERT_TRUE(ref.load(path)) << ref.error();
    EXPECT_TRUE(ref.valid());
    EXPECT_EQ(ref.sampleRate(), 44100.0);
    EXPECT_EQ(ref.channels(), 2);
    EXPECT_EQ(ref.frameCount(), 44100u * 2);
    EXPECT_NEAR(ref.durationSec(), 2.0, 1e-6);
    EXPECT_TRUE(ref.waveform().valid());
    // 金字塔与 PCM 一致：查询整段应有非零 min/max（正弦波）
    const auto r = ref.waveform().range(0, ref.frameCount());
    EXPECT_GT(r.max, 0.2f);
    EXPECT_LT(r.min, -0.2f);
    EXPECT_FALSE(ref.path().empty());
    EXPECT_EQ(ref.error(), "");
}

TEST_F(ReferenceTrackTest, WindowExtraction) {
    const std::string path = writeTestWav(m_tmp.string(), "stem2.wav", 44100, 2, 44100 * 2);
    beatbench::audio::ReferenceTrack ref;
    ASSERT_TRUE(ref.load(path));

    // [0, 1s) → 44100 帧 × 2 声道
    const auto w1 = ref.window(0.0, 1.0);
    EXPECT_EQ(w1.size(), 44100u * 2);
    // [0.5, 1.0) → 22050 帧
    const auto w2 = ref.window(0.5, 1.0);
    EXPECT_EQ(w2.size(), 22050u * 2);
    // 第一帧 = sin(0) ≈ 0（解码 16bit → float：±1 量程）
    EXPECT_NEAR(w1[0], 0.0f, 1e-3);
    // 第四秒越界：夹逼到末尾 → 空（1s 窗口超出 2s 时长后边界）
    EXPECT_TRUE(ref.window(3.0, 4.0).empty());
    // 反向窗口 → 空
    EXPECT_TRUE(ref.window(1.0, 0.5).empty());
}

TEST_F(ReferenceTrackTest, LoadFailureKeepsOld) {
    const std::string good = writeTestWav(m_tmp.string(), "good.wav", 44100, 2, 44100);
    beatbench::audio::ReferenceTrack ref;
    ASSERT_TRUE(ref.load(good));
    const auto oldPcm = ref.pcm();
    ASSERT_EQ(oldPcm->size(), 44100u * 2);

    // 加载坏文件 → 失败且原内容保留（工作台语义友好）
    const auto bad = (m_tmp / "no_such.wav").string();
    EXPECT_FALSE(ref.load(bad));
    EXPECT_FALSE(ref.error().empty());
    EXPECT_TRUE(ref.valid());
    EXPECT_EQ(ref.pcm(), oldPcm);
    EXPECT_NEAR(ref.durationSec(), 1.0, 1e-6);

    // clear → 释放
    ref.clear();
    EXPECT_FALSE(ref.valid());
}

TEST_F(ReferenceTrackTest, SliceFadeAttenuatesEnds) {
    // 1s stereo 恒定 0.5 振幅；fade 0.02s → 首/末 882 帧线性衰减，中段保持 0.5
    const std::size_t frames = 44100;
    std::vector<float> pcm(frames * 2, 0.5f);
    beatbench::audio::ReferenceTrack::apply_slice_fade(pcm, 44100.0, 0.02);
    const std::size_t fadeFrames = static_cast<std::size_t>(std::ceil(44100.0 * 0.02));
    EXPECT_NEAR(pcm[0], 0.0f, 1e-3);                 // 首帧 ≈ 0
    EXPECT_NEAR(pcm[fadeFrames * 2], 0.5f, 1e-3);    // 衰减后满幅
    EXPECT_NEAR(pcm[(frames / 2) * 2], 0.5f, 1e-3);  // 中段不变
    EXPECT_NEAR(pcm[(frames - 1) * 2], 0.0f, 1e-2);  // 末帧 ≈ 0
}

}  // namespace
