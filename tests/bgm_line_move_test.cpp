// SPDX-License-Identifier: GPL-3.0-only
// BGM 子轨移动测试：同 (measure,pos,lane,sample) 的多行 ch01 note 按 bgm_line 消歧、
// 跨子轨（bgm1↔bgm2）移动、BGM↔游玩轨互移、撤销精确还原、写回多行结构保持。
// 依据 2026-09 用户反馈（问题1/2）与参考图（iBMSC 式按行序分列）。
#include <gtest/gtest.h>

#include <algorithm>
#include <memory>
#include <tuple>
#include <vector>

#include "beatbench/core/Chart.hpp"
#include "beatbench/core/command/Command.hpp"
#include "beatbench/core/edit/EditorSession.hpp"
#include "beatbench/core/edit/SessionRegistry.hpp"

using namespace beatbench;
using namespace beatbench::edit;
using beatbench::json::Json;
using beatbench::cmd::global_registry;

namespace {

// 构造：m1 有 bgm_line 0 与 bgm_line 1 两个同值 note（同 pos 0/1、同 sample 0x22）；
// 另有 key1 一个。同小节多行 ch01 = 两个独立背景音轨。
Chart make_chart() {
    Chart c;
    c.meta["PLAYER"] = "1";
    c.meta["BPM"] = "130";
    c.samples[{SampleKind::Wav, 0x22}] = {"BGM1.wav", ""};
    Event<Note> b0{1, Rational(0, 1), {}};
    b0.value.lane = {0, LaneKind::Bgm, 0};
    b0.value.sample.id = 0x22;
    b0.value.bgm_line = 0;
    Event<Note> b1{1, Rational(0, 1), {}};
    b1.value.lane = {0, LaneKind::Bgm, 0};
    b1.value.sample.id = 0x22;
    b1.value.bgm_line = 1;
    Event<Note> k1{1, Rational(1, 2), {}};
    k1.value.lane = {0, LaneKind::Key, 1};
    k1.value.sample.id = 1;
    c.notes = {b0, b1, k1};
    return c;
}

}  // namespace

// —— bgm_line 消歧：删除/移动指定行，另一个同值 note 不受影响 ——

TEST(BgmLine, DisambiguateDelete) {
    EditorSession s;
    s.load(make_chart());
    // 删除 bgm_line=1 的那个
    ASSERT_TRUE(s.exec(std::make_unique<DeleteNoteCommand>(
        1, Rational(0, 1), Lane{0, LaneKind::Bgm, 0}, 0x22, 1)));
    ASSERT_EQ(s.chart().notes.size(), 2u);
    // 剩下 bgm_line=0 的 + key1
    bool saw_b0 = false;
    for (const auto& e : s.chart().notes) {
        if (e.value.lane.kind == LaneKind::Bgm) {
            EXPECT_EQ(e.value.bgm_line, 0u);
            saw_b0 = true;
        }
    }
    EXPECT_TRUE(saw_b0);
    // undo 还原两个
    ASSERT_TRUE(s.undo());
    ASSERT_EQ(s.chart().notes.size(), 3u);
}

// —— BGM 子轨间移动（bgm_line=0 → bgm_line=1 指定目标行） ——

TEST(BgmLine, MoveAcrossSubLane) {
    EditorSession s;
    s.load(make_chart());
    // 把 bgm_line=0 移到同小节 bgm_line=1（同 pos）
    ASSERT_TRUE(s.exec(std::make_unique<MoveNoteCommand>(
        1, Rational(0, 1), Lane{0, LaneKind::Bgm, 0}, 0x22,
        1, Rational(0, 1), false, std::nullopt, 0, 1)));
    ASSERT_EQ(s.chart().notes.size(), 3u);
    // 现在两个 Bgm note 都在 bgm_line=1？——不，移动后原 0 变 1，原 1 仍在 1
    // （同 pos 同 lane 同 sample 两 note 均 line=1——允许（编辑器合并显示问题另议；
    // 命令层不禁止，写回按行分组）。
    int bgm_count = 0;
    for (const auto& e : s.chart().notes) {
        if (e.value.lane.kind == LaneKind::Bgm) {
            EXPECT_EQ(e.value.bgm_line, 1u);
            ++bgm_count;
        }
    }
    EXPECT_EQ(bgm_count, 2);
    // undo → 还原 bgm_line=0
    ASSERT_TRUE(s.undo());
    bool saw_0 = false, saw_1 = false;
    for (const auto& e : s.chart().notes) {
        if (e.value.lane.kind == LaneKind::Bgm) {
            if (e.value.bgm_line == 0u) saw_0 = true;
            if (e.value.bgm_line == 1u) saw_1 = true;
        }
    }
    EXPECT_TRUE(saw_0);
    EXPECT_TRUE(saw_1);
}

// —— BGM → 游玩轨（跨命名空间内同 note 语义，到 key 通道） ——

TEST(BgmLine, BgmToKey) {
    EditorSession s;
    s.load(make_chart());
    ASSERT_TRUE(s.exec(std::make_unique<MoveNoteCommand>(
        1, Rational(0, 1), Lane{0, LaneKind::Bgm, 0}, 0x22,
        1, Rational(1, 4), false, Lane{0, LaneKind::Key, 3}, 0)));
    const auto& notes = s.chart().notes;
    ASSERT_EQ(notes.size(), 3u);
    bool found = false;
    for (const auto& e : notes) {
        if (e.measure == 1 && e.pos == Rational(1, 4) &&
            e.value.lane == Lane{0, LaneKind::Key, 3} && e.value.sample.id == 0x22) {
            found = true;
            EXPECT_EQ(e.value.bgm_line, 0u);  // 非 Bgm → 归 0
        }
    }
    EXPECT_TRUE(found);
    // undo 还原
    ASSERT_TRUE(s.undo());
    bool saw_bgm0 = false;
    for (const auto& e : notes) {
        if (e.value.lane.kind == LaneKind::Bgm && e.value.bgm_line == 0u) saw_bgm0 = true;
    }
    EXPECT_TRUE(saw_bgm0);
}
