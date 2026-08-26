// SPDX-License-Identifier: GPL-3.0-only
// BGA/BMP 命令测试（2026-09）：bga.put/delete/list + sample.setFile/rename 的 kind=bmp。
// 目标：完整编辑 BMS 谱面的 BGA（base/poor/layer/layer2）与 #BMP 定义。
#include <gtest/gtest.h>

#include <memory>
#include <string>

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
    return c;
}

}  // namespace

// —— bga.put：放置 + undo ——

TEST(Bga, PutAndUndo) {
    EditorSession s;
    s.load(make_chart());
    ASSERT_TRUE(s.exec(std::make_unique<BgaPutCommand>(1, Rational(0, 1), 0, 11)));
    ASSERT_TRUE(s.exec(std::make_unique<BgaPutCommand>(1, Rational(1, 2), 2, 22)));
    ASSERT_EQ(s.chart().bga_events.size(), 2u);
    EXPECT_EQ(s.chart().bga_events[0].value.layer, 0);
    EXPECT_EQ(s.chart().bga_events[0].value.image.id, 11u);
    EXPECT_EQ(s.chart().bga_events[1].value.layer, 2);
    EXPECT_EQ(s.chart().bga_events[1].value.image.id, 22u);
    ASSERT_TRUE(s.undo());
    ASSERT_EQ(s.chart().bga_events.size(), 1u);
    ASSERT_TRUE(s.undo());
    EXPECT_TRUE(s.chart().bga_events.empty());
}

TEST(Bga, PutSamePosLayerReplace) {
    EditorSession s;
    s.load(make_chart());
    ASSERT_TRUE(s.exec(std::make_unique<BgaPutCommand>(1, Rational(0, 1), 0, 11)));
    // 同 (measure,pos,layer) 再放 → 同位替换（改 BMP id），容器大小不变
    ASSERT_TRUE(s.exec(std::make_unique<BgaPutCommand>(1, Rational(0, 1), 0, 33)));
    ASSERT_EQ(s.chart().bga_events.size(), 1u);
    EXPECT_EQ(s.chart().bga_events[0].value.image.id, 33u);
    ASSERT_TRUE(s.undo());  // 恢复 11
    EXPECT_EQ(s.chart().bga_events[0].value.image.id, 11u);
    ASSERT_TRUE(s.undo());  // 删除
    EXPECT_TRUE(s.chart().bga_events.empty());
}

TEST(Bga, DifferentLayersSamePosCoexist) {
    EditorSession s;
    s.load(make_chart());
    ASSERT_TRUE(s.exec(std::make_unique<BgaPutCommand>(1, Rational(0, 1), 0, 11)));
    ASSERT_TRUE(s.exec(std::make_unique<BgaPutCommand>(1, Rational(0, 1), 3, 44)));
    ASSERT_EQ(s.chart().bga_events.size(), 2u);  // base + layer2 同 pos 共存
}

TEST(Bga, DeleteAndUndo) {
    EditorSession s;
    s.load(make_chart());
    ASSERT_TRUE(s.exec(std::make_unique<BgaPutCommand>(1, Rational(1, 2), 1, 55)));
    ASSERT_TRUE(s.exec(std::make_unique<BgaDeleteCommand>(1, Rational(1, 2), 1)));
    EXPECT_TRUE(s.chart().bga_events.empty());
    ASSERT_TRUE(s.undo());  // 恢复
    ASSERT_EQ(s.chart().bga_events.size(), 1u);
    EXPECT_EQ(s.chart().bga_events[0].value.image.id, 55u);
    EXPECT_EQ(s.chart().bga_events[0].value.layer, 1);
}

TEST(Bga, DeleteNoOp) {
    EditorSession s;
    s.load(make_chart());
    ASSERT_TRUE(s.exec(std::make_unique<BgaDeleteCommand>(5, Rational(0, 1), 0)));  // 不存在
    EXPECT_TRUE(s.chart().bga_events.empty());
}

// —— 协议 dispatch ——

TEST(Bga, ProtocolListPutDelete) {
    auto& session = beatbench::edit::global_editor_session();
    session.load(make_chart());
    // put base（layer 传整数 0）
    Json req = Json::object();
    req.set("command", "bga.put");
    Json args = Json::object();
    args.set("layer", 0);
    args.set("measure", 1);
    Json pos = Json::object();
    pos.set("num", 0);
    pos.set("den", 1);
    args.set("pos", std::move(pos));
    args.set("sample", 11);
    req.set("args", std::move(args));
    ASSERT_TRUE(global_registry().dispatch(req).at("ok").as_bool());
    // list（layer 传字符串 "base"）
    req = Json::object();
    req.set("command", "bga.list");
    args = Json::object();
    args.set("layer", "base");
    req.set("args", std::move(args));
    const Json resp = global_registry().dispatch(req);
    ASSERT_TRUE(resp.at("ok").as_bool());
    const auto& evs = resp.at("result").at("events").as_array();
    ASSERT_EQ(evs.size(), 1u);
    EXPECT_EQ(evs[0].at("sample").as_i64(), 11);
    EXPECT_EQ(evs[0].at("layer").as_i64(), 0);
    EXPECT_EQ(evs[0].at("measure").as_i64(), 1);
    // delete
    req = Json::object();
    req.set("command", "bga.delete");
    args = Json::object();
    args.set("layer", 0);
    args.set("measure", 1);
    pos = Json::object();
    pos.set("num", 0);
    pos.set("den", 1);
    args.set("pos", std::move(pos));
    req.set("args", std::move(args));
    ASSERT_TRUE(global_registry().dispatch(req).at("ok").as_bool());
    EXPECT_TRUE(session.chart().bga_events.empty());
}

TEST(Bga, ProtocolPutBadLayer) {
    auto& session = beatbench::edit::global_editor_session();
    session.load(make_chart());
    Json req = Json::object();
    req.set("command", "bga.put");
    Json args = Json::object();
    args.set("layer", 9);  // 非法层号
    args.set("measure", 1);
    Json pos = Json::object();
    pos.set("num", 0);
    pos.set("den", 1);
    args.set("pos", std::move(pos));
    args.set("sample", 11);
    req.set("args", std::move(args));
    const Json resp = global_registry().dispatch(req);
    EXPECT_FALSE(resp.at("ok").as_bool());
    EXPECT_EQ(resp.at("error").at("code").as_str(), "bad_args");
    EXPECT_TRUE(session.chart().bga_events.empty());  // 未产生脏数据
}

// —— sample.setFile / sample.rename 的 kind=bmp（#BMP 定义） ——

TEST(Bga, SampleSetFileAndRenameBmp) {
    auto& session = beatbench::edit::global_editor_session();
    session.load(make_chart());
    // 添加 #BMP 定义（kind=bmp）
    Json req = Json::object();
    req.set("command", "sample.setFile");
    Json args = Json::object();
    args.set("id", "01");
    args.set("file", "bg.png");
    args.set("kind", "bmp");
    req.set("args", std::move(args));
    ASSERT_TRUE(global_registry().dispatch(req).at("ok").as_bool());
    ASSERT_EQ(session.chart().samples.count({SampleKind::Bmp, 1}), 1);
    EXPECT_EQ(session.chart().samples.at({SampleKind::Bmp, 1}).file, "bg.png");
    // rename #BMP 定义（kind=bmp）
    req = Json::object();
    req.set("command", "sample.rename");
    args = Json::object();
    args.set("from", "01");
    args.set("to", "02");
    args.set("kind", "bmp");
    req.set("args", std::move(args));
    ASSERT_TRUE(global_registry().dispatch(req).at("ok").as_bool());
    EXPECT_EQ(session.chart().samples.count({SampleKind::Bmp, 2}), 1);
    EXPECT_EQ(session.chart().samples.at({SampleKind::Bmp, 2}).file, "bg.png");
    EXPECT_EQ(session.chart().samples.count({SampleKind::Bmp, 1}), 0);
}

TEST(Bga, SampleSetFileDefaultWavUnchanged) {
    auto& session = beatbench::edit::global_editor_session();
    session.load(make_chart());
    Json req = Json::object();
    req.set("command", "sample.setFile");
    Json args = Json::object();
    args.set("id", "01");
    args.set("file", "kick.wav");
    req.set("args", std::move(args));
    ASSERT_TRUE(global_registry().dispatch(req).at("ok").as_bool());
    EXPECT_EQ(session.chart().samples.count({SampleKind::Wav, 1}), 1);   // 默认 wav
    EXPECT_EQ(session.chart().samples.count({SampleKind::Bmp, 1}), 0);   // 未误建 bmp
}

// —— bga.move / timing.move：单 undo 步移动（值/id 保持） ——

TEST(Bga, ProtocolMoveSingleUndo) {
    auto& session = beatbench::edit::global_editor_session();
    session.load(make_chart());
    // put base 事件 #BMP11 @ m1 pos0
    Json req = Json::object();
    req.set("command", "bga.put");
    Json args = Json::object();
    args.set("layer", 0);
    args.set("measure", 1);
    Json pos = Json::object();
    pos.set("num", 0);
    pos.set("den", 1);
    args.set("pos", std::move(pos));
    args.set("sample", 11);
    req.set("args", std::move(args));
    ASSERT_TRUE(global_registry().dispatch(req).at("ok").as_bool());
    // move → layer2 @ m2 pos1/2（id 11 保持）
    req = Json::object();
    req.set("command", "bga.move");
    args = Json::object();
    Json from = Json::object();
    from.set("layer", 0);
    from.set("measure", 1);
    Json fpos = Json::object();
    fpos.set("num", 0);
    fpos.set("den", 1);
    from.set("pos", std::move(fpos));
    args.set("from", std::move(from));
    Json to = Json::object();
    to.set("layer", 2);
    to.set("measure", 2);
    Json tpos = Json::object();
    tpos.set("num", 1);
    tpos.set("den", 2);
    to.set("pos", std::move(tpos));
    args.set("to", std::move(to));
    req.set("args", std::move(args));
    ASSERT_TRUE(global_registry().dispatch(req).at("ok").as_bool());
    ASSERT_EQ(session.chart().bga_events.size(), 1u);  // 移动 = 一个事件（非两个）
    EXPECT_EQ(session.chart().bga_events[0].measure, 2u);
    EXPECT_EQ(session.chart().bga_events[0].value.layer, 2);
    EXPECT_EQ(session.chart().bga_events[0].value.image.id, 11u);
    // undo → 恢复 m1 pos0 layer0
    ASSERT_TRUE(session.undo());
    ASSERT_EQ(session.chart().bga_events.size(), 1u);
    EXPECT_EQ(session.chart().bga_events[0].measure, 1u);
    EXPECT_EQ(session.chart().bga_events[0].value.layer, 0);
    EXPECT_EQ(session.chart().bga_events[0].value.image.id, 11u);
}

TEST(Bga, ProtocolTimingMoveSingleUndo) {
    auto& session = beatbench::edit::global_editor_session();
    session.load(make_chart());
    // put bpm 180 @ m1 pos0
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
    req.set("args", std::move(args));
    ASSERT_TRUE(global_registry().dispatch(req).at("ok").as_bool());
    // move → m2
    req = Json::object();
    req.set("command", "timing.move");
    args = Json::object();
    args.set("kind", "bpm");
    Json from = Json::object();
    from.set("measure", 1);
    Json fpos = Json::object();
    fpos.set("num", 0);
    fpos.set("den", 1);
    from.set("pos", std::move(fpos));
    args.set("from", std::move(from));
    Json to = Json::object();
    to.set("measure", 2);
    Json tpos = Json::object();
    tpos.set("num", 0);
    tpos.set("den", 1);
    to.set("pos", std::move(tpos));
    args.set("to", std::move(to));
    req.set("args", std::move(args));
    ASSERT_TRUE(global_registry().dispatch(req).at("ok").as_bool());
    ASSERT_EQ(session.chart().bpm_events.size(), 1u);
    EXPECT_EQ(session.chart().bpm_events[0].measure, 2u);
    EXPECT_EQ(session.chart().bpm_events[0].value.value, 180.0);
    // undo → m1
    ASSERT_TRUE(session.undo());
    EXPECT_EQ(session.chart().bpm_events[0].measure, 1u);
    EXPECT_EQ(session.chart().bpm_events[0].value.value, 180.0);
}

// —— note.convertBack：BGA/BPM/STOP → note（反转换；单 undo 步） ——

TEST(Bga, ProtocolConvertBackToNote) {
    auto& session = beatbench::edit::global_editor_session();
    session.load(make_chart());
    // put bga base #BMP11 @ m1 pos0
    Json req = Json::object();
    req.set("command", "bga.put");
    Json args = Json::object();
    args.set("layer", 0);
    args.set("measure", 1);
    Json pos = Json::object();
    pos.set("num", 0);
    pos.set("den", 1);
    args.set("pos", std::move(pos));
    args.set("sample", 11);
    req.set("args", std::move(args));
    ASSERT_TRUE(global_registry().dispatch(req).at("ok").as_bool());
    // convertBack → key1 note @ m2 pos0（id 11 保持）
    req = Json::object();
    req.set("command", "note.convertBack");
    args = Json::object();
    args.set("kind", "bga");
    Json source = Json::object();
    source.set("layer", 0);
    source.set("measure", 1);
    Json spos = Json::object();
    spos.set("num", 0);
    spos.set("den", 1);
    source.set("pos", std::move(spos));
    args.set("source", std::move(source));
    Json target = Json::object();
    Json lane = Json::object();
    lane.set("player", 0);
    lane.set("kind", "key");
    lane.set("index", 1);
    target.set("lane", std::move(lane));
    args.set("target", std::move(target));
    Json to = Json::object();
    to.set("measure", 2);
    Json tpos = Json::object();
    tpos.set("num", 0);
    tpos.set("den", 1);
    to.set("pos", std::move(tpos));
    args.set("to", std::move(to));
    req.set("args", std::move(args));
    ASSERT_TRUE(global_registry().dispatch(req).at("ok").as_bool());
    EXPECT_TRUE(session.chart().bga_events.empty());
    ASSERT_EQ(session.chart().notes.size(), 1u);
    EXPECT_EQ(session.chart().notes[0].measure, 2u);
    EXPECT_EQ(session.chart().notes[0].value.lane.kind, LaneKind::Key);
    EXPECT_EQ(session.chart().notes[0].value.lane.index, 1);
    EXPECT_EQ(session.chart().notes[0].value.sample.id, 11u);
    // undo → bga 恢复
    ASSERT_TRUE(session.undo());
    ASSERT_EQ(session.chart().bga_events.size(), 1u);
    EXPECT_EQ(session.chart().bga_events[0].value.layer, 0);
    EXPECT_EQ(session.chart().bga_events[0].value.image.id, 11u);
    EXPECT_TRUE(session.chart().notes.empty());
}

// —— sample.delete（定义表解绑；引用保留原 id；undo 恢复） ——

TEST(Bga, ProtocolSampleDeleteBmpKeepsRef) {
    auto& session = beatbench::edit::global_editor_session();
    session.load(make_chart());
    // 添加 #BMP 定义（base36 "0B" = 11）+ 放一个引用它的 BGA 事件（sample 数值 11）
    {
        Json req = Json::object();
        req.set("command", "sample.setFile");
        Json args = Json::object();
        args.set("id", "0B");
        args.set("file", "bg.png");
        args.set("kind", "bmp");
        req.set("args", std::move(args));
        ASSERT_TRUE(global_registry().dispatch(req).at("ok").as_bool());
    }
    // setFile 后应立即存在定义
    EXPECT_EQ(session.chart().samples.count({SampleKind::Bmp, 11u}), 1u);
    {
        Json req = Json::object();
        req.set("command", "bga.put");
        Json args = Json::object();
        args.set("layer", 0);
        args.set("measure", 1);
        Json pos = Json::object();
        pos.set("num", 0);
        pos.set("den", 1);
        args.set("pos", std::move(pos));
        args.set("sample", 11);
        req.set("args", std::move(args));
        ASSERT_TRUE(global_registry().dispatch(req).at("ok").as_bool());
    }
    ASSERT_EQ(session.chart().samples.count({SampleKind::Bmp, 11u}), 1u);
    // 删除定义 → 定义移除，但 BGA 引用仍保留 id 11（只解绑文件，不删引用）
    {
        Json req = Json::object();
        req.set("command", "sample.delete");
        Json args = Json::object();
        args.set("id", "0B");
        args.set("kind", "bmp");
        req.set("args", std::move(args));
        ASSERT_TRUE(global_registry().dispatch(req).at("ok").as_bool());
    }
    EXPECT_EQ(session.chart().samples.count({SampleKind::Bmp, 11u}), 0u);
    ASSERT_EQ(session.chart().bga_events.size(), 1u);
    EXPECT_EQ(session.chart().bga_events[0].value.image.id, 11u);
    // undo → 定义恢复
    ASSERT_TRUE(session.undo());
    EXPECT_EQ(session.chart().samples.count({SampleKind::Bmp, 11u}), 1u);
}
