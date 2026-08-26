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

// —— sample.setFile：改现有定义文件 / 新键创建 / invert 恢复 ——

TEST(SampleRename, SetFileExistingAndCreate) {
    EditorSession s;
    s.load(make_chart());
    // 现有 #WAV1 改文件
    ASSERT_TRUE(s.exec(std::make_unique<SetSampleFileCommand>(SampleKind::Wav, 1, "x.wav")));
    EXPECT_EQ(s.chart().samples.at({SampleKind::Wav, 1}).file, "x.wav");
    // 新键 #WAV9 创建（原本不存在）
    ASSERT_TRUE(s.exec(std::make_unique<SetSampleFileCommand>(SampleKind::Wav, 9, "new.wav")));
    EXPECT_EQ(s.chart().samples.at({SampleKind::Wav, 9}).file, "new.wav");
    // undo 两次：9 移除、1 恢复 a.wav
    ASSERT_TRUE(s.undo());
    EXPECT_FALSE(has_sample(s.chart(), SampleKind::Wav, 9));
    ASSERT_TRUE(s.undo());
    EXPECT_EQ(s.chart().samples.at({SampleKind::Wav, 1}).file, "a.wav");
}

// —— sample.setValue：BPM/STOP 定义表 值设置 / 新键创建 / invert 恢复 ——

TEST(SampleRename, SetValueExistingAndCreate) {
    EditorSession s;
    s.load(make_chart());
    // 现有 #BPM4（make_chart 值 "280"）改值
    ASSERT_TRUE(s.exec(std::make_unique<SetSampleValueCommand>(SampleKind::Bpm, 4, "240")));
    EXPECT_EQ(s.chart().samples.at({SampleKind::Bpm, 4}).value, "240");
    // 新键 #STOP9 创建（原本不存在）
    ASSERT_TRUE(s.exec(std::make_unique<SetSampleValueCommand>(SampleKind::Stop, 9, "192")));
    EXPECT_EQ(s.chart().samples.at({SampleKind::Stop, 9}).value, "192");
    // undo 两次：9 移除、BPM4 恢复 "280"
    ASSERT_TRUE(s.undo());
    EXPECT_FALSE(has_sample(s.chart(), SampleKind::Stop, 9));
    ASSERT_TRUE(s.undo());
    EXPECT_EQ(s.chart().samples.at({SampleKind::Bpm, 4}).value, "280");
}

TEST(SampleRename, SetValueCrossSyncsRefEvents) {
    // 交叉同步：改定义值 → 引用该 #BPMxx 的事件值同步；undo 恢复旧值。
    // make_chart 已有 bpm_events[0]：value 280、ref_id 4（#BPM4 = "280"）。
    EditorSession s;
    s.load(make_chart());
    EXPECT_EQ(s.chart().bpm_events.size(), 1u);
    EXPECT_EQ(*s.chart().bpm_events[0].value.ref_id, 4u);
    // 改定义值 280 → 300：引用事件同步
    ASSERT_TRUE(s.exec(std::make_unique<SetSampleValueCommand>(SampleKind::Bpm, 4, "300")));
    EXPECT_EQ(s.chart().samples.at({SampleKind::Bpm, 4}).value, "300");
    ASSERT_EQ(s.chart().bpm_events.size(), 1u);
    EXPECT_DOUBLE_EQ(s.chart().bpm_events[0].value.value, 300.0);
    // undo：定义回 "280"，事件回 280
    ASSERT_TRUE(s.undo());
    EXPECT_EQ(s.chart().samples.at({SampleKind::Bpm, 4}).value, "280");
    EXPECT_DOUBLE_EQ(s.chart().bpm_events[0].value.value, 280.0);
}

TEST(SampleRename, PutTimingSyncsDefAndReferencers) {
    // 共享 id 语义：编辑 ref 绑定事件的值 = 改该 #BPMxx 定义值（引用者全同步）；undo 恢复。
    EditorSession s;
    s.load(make_chart());
    // make_chart：bpm_events[0] = m1 pos0 value 280 ref_id 4（#BPM4 = "280"）
    ASSERT_TRUE(s.exec(std::make_unique<PutTimingCommand>(
        TimingKind::Bpm, 1, Rational(0, 1), 300.0)));
    EXPECT_EQ(s.chart().samples.at({SampleKind::Bpm, 4}).value, "300");
    EXPECT_DOUBLE_EQ(s.chart().bpm_events[0].value.value, 300.0);
    // undo：定义回 "280"，事件回 280
    ASSERT_TRUE(s.undo());
    EXPECT_EQ(s.chart().samples.at({SampleKind::Bpm, 4}).value, "280");
    EXPECT_DOUBLE_EQ(s.chart().bpm_events[0].value.value, 280.0);
}

// —— note.setSample：改单条 note 引用、invert 恢复、找不到无操作 ——

TEST(SampleRename, SetNoteSampleChangesOne) {
    EditorSession s;
    s.load(make_chart());
    // m1 key1 pos0 的 s1 note → s5
    ASSERT_TRUE(s.exec(std::make_unique<SetNoteSampleCommand>(
        1, Rational(0, 1), Lane{0, LaneKind::Key, 1}, 1, 0, 5)));
    int s5 = 0, s2 = 0;
    for (const auto& e : s.chart().notes) {
        if (e.value.sample.id == 5) s5++;
        if (e.value.sample.id == 2) s2++;
    }
    EXPECT_EQ(s5, 1);  // 只有 pos0 那条改了
    EXPECT_EQ(s2, 1);
    ASSERT_TRUE(s.undo());
    int s1 = 0;
    for (const auto& e : s.chart().notes)
        if (e.value.sample.id == 1) s1++;
    EXPECT_EQ(s1, 2);
}

TEST(SampleRename, SetNoteSampleNoMatchNoOp) {
    EditorSession s;
    s.load(make_chart());
    // 不存在 (measure 9, pos0, lane key9, sample 7) → 无操作
    ASSERT_TRUE(s.exec(std::make_unique<SetNoteSampleCommand>(
        9, Rational(0, 1), Lane{0, LaneKind::Key, 9}, 7, 0, 5)));
    for (const auto& e : s.chart().notes)
        EXPECT_FALSE(e.value.sample.id == 5);
}

// —— 协议 note.setSample（id 文本 to） ——

TEST(SampleRename, ProtocolSetNoteSample) {
    auto& session = beatbench::edit::global_editor_session();
    session.load(make_chart());
    Json req = Json::object();
    req.set("command", "note.setSample");
    Json args = Json::object();
    args.set("measure", static_cast<std::int64_t>(1));
    Json pos = Json::object();
    pos.set("num", 0); pos.set("den", 1);
    args.set("pos", std::move(pos));
    Json lane = Json::object();
    lane.set("player", 0); lane.set("kind", "key"); lane.set("index", 1);
    args.set("lane", std::move(lane));
    args.set("sample", static_cast<std::int64_t>(1));
    args.set("to", "1A");
    req.set("args", std::move(args));
    const Json resp = global_registry().dispatch(req);
    ASSERT_TRUE(resp.at("ok").as_bool()) << resp.dump();
    int s46 = 0;
    for (const auto& e : session.chart().notes)
        if (e.value.sample.id == 46) s46++;
    EXPECT_EQ(s46, 1);  // 1A 解码 = 46
}
