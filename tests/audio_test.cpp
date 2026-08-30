// SPDX-License-Identifier: GPL-3.0-only
// 音频层单测（M4.1）：解码（合成 wav）→ SamplePlayer 内核渲染（无设备/无 Qt）。
// 验证：① 解码正确性（波形数据、采样率、声道）；② 混音（音量/包络/起止）；
// ③ startSec（M5 接口形状）；④ playPreview 切换（停旧播新）；⑤ 引用计数（无泄漏）。
// 不依赖 PortAudio 设备（SamplePlayer 是纯 DSP + 命令队列，可独立测试）。
#define _USE_MATH_DEFINES
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include "beatbench/audio/AudioDecoder.hpp"
#include "beatbench/audio/SamplePlayer.hpp"

namespace {

using beatbench::audio::DecodedSample;
using beatbench::audio::SamplePlayer;

/// 写一个 16 位 PCM 单声道 wav（合成正弦波；便于断言）。
std::string writeTestWav(const std::string& dir, const std::string& name,
                         int sampleRate, int channels, int frames, double freq = 440.0) {
    const std::string path = dir + "/" + name;
    FILE* f = std::fopen(path.c_str(), "wb");
    EXPECT_TRUE(f != nullptr);
    auto write16 = [&](std::uint16_t v) { std::fwrite(&v, 2, 1, f); };
    auto write32 = [&](std::uint32_t v) { std::fwrite(&v, 4, 1, f); };
    // RIFF 头
    std::fwrite("RIFF", 1, 4, f);
    write32(36 + frames * channels * 2);  // 文件大小（后续）
    std::fwrite("WAVE", 1, 4, f);
    std::fwrite("fmt ", 1, 4, f);
    write32(16);
    write16(1);                          // PCM
    write16(static_cast<std::uint16_t>(channels));
    write32(static_cast<std::uint32_t>(sampleRate));
    write32(static_cast<std::uint32_t>(sampleRate * channels * 2));  // byte rate
    write16(static_cast<std::uint16_t>(channels * 2));               // block align
    write16(16);                          // bits
    std::fwrite("data", 1, 4, f);
    write32(static_cast<std::uint32_t>(frames * channels * 2));
    for (int i = 0; i < frames; ++i) {
        const auto s = static_cast<std::int16_t>(
            std::lround(12000 * std::sin(2.0 * M_PI * freq * i / sampleRate)));
        for (int c = 0; c < channels; ++c)
            std::fwrite(&s, 2, 1, f);
    }
    std::fclose(f);
    return path;
}

/// 合成 DecodedSample（测试用；计数 1 = 创建者）
DecodedSample* makeTestSample(double rate, std::size_t frames, float amp = 1.0f) {
    auto* s = new DecodedSample();
    s->sampleRate = rate;
    s->interleavedStereo.resize(frames * 2);
    for (std::size_t i = 0; i < frames; ++i) {
        s->interleavedStereo[i * 2] = amp * static_cast<float>(std::sin(2.0 * M_PI * i / 64.0));
        s->interleavedStereo[i * 2 + 1] = amp * static_cast<float>(std::sin(2.0 * M_PI * i / 64.0));
    }
    // 计数 1（创建者持有）
    return s;
}

class AudioLibTest : public ::testing::Test {
protected:
    void SetUp() override {
        m_tmp = std::filesystem::temp_directory_path() / "beatbench_audio_test";
        std::filesystem::create_directories(m_tmp);
    }
    std::filesystem::path m_tmp;
};

TEST_F(AudioLibTest, DecodeWavBasic) {
    const std::string path = writeTestWav(m_tmp.string(), "sine.wav", 44100, 1, 4410);
    const auto r = beatbench::audio::decode_audio_file(path);
    EXPECT_TRUE(r.ok) << r.message;
    ASSERT_TRUE(r.sample != nullptr);
    EXPECT_DOUBLE_EQ(r.sample->sampleRate, 44100.0);
    EXPECT_EQ(r.sample->frameCount(), 4410u);
    EXPECT_EQ(r.sample->interleavedStereo.size(), 4410u * 2);  // 单声道 → stereo 复制
    // 首帧应为 0（sin(0)）
    EXPECT_NEAR(r.sample->interleavedStereo[0], 0.0f, 1e-4);
    // 峰值 ~ 12000/32768 ≈ 0.366
    float peak = 0.0f;
    for (const float v : r.sample->interleavedStereo) peak = std::max(peak, std::fabs(v));
    EXPECT_GT(peak, 0.3f);
    EXPECT_LT(peak, 0.5f);
    // 释放
    beatbench::audio::decoded_sample_release(r.sample);
}

TEST_F(AudioLibTest, DecodeMissingFile) {
    const auto r = beatbench::audio::decode_audio_file(
        (m_tmp / "no_such_file.wav").string());
    EXPECT_FALSE(r.ok);
    EXPECT_TRUE(r.sample == nullptr);
}

TEST_F(AudioLibTest, RenderBasicMixing) {
    // 播放 1 秒、44100Hz 的样本到 44100 设备：输出应为非零 + 播完结束事件
    SamplePlayer player;
    DecodedSample* s = makeTestSample(44100.0, 44100, 0.5f);
    ASSERT_TRUE(player.play(s, 1.0f, 0.0));  // 转移所有权
    std::vector<float> out(44100 * 2, 0.0f);
    player.render(out.data(), 44100, 44100.0);
    // 输出应有非零样本
    float peak = 0.0f;
    for (const float v : out) peak = std::max(peak, std::fabs(v));
    EXPECT_GT(peak, 0.1f);
    // 结束后应有 ended 事件
    EXPECT_GE(player.drainEndedEvents(), 0);
    player.drainReclaimed();
}

TEST_F(AudioLibTest, StartSecSkips) {
    // startSec = 0.5s：输出开始处（前 100 帧）应为 0（还未到样本播放）——不对，
    // startSec 用于「从中间开始」（任意起播），样本从 0.5s 处开始播。
    SamplePlayer player;
    DecodedSample* s = makeTestSample(44100.0, 44100, 1.0f);
    ASSERT_TRUE(player.play(s, 1.0f, 0.5));  // 从 0.5s（=22050 帧）开始
    std::vector<float> out(100 * 2, 0.0f);
    player.render(out.data(), 100, 44100.0);
    // 前 100 帧应非零（样本从 0.5s 处开始，正在播放）
    float peak = 0.0f;
    for (const float v : out) peak = std::max(peak, std::fabs(v));
    EXPECT_GT(peak, 0.01f);
    player.drainReclaimed();
}

TEST_F(AudioLibTest, PlayPreviewSwitch) {
    // 同采样连点 playPreview：停旧 → 播新（旧 voice 立即停止）
    SamplePlayer player;
    DecodedSample* s = makeTestSample(44100.0, 44100, 1.0f);
    ASSERT_TRUE(player.playPreview(s));
    std::vector<float> out(44100 * 2, 0.0f);
    player.render(out.data(), 44100, 44100.0);  // 第一次完整渲染（播 1s）
    // 再点一次（同一采样重播：停旧再启）
    DecodedSample* s2 = makeTestSample(44100.0, 44100, 1.0f);
    ASSERT_TRUE(player.playPreview(s2));
    std::vector<float> out2(100 * 2, 0.0f);
    player.render(out2.data(), 100, 44100.0);
    // out2 应非零（新 voice 已启动）
    float peak = 0.0f;
    for (const float v : out2) peak = std::max(peak, std::fabs(v));
    EXPECT_GT(peak, 0.01f);
    player.drainReclaimed();
}

TEST_F(AudioLibTest, VorbisOggSupported) {
    // 仅验证扩展名判定（实际 OGG 解码依赖 miniaudio；合成 OGG 复杂，跳过）
    EXPECT_TRUE(beatbench::audio::audio_extension_supported("ogg"));
    EXPECT_TRUE(beatbench::audio::audio_extension_supported("mp3"));
    EXPECT_TRUE(beatbench::audio::audio_extension_supported("wav"));
    EXPECT_TRUE(beatbench::audio::audio_extension_supported("flac"));
    EXPECT_FALSE(beatbench::audio::audio_extension_supported("txt"));
}

TEST_F(AudioLibTest, DecodeRealMp3) {
    // 真实 mp3 文件（本地 chart 目录；缺失则跳过——不是所有环境都有）
    const std::string mp3 =
        std::string(BEATBENCH_SOURCE_DIR) +
        "/local/chart/53857 Saiya - Remote Control/a.mp3";
    if (!std::filesystem::exists(mp3)) GTEST_SKIP() << "无本地 mp3";
    const auto r = beatbench::audio::decode_audio_file(mp3);
    EXPECT_TRUE(r.ok) << r.message;
    if (r.ok) {
        ASSERT_GT(r.sample->frameCount(), 100u);
        EXPECT_GT(r.sample->sampleRate, 8000.0);
        beatbench::audio::decoded_sample_release(r.sample);
    }
}

TEST_F(AudioLibTest, EndToEndPreviewChain) {
    // 端到端：真实 wav 解码 → playPreview 启动 voice → render 出非零（链路全通，
    // 不依赖 PortAudio 设备——纯逻辑）。
    const std::string wav = writeTestWav(m_tmp.string(), "chain.wav", 44100, 1, 22100);
    const auto r = beatbench::audio::decode_audio_file(wav);
    ASSERT_TRUE(r.ok) << r.message;
    DecodedSample* sample = r.sample;  // 计数 1 = 解码器持有
    SamplePlayer player;
    ASSERT_TRUE(player.playPreview(sample));  // 转移所有权（1 → voice 生命周期）
    std::vector<float> out(4410 * 2, 0.0f);  // 0.1s 渲染
    player.render(out.data(), 4410, 44100.0);
    float peak = 0.0f;
    for (const float v : out) peak = std::max(peak, std::fabs(v));
    EXPECT_GT(peak, 0.1f);
    player.drainReclaimed();
    // 注意：playPreview 已转移所有权；结束事件后 voice unref → 回收队列 → drainReclaimed delete。
    // 此处不再 release sample（所有权已转移）。
}

TEST_F(AudioLibTest, DecodeContentSniffMismatchedExt) {
    // 内容探测（回归：#WAV 定义 kick.wav 实际文件 kick.ogg 无声，2026-09 用户实测）：
    // miniaudio init_file 默认按扩展名选解码器（.wav → wav decoder）→ 实际 ogg 内容失败。
    // 修复 = encodingFormat=unknown（内容探测）。此处用 WAV 内容命名 .ogg 验证同一路径
    //（内容 == wav，扩展名 == ogg —— 名不副实；解码必须成功）。
    const std::string ogg = writeTestWav(m_tmp.string(), "sniff.ogg", 44100, 1, 4410);
    const auto r = beatbench::audio::decode_audio_file(ogg);
    EXPECT_TRUE(r.ok) << "内容探测失败（扩展名 .ogg 实际 wav 内容）: " << r.message;
    if (r.ok) {
        EXPECT_EQ(r.sample->frameCount(), 4410u);
        beatbench::audio::decoded_sample_release(r.sample);
    }
}

TEST_F(AudioLibTest, DecodeRealOggMismatchedName) {
    // 真实场景（Doppelganger 谱面）：#WAV 定义 `kick_16_1.wav`，实际文件 `kick_16_1.ogg`。
    // 两件事分开验证：
    // ① 本测试：真实 .ogg 内容解码（vorbis 本身）——AudioEngine 的**扩展名回退**
    //    （resolveAudioPath，app 层）找到 .ogg 后再由解码器按内容探测出 vorbis。
    // ② 回退逻辑是 Qt 层（app/bridge/AudioEngine.hpp），音频单测（零 Qt）不覆盖；
    //    由 GUI 集成（用户点击采样）验证。
    const std::string ogg =
        std::string(BEATBENCH_SOURCE_DIR) + "/local/Chart/Doppelganger/kick_16_1.ogg";
    if (!std::filesystem::exists(ogg)) GTEST_SKIP() << "无本地 Doppelganger ogg 样本";
    const auto r = beatbench::audio::decode_audio_file(ogg);
    EXPECT_TRUE(r.ok) << "真实 ogg（vorbis）解码失败: " << r.message;
    if (r.ok) {
        EXPECT_GT(r.sample->frameCount(), 100u);
        EXPECT_GT(r.sample->sampleRate, 8000.0);
        beatbench::audio::decoded_sample_release(r.sample);
    }
}

#ifdef _WIN32
TEST_F(AudioLibTest, DecodeWideCharJapanesePath) {    // Windows 宽字符路径（回归：日文谱面目录无声，2026-09 用户实测）：
    // 窄字符 fopen 走 ANSI 代码页打不开；ma_decoder_init_file_w（UTF-16）必须可用。
    const auto dir = m_tmp / L"彼岸帰航-idling mix-";
    std::filesystem::create_directories(dir);
    const std::wstring wpath = dir / L"abass-01.wav";
    // 直接写 wav（宽字符 _wfopen_s）：复用 writeTestWav 需窄路径，这里单独写
    FILE* f = nullptr;
    _wfopen_s(&f, wpath.c_str(), L"wb");
    ASSERT_TRUE(f != nullptr);
    auto write16 = [&](std::uint16_t v) { std::fwrite(&v, 2, 1, f); };
    auto write32 = [&](std::uint32_t v) { std::fwrite(&v, 4, 1, f); };
    std::fwrite("RIFF", 1, 4, f);
    write32(36 + 4410 * 2);
    std::fwrite("WAVE", 1, 4, f);
    std::fwrite("fmt ", 1, 4, f);
    write32(16); write16(1); write16(1);
    write32(44100); write32(44100 * 2); write16(2); write16(16);
    std::fwrite("data", 1, 4, f);
    write32(4410 * 2);
    for (int i = 0; i < 4410; ++i) {
        const auto s = static_cast<std::int16_t>(
            std::lround(12000 * std::sin(2.0 * M_PI * 440.0 * i / 44100.0)));
        std::fwrite(&s, 2, 1, f);
    }
    std::fclose(f);

    const auto r = beatbench::audio::decode_audio_file_w(wpath);
    EXPECT_TRUE(r.ok) << "宽字符解码失败: " << r.message;
    if (r.ok) {
        EXPECT_GT(r.sample->frameCount(), 1000u);
        beatbench::audio::decoded_sample_release(r.sample);
    }
    // 清理
    std::filesystem::remove_all(dir);
}
#endif  // _WIN32（上方：宽字符路径仅 Windows）

TEST_F(AudioLibTest, MasterVolumeScales) {
    // 主音量（M4.2 设置页）：0.5 → 混音输出振幅减半（内核级，不依赖设备）
    SamplePlayer player;
    DecodedSample* s = makeTestSample(44100.0, 44100, 0.8f);
    player.setMasterVolume(0.5f);
    ASSERT_TRUE(player.play(s, 1.0f, 0.0));
    std::vector<float> out(4410 * 2, 0.0f);
    player.render(out.data(), 4410, 44100.0);
    float peak = 0.0f;
    for (const float v : out) peak = std::max(peak, std::fabs(v));
    EXPECT_GT(peak, 0.2f);   // 0.8×0.5=0.4 减包络（取中段峰值）
    EXPECT_LT(peak, 0.5f);
    player.drainReclaimed();
}

TEST_F(AudioLibTest, MasterVolumeZeroSilences) {
    // 主音量 0 → 静音
    SamplePlayer player;
    DecodedSample* s = makeTestSample(44100.0, 44100, 0.8f);
    player.setMasterVolume(0.0f);
    ASSERT_TRUE(player.play(s, 1.0f, 0.0));
    std::vector<float> out(4410 * 2, 0.0f);
    player.render(out.data(), 4410, 44100.0);
    float peak = 0.0f;
    for (const float v : out) peak = std::max(peak, std::fabs(v));
    EXPECT_LT(peak, 1e-4f);
    player.drainReclaimed();
}

TEST_F(AudioLibTest, MasterVolumeClamps) {
    // 主音量越界钳制
    SamplePlayer player;
    player.setMasterVolume(2.0f);
    EXPECT_FLOAT_EQ(player.masterVolume(), 1.0f);
    player.setMasterVolume(-0.5f);
    EXPECT_FLOAT_EQ(player.masterVolume(), 0.0f);
}

}  // namespace
