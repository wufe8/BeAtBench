// SPDX-License-Identifier: GPL-3.0-only
// note.convert 跨命名空间转换测试：note → BGA/BPM/STOP 事件（id 不变），
// invert 精确还原、批量一个 undo 步、BPM/STOP 保留 ref_id 写回。
// 依据 2026-09 用户确认：「只要 BMS 格式上 id 可表示就允许移动」。
#include <gtest/gtest.h>

#include <algorithm>
#include <memory>
#include <string>
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

Chart make_chart() {
    Chart c;
    c.meta["TITLE"] = "测试";
    c.meta["PLAYER"] = "1";
    c.meta["BPM"] = "130";
    // 定义表：#WAV22（BGM 用）→ 只有 WAV；#BPM01=200、#STOP01=96（供转换引用）
    c.samples[{SampleKind::Wav, 0x22}] = {"BGM1.wav", ""};
    c.samples[{SampleKind::Bpm, 1}] = {"", "200"};
    c.samples[{SampleKind::Stop, 1}] = {"", "96"};
    Event<Note> n1{1, Rational(0, 1), {}};
    n1.value.lane = {0, LaneKind::Key, 1};
    n1.value.sample.id = 1;  // #WAV01（与 #BPM01/#STOP01 同文本 id 命名空间）
    Event<Note> n2{1, Rational(1, 2), {}};
    n2.value.lane = {0, LaneKind::Key, 2};
    n2.value.sample.id = 0x22;  // #WAV22（BGA 转换用）
    c.notes = {n1, n2};
    return c;
}

}  // namespace

// —— note → BGA base 层（id 不变：#WAV22 → #BMP22） ——

TEST(ConvertNote, ToBgaIdSame) {
    EditorSession s;
    s.load(make_chart());
    // n2（m1 pos1/2 key2 sample=0x22 #WAV22）→ BGA base，#BMP22 同文本 id
    ASSERT_TRUE(s.exec(std::make_unique<ConvertNoteCommand>(
        1, Rational(1, 2), Lane{0, LaneKind::Key, 2}, 0x22, 0, ConvertTarget::BgaBase,
        1, Rational(1, 2))));
    // note 剩 n1，bga_events 有一个 (m1 pos1/2, image.id=0x22, layer=0)
    ASSERT_EQ(s.chart().notes.size(), 1u);
    ASSERT_EQ(s.chart().bga_events.size(), 1u);
    EXPECT_EQ(s.chart().bga_events[0].measure, 1u);
    EXPECT_EQ(s.chart().bga_events[0].pos, Rational(1, 2));
    EXPECT_EQ(s.chart().bga_events[0].value.image.id, 0x22u);
    EXPECT_EQ(s.chart().bga_events[0].value.layer, 0);
    // undo → 完全还原
    ASSERT_TRUE(s.undo());
    ASSERT_EQ(s.chart().notes.size(), 2u);
    ASSERT_EQ(s.chart().bga_events.size(), 0u);
    bool ok = false;
    for (const auto& e : s.chart().notes) {
        if (e.measure == 1 && e.pos == Rational(1, 2) &&
            e.value.lane == Lane{0, LaneKind::Key, 2} && e.value.sample.id == 0x22)
            ok = true;
    }
    EXPECT_TRUE(ok);
}

// —— note → BPM（#BPM01 引用；value=200 解析自定义表） ——

TEST(ConvertNote, ToBpmRefIdKept) {
    EditorSession s;
    s.load(make_chart());
    // n1（m1 pos0 key1 sample=1 #WAV01）→ #BPM01 引用
    ASSERT_TRUE(s.exec(std::make_unique<ConvertNoteCommand>(
        1, Rational(0, 1), Lane{0, LaneKind::Key, 1}, 1, 0, ConvertTarget::Bpm,
        1, Rational(0, 1))));
    ASSERT_EQ(s.chart().notes.size(), 1u);
    ASSERT_EQ(s.chart().bpm_events.size(), 1u);
    EXPECT_EQ(s.chart().bpm_events[0].measure, 1u);
    EXPECT_EQ(s.chart().bpm_events[0].value.value, 200.0);
    ASSERT_TRUE(s.chart().bpm_events[0].value.ref_id.has_value());
    EXPECT_EQ(*s.chart().bpm_events[0].value.ref_id, 1u);
    // undo → note 回来
    ASSERT_TRUE(s.undo());
    ASSERT_EQ(s.chart().notes.size(), 2u);
    EXPECT_TRUE(s.chart().bpm_events.empty());
}

// —— note → STOP（#STOP01 引用；原始计数 96） ——

TEST(ConvertNote, ToStopRefIdKept) {
    EditorSession s;
    s.load(make_chart());
    ASSERT_TRUE(s.exec(std::make_unique<ConvertNoteCommand>(
        1, Rational(0, 1), Lane{0, LaneKind::Key, 1}, 1, 0, ConvertTarget::Stop,
        1, Rational(0, 1))));
    ASSERT_EQ(s.chart().notes.size(), 1u);
    ASSERT_EQ(s.chart().stop_events.size(), 1u);
    EXPECT_EQ(s.chart().stop_events[0].value.count, 96);
    ASSERT_TRUE(s.chart().stop_events[0].value.ref_id.has_value());
    EXPECT_EQ(*s.chart().stop_events[0].value.ref_id, 1u);
    ASSERT_TRUE(s.undo());
    ASSERT_EQ(s.chart().notes.size(), 2u);
    EXPECT_TRUE(s.chart().stop_events.empty());
}

// —— 协议 dispatch note.convert + 批量 ——

TEST(ConvertNote, ProtocolBatchOneUndo) {
    using beatbench::cmd::global_registry;
    auto& session = beatbench::edit::global_editor_session();
    session.load(make_chart());

    Json req = Json::object();
    req.set("command", "note.convert");
    Json args = Json::object();
    Json sel = Json::array();
    auto push_ref = [&](std::uint32_t measure, std::int64_t pn, std::int64_t pd,
                        std::uint32_t sample, const char* kind, std::uint32_t index) {
        Json item = Json::object();
        item.set("measure", static_cast<std::int64_t>(measure));
        Json pos = Json::object();
        pos.set("num", pn);
        pos.set("den", pd);
        item.set("pos", std::move(pos));
        Json lane = Json::object();
        lane.set("player", 0);
        lane.set("kind", kind);
        lane.set("index", static_cast<std::int64_t>(index));
        item.set("lane", std::move(lane));
        item.set("sample", static_cast<std::int64_t>(sample));
        sel.push_back(std::move(item));
    };
    push_ref(1, 0, 1, 1, "key", 1);      // n1
    push_ref(1, 1, 2, 0x22, "key", 2);   // n2
    args.set("selection", std::move(sel));
    args.set("target", "bga_layer");
    req.set("args", std::move(args));

    const Json resp = global_registry().dispatch(req);
    ASSERT_TRUE(resp.at("ok").as_bool()) << resp.dump();
    EXPECT_EQ(resp.at("result").at("notes").as_i64(), 2);
    EXPECT_EQ(session.undo_depth(), 1u);

    // 两个都变 BGA layer 事件
    ASSERT_EQ(session.chart().bga_events.size(), 2u);
    for (const auto& e : session.chart().bga_events) {
        EXPECT_EQ(e.value.layer, 2);
    }
    ASSERT_EQ(session.chart().notes.size(), 0u);

    // 一次 undo 全部还原
    req = Json::object();
    req.set("command", "session.undo");
    const Json resp2 = global_registry().dispatch(req);
    ASSERT_TRUE(resp2.at("ok").as_bool()) << resp2.dump();
    ASSERT_EQ(session.chart().notes.size(), 2u);
    EXPECT_TRUE(session.chart().bga_events.empty());
}

// —— note.moveRegion 带 target（整组转换 + 位移，一个 undo 步） ——

TEST(ConvertNote, MoveRegionWithTarget) {
    using beatbench::cmd::global_registry;
    auto& session = beatbench::edit::global_editor_session();
    session.load(make_chart());

    Json req = Json::object();
    req.set("command", "note.moveRegion");
    Json args = Json::object();
    Json sel = Json::array();
    Json item = Json::object();
    item.set("measure", 1);
    Json pos = Json::object();
    pos.set("num", 1);
    pos.set("den", 2);
    item.set("pos", std::move(pos));
    Json lane = Json::object();
    lane.set("player", 0);
    lane.set("kind", "key");
    lane.set("index", 2);
    item.set("lane", std::move(lane));
    item.set("sample", 0x22);
    sel.push_back(std::move(item));
    args.set("selection", std::move(sel));
    Json delta = Json::object();
    delta.set("measure", 2);
    Json dpos = Json::object();
    dpos.set("num", 0);
    dpos.set("den", 1);
    delta.set("pos", std::move(dpos));
    args.set("delta", std::move(delta));
    args.set("target", "bpm");
    req.set("args", std::move(args));

    const Json resp = global_registry().dispatch(req);
    ASSERT_TRUE(resp.at("ok").as_bool()) << resp.dump();
    // note 消失；bpm 事件在 m3 pos1/2（+2 小节，pos 保持源值 1/2）
    ASSERT_EQ(session.chart().notes.size(), 1u);  // 只转换了选中的那个
    ASSERT_EQ(session.chart().bpm_events.size(), 1u);
    EXPECT_EQ(session.chart().bpm_events[0].measure, 3u);
    EXPECT_EQ(session.chart().bpm_events[0].pos, Rational(1, 2));
}
