// SPDX-License-Identifier: GPL-3.0-only
// M5 播放单测：PcmPlayback（状态机 + 时钟）+ PlaybackPlan（映射层）。
// 无设备/无 Qt：SamplePlayer 纯 DSP + 命令队列可独立测试（同 audio_test）。
#include <cmath>
#include <memory>
#include <vector>

#include <gtest/gtest.h>

#include "beatbench/audio/DecodedSample.hpp"
#include "beatbench/audio/PcmPlayback.hpp"
#include "beatbench/audio/PlaybackPlan.hpp"
#include "beatbench/audio/SamplePlayer.hpp"

namespace {

using beatbench::audio::DecodedSample;
using beatbench::audio::PcmPlayback;
using beatbench::audio::PlaybackPlan;
using beatbench::audio::SamplePlayer;

/// 合成共享 PCM（渲染代理缓冲等价物）：常数振幅（断言方便）。
std::shared_ptr<const std::vector<float>> makePcm(double rate, std::size_t frames,
                                                  float amp = 0.5f) {
    auto v = std::make_shared<std::vector<float>>(frames * 2);
    for (std::size_t i = 0; i < frames; ++i) {
        (*v)[i * 2] = amp;
        (*v)[i * 2 + 1] = amp;
    }
    return v;
}

/// 读一份 voice 输出并推帧（模拟回调：render 推进 m_totalFrames）。
void renderFrames(SamplePlayer& p, std::size_t frames, double rate = 44100.0) {
    std::vector<float> out(frames * 2, 0.0f);
    p.render(out.data(), static_cast<int>(frames), rate);
}

TEST(PcmPlaybackTest, PlayFromStartRendersAudio) {
    SamplePlayer player;
    PcmPlayback pb(&player, 44100.0);
    const auto pcm = makePcm(44100.0, 44100 * 2);  // 2 秒
    pb.load(pcm, 44100.0);
    ASSERT_TRUE(pb.play(1.0f));
    EXPECT_EQ(pb.state(), PcmPlayback::State::Playing);
    // 推 1 秒：应有非零输出 + 时钟前进
    std::vector<float> out(44100 * 2, 0.0f);
    player.render(out.data(), 44100, 44100.0);
    float peak = 0.0f;
    for (const float v : out) peak = std::max(peak, std::fabs(v));
    EXPECT_GT(peak, 0.1f);
    EXPECT_NEAR(pb.currentSec(), 1.0, 0.01);
    // 收尾
    pb.stop();
    player.drainReclaimed();
}

TEST(PcmPlaybackTest, PauseFreezeAndResume) {
    SamplePlayer player;
    PcmPlayback pb(&player, 44100.0);
    pb.load(makePcm(44100.0, 44100 * 2), 44100.0);
    ASSERT_TRUE(pb.play(1.0f));
    renderFrames(player, 44100);  // 1s
    EXPECT_NEAR(pb.currentSec(), 1.0, 0.01);
    pb.pause();
    EXPECT_EQ(pb.state(), PcmPlayback::State::Paused);
    const double frozen = pb.currentSec();
    // 暂停后不推进（无 voice）——再推帧也不会前进（时钟 = 冻结位置）
    renderFrames(player, 44100);
    EXPECT_NEAR(pb.currentSec(), frozen, 1e-9);
    // 恢复 = 播放（从冻结位置）
    ASSERT_TRUE(pb.play(1.0f));
    EXPECT_EQ(pb.state(), PcmPlayback::State::Playing);
    renderFrames(player, 44100);  // 再 1s
    EXPECT_NEAR(pb.currentSec(), 2.0, 0.02);
    pb.stop();
    player.drainReclaimed();
}

TEST(PcmPlaybackTest, StopKeepsPositionAndResumeContinues) {
    SamplePlayer player;
    PcmPlayback pb(&player, 44100.0);
    pb.load(makePcm(44100.0, 44100 * 2), 44100.0);
    ASSERT_TRUE(pb.play(1.0f));
    renderFrames(player, 44100 * 0.5);  // 0.5s
    pb.stop();
    EXPECT_EQ(pb.state(), PcmPlayback::State::Stopped);
    EXPECT_NEAR(pb.currentSec(), 0.5, 0.01);
    // 再播 → 从 0.5 续播（不是从头）
    ASSERT_TRUE(pb.play(1.0f));
    EXPECT_EQ(pb.state(), PcmPlayback::State::Playing);
    renderFrames(player, 44100 * 0.5);
    EXPECT_NEAR(pb.currentSec(), 1.0, 0.02);
    pb.stop();
    player.drainReclaimed();
}

TEST(PcmPlaybackTest, SeekClampsAndWorksWhilePlaying) {
    SamplePlayer player;
    PcmPlayback pb(&player, 44100.0);
    pb.load(makePcm(44100.0, 44100 * 3), 44100.0);  // 3 秒
    ASSERT_TRUE(pb.play(1.0f));
    renderFrames(player, 44100);
    // 播放中 seek 到 2.0
    ASSERT_TRUE(pb.seek(2.0));
    EXPECT_EQ(pb.state(), PcmPlayback::State::Playing);
    renderFrames(player, 44100);
    EXPECT_NEAR(pb.currentSec(), 3.0, 0.02);  // 2.0 + 1s（夹到 3）
    // seek 负值夹到 0
    ASSERT_TRUE(pb.seek(-5.0));
    EXPECT_EQ(pb.state(), PcmPlayback::State::Playing);
    EXPECT_NEAR(pb.currentSec(), 0.0, 0.02);
    pb.stop();
    player.drainReclaimed();
}

TEST(PcmPlaybackTest, LoadEmptyFails) {
    SamplePlayer player;
    PcmPlayback pb(&player, 44100.0);
    EXPECT_FALSE(pb.play(1.0f));  // 未载入
    EXPECT_EQ(pb.state(), PcmPlayback::State::Idle);
}

TEST(PcmPlaybackTest, DeviceRateClock) {
    SamplePlayer player;
    PcmPlayback pb(&player, 48000.0);  // 设备率 48k
    pb.load(makePcm(44100.0, 44100 * 2), 44100.0);
    ASSERT_TRUE(pb.play(1.0f));
    renderFrames(player, 48000, 48000.0);  // 1s 输出
    EXPECT_NEAR(pb.currentSec(), 1.0, 0.01);
    pb.stop();
    player.drainReclaimed();
}

// —— PlaybackPlan（映射层） ——

TEST(PlaybackPlanTest, SortAndSearch) {
    beatbench::Chart chart;
    beatbench::TimingEngine timing;
    // 每小节 4 拍 130BPM；16 分音符
    chart.meta["BPM"] = "130";
    for (std::uint32_t m = 0; m < 4; ++m) {
        for (int i = 0; i < 16; ++i) {
            beatbench::Event<beatbench::Note> n;
            n.measure = m;
            n.pos = beatbench::Rational(i, 16);
            n.value.lane.player = 0;
            n.value.lane.kind = beatbench::LaneKind::Key;
            n.value.lane.index = 0;
            n.value.sample.id = 1;
            chart.notes.push_back(n);
        }
    }
    chart.samples[{beatbench::SampleKind::Wav, 1}].file = "kick.wav";
    timing.rebuild(chart);
    const auto plan = PlaybackPlan::build(chart, timing);
    ASSERT_EQ(plan.size(), 64u);
    // 触发秒单调 + 有间隔（130BPM 16 分音符 ≈ 0.115s）
    for (std::size_t i = 1; i < plan.size(); ++i)
        EXPECT_GE(plan.notes()[i].triggerSec, plan.notes()[i - 1].triggerSec);
    EXPECT_GT(plan.notes()[1].triggerSec - plan.notes()[0].triggerSec, 0.05);
    // lower_bound/upper_bound
    const double t = plan.notes()[20].triggerSec;
    EXPECT_EQ(plan.firstIndexAt(t), 20u);
    // lastIndexBefore(t) = 最后一个 <= t
    EXPECT_EQ(plan.lastIndexBefore(t), 20u);
    // 边界
    EXPECT_EQ(plan.firstIndexAt(0.0), 0u);
    EXPECT_EQ(plan.firstIndexAt(plan.notes().back().triggerSec + 1.0), 64u);
}

TEST(PlaybackPlanTest, StopAndChangingBpm) {
    // 变速 + STOP：验证 triggerSec 与 TimingEngine 一致（M4 锚点）
    beatbench::Chart chart;
    beatbench::TimingEngine timing;
    chart.meta["BPM"] = "120";
    // measure 0：BPM 120；measure 1：BPM 240；measure 2：STOP 1 次（1/192 全音符）
    beatbench::Event<beatbench::Bpm> bpm;
    bpm.measure = 1; bpm.pos = beatbench::Rational(0, 1); bpm.value.value = 240;
    chart.bpm_events.push_back(bpm);
    beatbench::Event<beatbench::Stop> stop;
    stop.measure = 1; stop.pos = beatbench::Rational(2, 4); stop.value.count = 192;
    chart.stop_events.push_back(stop);
    for (int i = 0; i < 4; ++i) {
        beatbench::Event<beatbench::Note> n;
        n.measure = 1;
        n.pos = beatbench::Rational(i, 4);
        n.value.lane.player = 0;
        n.value.lane.kind = beatbench::LaneKind::Key;
        n.value.lane.index = 0;
        n.value.sample.id = 1;
        chart.notes.push_back(n);
    }
    chart.samples[{beatbench::SampleKind::Wav, 1}].file = "kick.wav";
    timing.rebuild(chart);
    const auto plan = PlaybackPlan::build(chart, timing);
    ASSERT_EQ(plan.size(), 4u);
    // m1 pos0 = 1 小节（120BPM 4/4 = 2s）
    EXPECT_NEAR(plan.notes()[0].triggerSec, 2.0, 0.001);
    // m1 pos1/4 = 2s + 1 拍 (240BPM) = 2.25s
    EXPECT_NEAR(plan.notes()[1].triggerSec, 2.25, 0.001);
    // m1 pos2/4 处 STOP 192（1 全音符 = 240/240 = 1s）→ pos3/4 = 2.0 + 0.75(拍位) + 1(STOP) = 3.75s
    EXPECT_NEAR(plan.notes()[3].triggerSec, 3.75, 0.001);
    EXPECT_NEAR(plan.notes()[2].triggerSec, 2.5, 0.001);  // pos2/4 = STOP 起点（不加）
}

TEST(PlaybackPlanTest, Bpm9999xxxZeroDuration) {
    // 9999xxx（9999280）：该段 ≈0 时长（beatoraja 口径；M4 已拍板不特判）
    beatbench::Chart chart;
    beatbench::TimingEngine timing;
    chart.meta["BPM"] = "280";
    chart.samples[{beatbench::SampleKind::Wav, 1}].file = "kick.wav";
    // measure 0 起点就换 9999280（事件在 pos0）
    beatbench::Event<beatbench::Bpm> bpm;
    bpm.measure = 0; bpm.pos = beatbench::Rational(0, 1); bpm.value.value = 9999280;
    chart.bpm_events.push_back(bpm);
    // 段内 note（pos1/4 与 pos3/4）
    for (int i = 1; i <= 3; i += 2) {
        beatbench::Event<beatbench::Note> n;
        n.measure = 0;
        n.pos = beatbench::Rational(i, 4);
        n.value.lane.player = 0;
        n.value.lane.kind = beatbench::LaneKind::Key;
        n.value.lane.index = 0;
        n.value.sample.id = 1;
        chart.notes.push_back(n);
    }
    timing.rebuild(chart);
    const auto plan = PlaybackPlan::build(chart, timing);
    ASSERT_EQ(plan.size(), 2u);
    // 两 note 触发秒 ≈ 0（9999280 段时长 ≈ 24µs）
    EXPECT_NEAR(plan.notes()[0].triggerSec, 0.0, 1e-3);
    EXPECT_NEAR(plan.notes()[1].triggerSec, 0.0, 1e-3);
}

}  // namespace
