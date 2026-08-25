// SPDX-License-Identifier: GPL-3.0-only
// 采样定义 id 重命名（sample.rename / RenameSampleCommand + session.samples）测试：
// - Wav：定义表键 (Wav, from) → (Wav, to)，引用该 id 的 note 一并改到 to；
// - invert 精确还原（含碰撞时原 to 定义与引用恢复）；
// - 协议 dispatch sample.rename + session.samples（返回内存会话样本，供面板刷新）。
#include <gtest/gtest.h>

#include <memory>
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

// m1 key1：pos0 s1、pos1/2 s1；m1 key2：pos0 s2；BGA base 层用 BMP 3；BPM ch08 引用 #BPM4
Chart make_chart() {
    Chart c;
    c.samples[{SampleKind::Wav, 1}] = {"a.wav", ""};
    c.samples[{SampleKind::Wav, 2}] = {"b.wav", ""};
    c.samples[{SampleKind::Bmp, 3}] = {"bg.png", ""};
    c.samples[{SampleKind::Bpm, 4}] = {"", "280"};

    Event<Note> n1{1, Rational(0, 1), {}};
    n1.value.lane = {0, LaneKind::Key, 1};
    n1.value.sample.id = 1;
    Event<Note> n2{1, Rational(1, 2), {}};
    n2.value.lane = {0, LaneKind::Key, 1};
    n2.value.sample.id = 1;
    Event<Note> n3{1, Rational(0, 1), {}};
    n3.value.lane = {0, LaneKind::Key, 2};
    n3.value.sample.id = 2;
    c.notes = {n1, n2, n3};

    Event<Bga> bga{1, Rational(0, 1), {}};
    bga.value.image.id = 3;
    bga.value.layer = 0;
    c.bga_events = {bga};

    Event<Bpm> bpm{1, Rational(0, 1), {}};
    bpm.value.value = 280.0;
    bpm.value.ref_id = 4;
    c.bpm_events = {bpm};

    return c;
}

bool has_sample(const Chart& c, SampleKind k, std::uint32_t id) {
    return c.samples.count({k, id}) > 0;
}

}  // namespace

// —— Wav 重映射：定义表键 + note 引用 ——

TEST(SampleRename, WavRemapsDefAndNotes) {
    EditorSession s;
    s.load(make_chart());
    ASSERT_TRUE(s.exec(std::make_unique<RenameSampleCommand>(SampleKind::Wav, 1, 5)));

    const auto& c = s.chart();
    EXPECT_FALSE(has_sample(c, SampleKind::Wav, 1));
    EXPECT_TRUE(has_sample(c, SampleKind::Wav, 5));
    EXPECT_EQ(c.samples.at({SampleKind::Wav, 5}).file, "a.wav");
    // 引用 1 的两个 note → 5；引用 2 的 note 不变
    int ref5 = 0, ref2 = 0;
    for (const auto& e : c.notes) (e.value.sample.id == 5 ? ref5 : ref2)++;
    EXPECT_EQ(ref5, 2);
    EXPECT_EQ(ref2, 1);
}

// —— invert 精确还原 ——

TEST(SampleRename, InvertRestores) {
    EditorSession s;
    s.load(make_chart());
    ASSERT_TRUE(s.exec(std::make_unique<RenameSampleCommand>(SampleKind::Wav, 1, 5)));
    ASSERT_TRUE(s.undo());

    const auto& c = s.chart();
    EXPECT_TRUE(has_sample(c, SampleKind::Wav, 1));
    EXPECT_FALSE(has_sample(c, SampleKind::Wav, 5));
    int ref1 = 0, ref2 = 0;
    for (const auto& e : c.notes) (e.value.sample.id == 1 ? ref1 : ref2)++;
    EXPECT_EQ(ref1, 2);
    EXPECT_EQ(ref2, 1);
}

// —— 碰撞：to 已有不同定义 → 覆盖后 invert 恢复原 to ——

TEST(SampleRename, CollisionRestoresTarget) {
    EditorSession s;
    s.load(make_chart());
    // 重命名 2 → 3，但 3 已有 Bmp（不同 kind，无碰撞）；换成 Wav 5 与 Wav 2 碰撞验证
    // 这里用 Wav 2 → Wav 1（1 已存在 Wav）演示碰撞覆盖
    ASSERT_TRUE(s.exec(std::make_unique<RenameSampleCommand>(SampleKind::Wav, 2, 1)));
    const auto& c = s.chart();
    // 2→1：samples[(Wav,1)] 被覆盖为 b.wav
    EXPECT_EQ(c.samples.at({SampleKind::Wav, 1}).file, "b.wav");
    ASSERT_TRUE(s.undo());
    // 还原：1 恢复 a.wav，2 恢复 b.wav
    EXPECT_EQ(s.chart().samples.at({SampleKind::Wav, 1}).file, "a.wav");
    EXPECT_EQ(s.chart().samples.at({SampleKind::Wav, 2}).file, "b.wav");
}

// —— BGA(BMP) / BPM 引用不因 Wav 重命名受影响 ——

TEST(SampleRename, OtherKindsUntouched) {
    EditorSession s;
    s.load(make_chart());
    ASSERT_TRUE(s.exec(std::make_unique<RenameSampleCommand>(SampleKind::Wav, 1, 5)));
    const auto& c = s.chart();
    EXPECT_EQ(c.bga_events[0].value.image.id, 3u);
    ASSERT_TRUE(c.bpm_events[0].value.ref_id);
    EXPECT_EQ(*c.bpm_events[0].value.ref_id, 4u);
}

// —— 协议 sample.rename + session.samples ——

TEST(SampleRename, ProtocolAndSessionSamples) {
    auto& session = beatbench::edit::global_editor_session();
    session.load(make_chart());

    Json req = Json::object();
    req.set("command", "sample.rename");
    Json args = Json::object();
    args.set("from", "01");
    args.set("to", "1A");
    req.set("args", std::move(args));
    const Json resp = global_registry().dispatch(req);
    ASSERT_TRUE(resp.at("ok").as_bool()) << resp.dump();
    // 01 → 1A（base36 解码：01=1, 1A=1*36+10=46）
    EXPECT_EQ(session.undo_depth(), 1u);

    // session.samples 返回当前内存样本（应含 1A 对应条目）
    Json req2 = Json::object();
    req2.set("command", "session.samples");
    Json args2 = Json::object();
    req2.set("args", std::move(args2));
    const Json resp2 = global_registry().dispatch(req2);
    ASSERT_TRUE(resp2.at("ok").as_bool()) << resp2.dump();
    bool found = false;
    const Json& wav = resp2.at("result").at("samples").at("wav");
    ASSERT_TRUE(wav.is_array());
    for (const auto& item : wav.as_array()) {
        if (item.at("id").as_str() == "1A") found = true;
    }
    EXPECT_TRUE(found) << resp2.dump();
}
