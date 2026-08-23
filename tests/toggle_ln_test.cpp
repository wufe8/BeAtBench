// SPDX-License-Identifier: GPL-3.0-only
// 单点 ↔ LN 转换（note.toggleLn / ToggleLnCommand）测试：
// - LN → 单点：断开两端 ln_pair；
// - 单点 → LN：向前配最近同 lane 同 sample 未配对单点；
// - 找不到配对象 → 无操作（命令成功、m_did_change=false）；
// - invert 精确还原；协议批量一个 undo 步。
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

bool ln_consistent(const std::vector<Event<Note>>& notes) {
    for (std::size_t i = 0; i < notes.size(); ++i) {
        const auto p = notes[i].value.ln_pair;
        if (!p) continue;
        if (*p >= notes.size()) return false;
        const auto q = notes[*p].value.ln_pair;
        if (!q || *q != i) return false;
    }
    return true;
}

// m1 key1：pos0 单点 s1 + pos1/2 单点 s1（两个可配）；m1 key2：pos0 单点 s2
Chart make_chart() {
    Chart c;
    c.meta["PLAYER"] = "1";
    c.meta["BPM"] = "130";
    c.samples[{SampleKind::Wav, 1}] = {"a.wav", ""};
    c.samples[{SampleKind::Wav, 2}] = {"b.wav", ""};
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
    return c;
}

}  // namespace

// —— 单点 → LN（pos1/2 的 s1 key1 向前配 pos0 的 s1 key1） ——

TEST(ToggleLn, SingleToLnPairsForward) {
    EditorSession s;
    s.load(make_chart());
    ASSERT_TRUE(s.exec(std::make_unique<ToggleLnCommand>(
        1, Rational(1, 2), Lane{0, LaneKind::Key, 1}, 1)));
    EXPECT_TRUE(ln_consistent(s.chart().notes));
    // n1（pos0）与 n2（pos1/2）互指；n3 无配对
    bool n1_ok = false, n2_ok = false, n3_ok = false;
    for (const auto& e : s.chart().notes) {
        if (e.measure == 1 && e.pos == Rational(0, 1) &&
            e.value.lane == Lane{0, LaneKind::Key, 1})
            n1_ok = e.value.ln_pair.has_value();
        if (e.measure == 1 && e.pos == Rational(1, 2) &&
            e.value.lane == Lane{0, LaneKind::Key, 1})
            n2_ok = e.value.ln_pair.has_value();
        if (e.measure == 1 && e.value.lane == Lane{0, LaneKind::Key, 2})
            n3_ok = !e.value.ln_pair.has_value();
    }
    EXPECT_TRUE(n1_ok);
    EXPECT_TRUE(n2_ok);
    EXPECT_TRUE(n3_ok);
    // invert：清除配对 → 全部单点
    ASSERT_TRUE(s.undo());
    EXPECT_TRUE(ln_consistent(s.chart().notes));
    for (const auto& e : s.chart().notes)
        EXPECT_FALSE(e.value.ln_pair.has_value());
}

// —— LN → 单点（断开两端） ——

TEST(ToggleLn, LnToSingleBreaksBoth) {
    EditorSession s;
    s.load(make_chart());
    // 先配成 LN
    ASSERT_TRUE(s.exec(std::make_unique<ToggleLnCommand>(
        1, Rational(1, 2), Lane{0, LaneKind::Key, 1}, 1)));
    // 再断开（对任一端）
    ASSERT_TRUE(s.exec(std::make_unique<ToggleLnCommand>(
        1, Rational(0, 1), Lane{0, LaneKind::Key, 1}, 1)));
    EXPECT_TRUE(ln_consistent(s.chart().notes));
    for (const auto& e : s.chart().notes)
        EXPECT_FALSE(e.value.ln_pair.has_value());
    // undo 两次 → 恢复配对（第二次 undo 重建互指）
    ASSERT_TRUE(s.undo());
    bool paired = false;
    for (const auto& e : s.chart().notes)
        if (e.value.ln_pair) paired = true;
    EXPECT_TRUE(paired);
}

// —— 找不到配对象 → 无操作（key2 的 s2 前后无同 lane 同 sample 单点） ——

TEST(ToggleLn, NoPartnerNoOp) {
    EditorSession s;
    s.load(make_chart());
    ASSERT_TRUE(s.exec(std::make_unique<ToggleLnCommand>(
        1, Rational(0, 1), Lane{0, LaneKind::Key, 2}, 2)));
    for (const auto& e : s.chart().notes)
        EXPECT_FALSE(e.value.ln_pair.has_value());
}

// —— 协议 dispatch 批量一个 undo 步 ——

TEST(ToggleLn, ProtocolBatchOneUndo) {
    using beatbench::cmd::global_registry;
    auto& session = beatbench::edit::global_editor_session();
    session.load(make_chart());

    Json req = Json::object();
    req.set("command", "note.toggleLn");
    Json args = Json::object();
    Json sel = Json::array();
    auto push = [&](std::uint32_t measure, std::int64_t pn, std::int64_t pd,
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
    push(1, 1, 2, 1, "key", 1);
    args.set("selection", std::move(sel));
    req.set("args", std::move(args));

    const Json resp = global_registry().dispatch(req);
    ASSERT_TRUE(resp.at("ok").as_bool()) << resp.dump();
    EXPECT_EQ(session.undo_depth(), 1u);
    EXPECT_TRUE(ln_consistent(session.chart().notes));
}
