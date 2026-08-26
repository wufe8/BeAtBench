// SPDX-License-Identifier: GPL-3.0-only
// 时间轴事件命令测试：timing.put / timing.delete / timing.list 的 apply/invert
// 精确往返、同位替换语义、undo/redo 栈、协议 dispatch 链路（doc/05 §9「工具栏值放置」
// + 右 dock 管理列表；两种 GUI 形态共用同一套接口）。
// 全部合成 Chart，不依赖 local/。
#include <gtest/gtest.h>

#include <memory>
#include <string>
#include <tuple>
#include <type_traits>
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
    // 一个 BPM 事件 + 一个 STOP 事件 + 一个小节定义
    c.bpm_events = {{1, Rational(0, 1), Bpm{130.0}},
                    {1, Rational(1, 2), Bpm{170.0}}};
    c.stop_events = {{1, Rational(1, 2), Stop{96}}};
    c.measure_events = {{1, Rational(0, 1), MeasureLen{4.0}}};
    return c;
}

// 事件 → (measure, pos, value) 归一列表（值统一 double）
template <typename T>
std::vector<std::tuple<std::uint32_t, Rational, double>> norm_evs(
    const std::vector<Event<T>>& evs) {
    std::vector<std::tuple<std::uint32_t, Rational, double>> out;
    for (const auto& e : evs) {
        double v = 0;
        if constexpr (std::is_same_v<T, Bpm>) v = e.value.value;
        else if constexpr (std::is_same_v<T, Stop>) v = static_cast<double>(e.value.count);
        else v = e.value.beats;
        out.emplace_back(e.measure, e.pos, v);
    }
    return out;
}

}  // namespace

// —— put：插入 / 同位替换 / 往返 ——

TEST(TimingCommands, PutBpmInserts) {
    EditorSession s;
    s.load(make_chart());
    ASSERT_TRUE(s.exec(std::make_unique<PutTimingCommand>(
        TimingKind::Bpm, 2, Rational(0, 1), 200.0)));
    ASSERT_EQ(s.chart().bpm_events.size(), 3u);
    EXPECT_DOUBLE_EQ(s.chart().bpm_events.back().value.value, 200.0);
    EXPECT_EQ(s.chart().bpm_events.back().measure, 2u);
    // undo 移除
    ASSERT_TRUE(s.undo());
    EXPECT_EQ(norm_evs(s.chart().bpm_events), norm_evs(make_chart().bpm_events));
    // redo 恢复
    ASSERT_TRUE(s.redo());
    ASSERT_EQ(s.chart().bpm_events.size(), 3u);
}

TEST(TimingCommands, PutReplacesSamePos) {
    EditorSession s;
    s.load(make_chart());
    // m1 pos0 已有 130 → 同位 put 200 替换（引擎同 pos 覆盖语义）
    ASSERT_TRUE(s.exec(std::make_unique<PutTimingCommand>(
        TimingKind::Bpm, 1, Rational(0, 1), 200.0)));
    ASSERT_EQ(s.chart().bpm_events.size(), 2u);  // 不新增
    // undo 恢复 130
    ASSERT_TRUE(s.undo());
    const auto before = norm_evs(make_chart().bpm_events);
    EXPECT_EQ(norm_evs(s.chart().bpm_events), before);
}

TEST(TimingCommands, PutStopAndMeasure) {
    EditorSession s;
    s.load(make_chart());
    // STOP：新增（m2）
    ASSERT_TRUE(s.exec(std::make_unique<PutTimingCommand>(
        TimingKind::Stop, 2, Rational(1, 4), 96.0)));
    ASSERT_EQ(s.chart().stop_events.size(), 2u);
    // 节拍：pos 传非 0 → 归 0；同位替换 m1
    ASSERT_TRUE(s.exec(std::make_unique<PutTimingCommand>(
        TimingKind::Measure, 1, Rational(1, 2), 8.0)));
    ASSERT_EQ(s.chart().measure_events.size(), 1u);  // 替换不新增
    EXPECT_DOUBLE_EQ(s.chart().measure_events[0].value.beats, 8.0);
    EXPECT_EQ(s.chart().measure_events[0].pos, Rational(0, 1));  // 归 0
    // 两步一起 undo → 原始
    ASSERT_TRUE(s.undo());
    ASSERT_TRUE(s.undo());
    EXPECT_EQ(norm_evs(s.chart().stop_events), norm_evs(make_chart().stop_events));
    EXPECT_EQ(norm_evs(s.chart().measure_events), norm_evs(make_chart().measure_events));
}

// —— delete ——

TEST(TimingCommands, DeleteRemoves) {
    EditorSession s;
    s.load(make_chart());
    // 删 m1 pos1/2 的 BPM（170）
    ASSERT_TRUE(s.exec(std::make_unique<DeleteTimingCommand>(
        TimingKind::Bpm, 1, Rational(1, 2))));
    ASSERT_EQ(s.chart().bpm_events.size(), 1u);
    EXPECT_DOUBLE_EQ(s.chart().bpm_events[0].value.value, 130.0);
    // undo 恢复
    ASSERT_TRUE(s.undo());
    EXPECT_EQ(norm_evs(s.chart().bpm_events), norm_evs(make_chart().bpm_events));
}

TEST(TimingCommands, DeleteMissingNoop) {
    EditorSession s;
    s.load(make_chart());
    const auto before = norm_evs(s.chart().bpm_events);
    ASSERT_TRUE(s.exec(std::make_unique<DeleteTimingCommand>(
        TimingKind::Bpm, 9, Rational(0, 1))));
    EXPECT_EQ(norm_evs(s.chart().bpm_events), before);
    ASSERT_TRUE(s.undo());  // 空命令 undo 也无变化
    EXPECT_EQ(norm_evs(s.chart().bpm_events), before);
}

TEST(TimingCommands, DeleteStop) {
    EditorSession s;
    s.load(make_chart());
    ASSERT_TRUE(s.exec(std::make_unique<DeleteTimingCommand>(
        TimingKind::Stop, 1, Rational(1, 2))));
    EXPECT_TRUE(s.chart().stop_events.empty());
    ASSERT_TRUE(s.undo());
    EXPECT_EQ(norm_evs(s.chart().stop_events), norm_evs(make_chart().stop_events));
}

TEST(TimingCommands, DeleteMeasure) {
    EditorSession s;
    s.load(make_chart());
    ASSERT_TRUE(s.exec(std::make_unique<DeleteTimingCommand>(
        TimingKind::Measure, 1, Rational(0, 1))));
    EXPECT_TRUE(s.chart().measure_events.empty());
    ASSERT_TRUE(s.undo());
    EXPECT_EQ(norm_evs(s.chart().measure_events), norm_evs(make_chart().measure_events));
}

// —— 协议 dispatch ——

TEST(TimingCommands, ProtocolPutListDelete) {
    using beatbench::cmd::global_registry;
    auto& session = beatbench::edit::global_editor_session();
    session.load(make_chart());

    // timing.list
    Json req = Json::object();
    req.set("command", "timing.list");
    Json args = Json::object();
    args.set("kind", "bpm");
    req.set("args", std::move(args));
    Json resp = global_registry().dispatch(req);
    ASSERT_TRUE(resp.at("ok").as_bool()) << resp.dump();
    EXPECT_EQ(resp.at("result").at("events").as_array().size(), 2u);

    // timing.put（改值 + 新增）
    req = Json::object();
    req.set("command", "timing.put");
    args = Json::object();
    args.set("kind", "bpm");
    args.set("measure", 1);
    Json pos = Json::object();
    pos.set("num", 0);
    pos.set("den", 1);
    args.set("pos", std::move(pos));
    args.set("value", 180.0);
    req.set("args", std::move(args));
    resp = global_registry().dispatch(req);
    ASSERT_TRUE(resp.at("ok").as_bool()) << resp.dump();
    EXPECT_EQ(session.chart().bpm_events.size(), 2u);  // 同位替换
    EXPECT_DOUBLE_EQ(session.chart().bpm_events[0].value.value, 180.0);

    // timing.delete
    req = Json::object();
    req.set("command", "timing.delete");
    args = Json::object();
    args.set("kind", "bpm");
    args.set("measure", 1);
    pos = Json::object();
    pos.set("num", 0);
    pos.set("den", 1);
    args.set("pos", std::move(pos));
    req.set("args", std::move(args));
    resp = global_registry().dispatch(req);
    ASSERT_TRUE(resp.at("ok").as_bool()) << resp.dump();
    ASSERT_EQ(session.chart().bpm_events.size(), 1u);
    EXPECT_DOUBLE_EQ(session.chart().bpm_events[0].value.value, 170.0);
}

TEST(TimingCommands, ProtocolBadKind) {
    using beatbench::cmd::global_registry;
    auto& session = beatbench::edit::global_editor_session();
    session.load(make_chart());
    Json req = Json::object();
    req.set("command", "timing.put");
    Json args = Json::object();
    args.set("kind", "nosuch");
    req.set("args", std::move(args));
    const Json resp = global_registry().dispatch(req);
    EXPECT_FALSE(resp.at("ok").as_bool());
    EXPECT_EQ(resp.at("error").at("code").as_str(), "bad_args");
}

// —— 手动绑定 id（ref）：timing.put 带 ref → 事件保持 #BPMxx 引用；list 输出 ref ——

TEST(TimingCommands, ProtocolPutWithRefBindsId) {
    using beatbench::cmd::global_registry;
    auto& session = beatbench::edit::global_editor_session();
    session.load(make_chart());
    // 一个 BPM 事件，手动绑定 #BPM01（ref="01" → id 1）
    Json req = Json::object();
    req.set("command", "timing.put");
    Json args = Json::object();
    args.set("kind", "bpm");
    args.set("measure", 1);
    Json pos = Json::object();
    pos.set("num", 0);
    pos.set("den", 1);
    args.set("pos", std::move(pos));
    args.set("value", 180.0);
    args.set("ref", "01");
    req.set("args", std::move(args));
    ASSERT_TRUE(global_registry().dispatch(req).at("ok").as_bool());
    ASSERT_EQ(session.chart().bpm_events.size(), 2u);
    ASSERT_TRUE(session.chart().bpm_events[0].value.ref_id.has_value());
    EXPECT_EQ(*session.chart().bpm_events[0].value.ref_id, 1u);
    // timing.list 应输出 ref="01"
    req = Json::object();
    req.set("command", "timing.list");
    args = Json::object();
    args.set("kind", "bpm");
    req.set("args", std::move(args));
    const Json resp = global_registry().dispatch(req);
    ASSERT_TRUE(resp.at("ok").as_bool()) << resp.dump();
    const auto& evs = resp.at("result").at("events").as_array();
    ASSERT_EQ(evs.size(), 2u);
    EXPECT_EQ(evs[0].at("ref").as_str(), "01");
    // undo → ref 清除，值恢复 130
    ASSERT_TRUE(session.undo());
    EXPECT_DOUBLE_EQ(session.chart().bpm_events[0].value.value, 130.0);
    ASSERT_FALSE(session.chart().bpm_events[0].value.ref_id.has_value());
    EXPECT_EQ(session.chart().bpm_events.size(), 2u);
}

// —— 排序保持：插入后容器仍按 (measure,pos) 升序 ——

TEST(TimingCommands, InsertKeepsOrder) {
    EditorSession s;
    s.load(make_chart());
    // 插到已有序事件的中间（m1 pos0 与 pos1/2 之间 → pos1/4）
    ASSERT_TRUE(s.exec(std::make_unique<PutTimingCommand>(
        TimingKind::Bpm, 1, Rational(1, 4), 150.0)));
    const auto& evs = s.chart().bpm_events;
    ASSERT_EQ(evs.size(), 3u);
    EXPECT_EQ(evs[0].pos, Rational(0, 1));
    EXPECT_EQ(evs[1].pos, Rational(1, 4));
    EXPECT_EQ(evs[2].pos, Rational(1, 2));
}
