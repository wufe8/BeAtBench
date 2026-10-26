// SPDX-License-Identifier: GPL-3.0-only
// 变换/量化命令测试：note.quantize（吸附网格）/ note.transform（镜像/旋转）的
// apply/invert 精确往返、批量 Composite 一个 undo 步、协议 dispatch。
// 依据 M3 设计「量化/镜像/旋转（LR2 式 mirror/random）」。
#include <gtest/gtest.h>

#include <algorithm>
#include <memory>
#include <stdexcept>
#include <string>
#include <tuple>
#include <vector>

#include "beatbench/core/Chart.hpp"
#include "beatbench/core/ChartMode.hpp"
#include "beatbench/core/bms/BmsCodec.hpp"
#include "beatbench/core/command/Command.hpp"
#include "beatbench/core/edit/EditorSession.hpp"
#include "beatbench/core/edit/SessionRegistry.hpp"

using namespace beatbench;
using namespace beatbench::edit;
using beatbench::cmd::global_registry;
using beatbench::json::Json;

namespace {

Chart make_chart() {
    Chart c;
    c.meta["TITLE"] = "测试";
    c.meta["PLAYER"] = "1";
    c.meta["BPM"] = "130";
    // 三个 note：m1 pos1/3 key1 s1；m1 pos1/2 key2 s2；m1 pos0 key7 s3
    Event<Note> n1{1, Rational(1, 3), {}};
    n1.value.lane = {0, LaneKind::Key, 1};
    n1.value.sample.id = 1;
    Event<Note> n2{1, Rational(1, 2), {}};
    n2.value.lane = {0, LaneKind::Key, 2};
    n2.value.sample.id = 2;
    Event<Note> n3{1, Rational(0, 1), {}};
    n3.value.lane = {0, LaneKind::Key, 7};
    n3.value.sample.id = 3;
    c.notes = {n1, n2, n3};
    return c;
}

std::vector<std::tuple<std::uint32_t, Rational, Lane, std::uint32_t>> norm_notes(
    const std::vector<Event<Note>>& notes) {
    std::vector<std::tuple<std::uint32_t, Rational, Lane, std::uint32_t>> out;
    for (const auto& e : notes) {
        out.emplace_back(e.measure, e.pos, e.value.lane, e.value.sample.id);
    }
    std::sort(out.begin(), out.end());
    return out;
}

}  // namespace

// —— 量化 ——

TEST(TransformQuantize, QuantizeSnapsToGrid) {
    EditorSession s;
    s.load(make_chart());
    // 量化 m1 pos1/3 → 1/16 网格：1/3 ≈ 0.333，round(0.333*16)=5 → 5/16
    ASSERT_TRUE(s.exec(std::make_unique<QuantizeNoteCommand>(
        NoteRef{1, Rational(1, 3), Lane{0, LaneKind::Key, 1}, 1}, 1, 16)));
    // 找到该 note（值定位：新 pos 5/16）
    bool found = false;
    for (const auto& e : s.chart().notes) {
        if (e.value.lane == Lane{0, LaneKind::Key, 1} && e.value.sample.id == 1) {
            EXPECT_EQ(e.pos, Rational(5, 16));  // 1/3 → 5/16
            found = true;
        }
    }
    EXPECT_TRUE(found);
    // 其它 note 不动
    EXPECT_EQ(s.chart().notes.size(), 3u);
    // undo 恢复 1/3
    ASSERT_TRUE(s.undo());
    EXPECT_EQ(norm_notes(s.chart().notes), norm_notes(make_chart().notes));
}

TEST(TransformQuantize, QuantizeAlreadyOnGridNoop) {
    EditorSession s;
    s.load(make_chart());
    // pos 1/2 已在 1/16 网格 → 无变化
    ASSERT_TRUE(s.exec(std::make_unique<QuantizeNoteCommand>(
        NoteRef{1, Rational(1, 2), Lane{0, LaneKind::Key, 2}, 2}, 1, 16)));
    EXPECT_EQ(norm_notes(s.chart().notes), norm_notes(make_chart().notes));
    // undo 也无变化（空命令）
    ASSERT_TRUE(s.undo());
    EXPECT_EQ(norm_notes(s.chart().notes), norm_notes(make_chart().notes));
}

TEST(TransformQuantize, QuantizeCustomSnap) {
    EditorSession s;
    s.load(make_chart());
    // snap 3/16：1/3 ≈ 0.333，round(0.333*16/3)=round(1.777)=2 → 6/16 = 3/8
    ASSERT_TRUE(s.exec(std::make_unique<QuantizeNoteCommand>(
        NoteRef{1, Rational(1, 3), Lane{0, LaneKind::Key, 1}, 1}, 3, 16)));
    bool found = false;
    for (const auto& e : s.chart().notes) {
        if (e.value.lane == Lane{0, LaneKind::Key, 1} && e.value.sample.id == 1) {
            EXPECT_EQ(e.pos, Rational(3, 8));  // 6/16 约分
            found = true;
        }
    }
    EXPECT_TRUE(found);
}

// —— 变换：镜像 ——

TEST(TransformQuantize, MirrorFlipsKeys) {
    EditorSession s;
    s.load(make_chart());
    // 镜像全部 3 个 note
    auto comp = std::make_unique<CompositeCommand>();
    comp->add(std::make_unique<TransformNoteCommand>(
        NoteRef{1, Rational(1, 3), Lane{0, LaneKind::Key, 1}, 1}, true, 0));
    comp->add(std::make_unique<TransformNoteCommand>(
        NoteRef{1, Rational(1, 2), Lane{0, LaneKind::Key, 2}, 2}, true, 0));
    comp->add(std::make_unique<TransformNoteCommand>(
        NoteRef{1, Rational(0, 1), Lane{0, LaneKind::Key, 7}, 3}, true, 0));
    ASSERT_TRUE(s.exec(std::move(comp)));
    EXPECT_EQ(s.undo_depth(), 1u);
    // key1→key7, key2→key6, key7→key1
    const auto& notes = s.chart().notes;
    bool a = false, b = false, c_ok = false;
    for (const auto& e : notes) {
        if (e.pos == Rational(1, 3) && e.value.lane == Lane{0, LaneKind::Key, 7} &&
            e.value.sample.id == 1)
            a = true;
        if (e.pos == Rational(1, 2) && e.value.lane == Lane{0, LaneKind::Key, 6} &&
            e.value.sample.id == 2)
            b = true;
        if (e.pos == Rational(0, 1) && e.value.lane == Lane{0, LaneKind::Key, 1} &&
            e.value.sample.id == 3)
            c_ok = true;
    }
    EXPECT_TRUE(a);
    EXPECT_TRUE(b);
    EXPECT_TRUE(c_ok);
    // 一次 undo 全回原位
    ASSERT_TRUE(s.undo());
    EXPECT_EQ(norm_notes(s.chart().notes), norm_notes(make_chart().notes));
}

// —— 变换：旋转 ——

TEST(TransformQuantize, RotateCyclesKeys) {
    EditorSession s;
    s.load(make_chart());
    // 旋转 +1：key1→key2, key2→key3, key7→key1
    auto comp = std::make_unique<CompositeCommand>();
    comp->add(std::make_unique<TransformNoteCommand>(
        NoteRef{1, Rational(1, 3), Lane{0, LaneKind::Key, 1}, 1}, false, 1));
    comp->add(std::make_unique<TransformNoteCommand>(
        NoteRef{1, Rational(1, 2), Lane{0, LaneKind::Key, 2}, 2}, false, 1));
    comp->add(std::make_unique<TransformNoteCommand>(
        NoteRef{1, Rational(0, 1), Lane{0, LaneKind::Key, 7}, 3}, false, 1));
    ASSERT_TRUE(s.exec(std::move(comp)));
    const auto& notes = s.chart().notes;
    bool a = false, b = false, c_ok = false;
    for (const auto& e : notes) {
        if (e.pos == Rational(1, 3) && e.value.lane == Lane{0, LaneKind::Key, 2} &&
            e.value.sample.id == 1)
            a = true;
        if (e.pos == Rational(1, 2) && e.value.lane == Lane{0, LaneKind::Key, 3} &&
            e.value.sample.id == 2)
            b = true;
        if (e.pos == Rational(0, 1) && e.value.lane == Lane{0, LaneKind::Key, 1} &&
            e.value.sample.id == 3)
            c_ok = true;
    }
    EXPECT_TRUE(a);
    EXPECT_TRUE(b);
    EXPECT_TRUE(c_ok);
    ASSERT_TRUE(s.undo());
    EXPECT_EQ(norm_notes(s.chart().notes), norm_notes(make_chart().notes));
}

// —— 协议 dispatch ——

TEST(TransformQuantize, ProtocolQuantize) {
    using beatbench::cmd::global_registry;
    auto& session = beatbench::edit::global_editor_session();
    session.load(make_chart());

    Json req = Json::object();
    req.set("command", "note.quantize");
    Json args = Json::object();
    Json sel = Json::array();
    Json item = Json::object();
    item.set("measure", 1);
    Json pos = Json::object();
    pos.set("num", 1);
    pos.set("den", 3);
    item.set("pos", std::move(pos));
    Json lane = Json::object();
    lane.set("player", 0);
    lane.set("kind", "key");
    lane.set("index", 1);
    item.set("lane", std::move(lane));
    item.set("sample", 1);
    sel.push_back(std::move(item));
    args.set("selection", std::move(sel));
    Json snap = Json::object();
    snap.set("num", 1);
    snap.set("den", 16);
    args.set("snap", std::move(snap));
    req.set("args", std::move(args));

    const Json resp = global_registry().dispatch(req);
    ASSERT_TRUE(resp.at("ok").as_bool()) << resp.dump();
    EXPECT_EQ(resp.at("result").at("notes").as_i64(), 1);
    bool found = false;
    for (const auto& e : session.chart().notes) {
        if (e.value.lane == Lane{0, LaneKind::Key, 1} && e.value.sample.id == 1) {
            EXPECT_EQ(e.pos, Rational(5, 16));
            found = true;
        }
    }
    EXPECT_TRUE(found);
}

TEST(TransformQuantize, ProtocolTransformMirror) {
    using beatbench::cmd::global_registry;
    auto& session = beatbench::edit::global_editor_session();
    session.load(make_chart());

    Json req = Json::object();
    req.set("command", "note.transform");
    Json args = Json::object();
    Json sel = Json::array();
    Json item = Json::object();
    item.set("measure", 1);
    Json pos = Json::object();
    pos.set("num", 0);
    pos.set("den", 1);
    item.set("pos", std::move(pos));
    Json lane = Json::object();
    lane.set("player", 0);
    lane.set("kind", "key");
    lane.set("index", 7);
    item.set("lane", std::move(lane));
    item.set("sample", 3);
    sel.push_back(std::move(item));
    args.set("selection", std::move(sel));
    args.set("mirror", true);
    req.set("args", std::move(args));

    const Json resp = global_registry().dispatch(req);
    ASSERT_TRUE(resp.at("ok").as_bool()) << resp.dump();
    // key7 → key1
    bool found = false;
    for (const auto& e : session.chart().notes) {
        if (e.value.lane == Lane{0, LaneKind::Key, 1} && e.value.sample.id == 3) {
            found = true;
        }
    }
    EXPECT_TRUE(found);
    // undo 恢复
    req = Json::object();
    req.set("command", "session.undo");
    const Json resp2 = global_registry().dispatch(req);
    ASSERT_TRUE(resp2.at("ok").as_bool()) << resp2.dump();
    EXPECT_EQ(norm_notes(session.chart().notes), norm_notes(make_chart().notes));
}

TEST(TransformQuantize, ProtocolEmptySelection) {
    using beatbench::cmd::global_registry;
    auto& session = beatbench::edit::global_editor_session();
    session.load(make_chart());
    Json req = Json::object();
    req.set("command", "note.quantize");
    Json args = Json::object();
    args.set("selection", Json::array());
    req.set("args", std::move(args));
    const Json resp = global_registry().dispatch(req);
    EXPECT_FALSE(resp.at("ok").as_bool());
    EXPECT_EQ(resp.at("error").at("code").as_str(), "empty_selection");
}

namespace {

Chart collision_chart(bool bmesp) {
    bms::BmsReadOptions opts;
    opts.mode = "pms9k";
    // 两拍的普通音与 LN 头尾都用 sample1，镜像后源/目标值交叉。
    const std::string normal = bmesp ? "17" : "25";
    return bms::read_bms(
               "#BPM 120\n#LNTYPE 1\n#WAV01 hit.wav\n"
               "#00051:01010000\n#000" +
                   normal + ":01010000\n",
               opts)
        .chart;
}

void expect_pairs(const Chart& chart, int ln_key) {
    std::size_t paired = 0;
    for (std::size_t i = 0; i < chart.notes.size(); ++i) {
        const auto& e = chart.notes[i];
        if (!e.value.ln_channel) {
            EXPECT_FALSE(e.value.ln_pair.has_value());
            continue;
        }
        EXPECT_EQ(e.value.lane.index, ln_key);
        ASSERT_TRUE(e.value.ln_pair.has_value());
        const auto p = *e.value.ln_pair;
        ASSERT_LT(p, chart.notes.size());
        EXPECT_EQ(chart.notes[p].value.ln_pair, i);
        EXPECT_EQ(chart.notes[p].value.lane, e.value.lane);
        ++paired;
    }
    EXPECT_EQ(paired, 2u);
}

using FullNoteSig = std::tuple<std::uint32_t, Rational, Lane, std::uint32_t, bool, NoteKind>;
std::vector<FullNoteSig> full_notes(const Chart& chart) {
    std::vector<FullNoteSig> out;
    for (const auto& e : chart.notes)
        out.emplace_back(e.measure, e.pos, e.value.lane, e.value.sample.id, e.value.ln_channel,
                         e.value.kind);
    std::sort(out.begin(), out.end());
    return out;
}

Json transform_request(const Chart& chart, const std::vector<int>& order, bool mirror, int rotate) {
    Json sel = Json::array();
    for (const int i : order) {
        const auto& e = chart.notes[static_cast<std::size_t>(i)];
        Json item = Json::object();
        item.set("measure", static_cast<std::int64_t>(e.measure));
        Json pos = Json::object();
        pos.set("num", e.pos.num);
        pos.set("den", e.pos.den);
        item.set("pos", std::move(pos));
        Json lane = Json::object();
        lane.set("player", static_cast<std::int64_t>(e.value.lane.player));
        lane.set("kind", "key");
        lane.set("index", static_cast<std::int64_t>(e.value.lane.index));
        item.set("lane", std::move(lane));
        item.set("sample", static_cast<std::int64_t>(e.value.sample.id));
        item.set("sub_line", static_cast<std::int64_t>(e.value.sub_line));
        sel.push_back(std::move(item));
    }
    Json args = Json::object();
    args.set("selection", std::move(sel));
    args.set("mirror", mirror);
    args.set("rotate", rotate);
    Json req = Json::object();
    req.set("command", "note.transform");
    req.set("args", std::move(args));
    return req;
}

std::unique_ptr<TransformNoteCommand> mirror_note(const Event<Note>& e) {
    return std::make_unique<TransformNoteCommand>(
        NoteRef{e.measure, e.pos, e.value.lane, e.value.sample.id, e.value.sub_line}, true, 0);
}

class FailingTransformBatchCommand : public EditCommand {
   public:
    std::string name() const override { return "test.fail"; }
    void apply(Chart&) override { throw std::runtime_error("batch rollback"); }
    void invert(Chart&) override {}
};

}  // namespace

TEST(TransformQuantize, BatchCollisionsAllOrdersMirrorRotateUndoRedoRoundTrip) {
    auto& session = global_editor_session();
    for (const bool bmesp : {false, true}) {
        const Chart before = collision_chart(bmesp);
        ASSERT_EQ(before.notes.size(), 4u);
        ASSERT_EQ(before.mode_id.value_or(""), bmesp ? "pms9k-bmesp" : "pms9k");
        expect_pairs(before, 1);
        for (const int operation : {0, 1, -1}) {
            const bool mirror = operation == 0;
            const int ln_key = mirror ? 9 : (operation == 1 ? 2 : 9);
            std::vector<int> order{0, 1, 2, 3};
            do {
                SCOPED_TRACE(::testing::Message()
                             << "bmesp=" << bmesp << " rotate=" << operation
                             << " order=" << order[0] << order[1] << order[2] << order[3]);
                session.load(before);
                const auto response =
                    global_registry().dispatch(transform_request(before, order, mirror, operation));
                ASSERT_TRUE(response.at("ok").as_bool()) << response.dump();
                ASSERT_TRUE(response.at("result").at("ok").as_bool()) << response.dump();
                EXPECT_EQ(session.undo_depth(), 1u);
                Chart expected = before;
                for (auto& e : expected.notes)
                    e.value.lane = transform_lane(e.value.lane, mirror, operation, 9);
                EXPECT_EQ(full_notes(session.chart()), full_notes(expected));
                expect_pairs(session.chart(), ln_key);
                const auto& selection = response.at("result").at("selection").as_array();
                ASSERT_EQ(selection.size(), order.size());
                for (std::size_t i = 0; i < order.size(); ++i) {
                    const auto index = static_cast<std::size_t>(order[i]);
                    EXPECT_EQ(selection[i].at("lane").at("index").as_i64(),
                              expected.notes[index].value.lane.index);
                }
                // 序列化重读后不仅数量/位置/sample 不变，LN 也必须仍互指。
                bms::BmsReadOptions opts;
                opts.mode = "pms9k";
                const auto reopened = bms::read_bms(bms::write_bms(session.chart()), opts);
                EXPECT_EQ(full_notes(reopened.chart), full_notes(expected));
                expect_pairs(reopened.chart, ln_key);
                for (int cycle = 0; cycle < 2; ++cycle) {
                    ASSERT_TRUE(session.undo());
                    EXPECT_EQ(full_notes(session.chart()), full_notes(before));
                    expect_pairs(session.chart(), 1);
                    ASSERT_TRUE(session.redo());
                    EXPECT_EQ(full_notes(session.chart()), full_notes(expected));
                    expect_pairs(session.chart(), ln_key);
                }
            } while (std::next_permutation(order.begin(), order.end()));
        }
    }
}

TEST(TransformQuantize, DetectedBmespUsesNineKeysWithoutBecomingCreationMode) {
    bms::BmsReadOptions opts;
    opts.mode = "pms9k";
    const auto read = bms::read_bms("#BPM 120\n#00016:01\n", opts);
    ASSERT_EQ(read.chart.mode_id.value_or(""), "pms9k-bmesp");
    ASSERT_TRUE(chart_mode_key_count(*read.chart.mode_id).has_value());
    EXPECT_EQ(*chart_mode_key_count(*read.chart.mode_id), 9);
    EXPECT_FALSE(chart_mode_by_id("pms9k-bmesp").has_value());
    EXPECT_FALSE(chart_mode_key_count("unknown").has_value());
    auto& session = global_editor_session();
    session.load(read.chart);
    const auto response = global_registry().dispatch(transform_request(read.chart, {0}, true, 0));
    ASSERT_TRUE(response.at("ok").as_bool()) << response.dump();
    ASSERT_EQ(session.chart().notes.size(), 1u);
    EXPECT_EQ(session.chart().notes[0].value.lane.index, 2);
    EXPECT_EQ(response.at("result").at("selection").as_array()[0].at("lane").at("index").as_i64(),
              2);
    ASSERT_TRUE(session.undo());
    EXPECT_EQ(session.chart().notes[0].value.lane.index, 8);
    ASSERT_TRUE(session.redo());
    EXPECT_EQ(session.chart().notes[0].value.lane.index, 2);
}

TEST(TransformQuantize, SingleTransformUndoDoesNotPickExistingDestination) {
    Chart chart = collision_chart(false);
    // 源排在目标之后，旧 invert 会先命中未选中的普通音。
    std::stable_sort(chart.notes.begin(), chart.notes.end(), [](const auto& a, const auto& b) {
        return !a.value.ln_channel && b.value.ln_channel;
    });
    EditorSession session;
    session.load(chart);
    const auto source = chart.notes[2];
    ASSERT_TRUE(source.value.ln_channel);
    ASSERT_TRUE(session.exec(mirror_note(source)));
    ASSERT_TRUE(session.undo());
    EXPECT_EQ(full_notes(session.chart()), full_notes(chart));
    expect_pairs(session.chart(), 1);
}

TEST(TransformQuantize, MixedNestedCompositeRebindsAfterInsertion) {
    const Chart chart = collision_chart(false);
    EditorSession session;
    session.load(chart);
    auto batch = std::make_unique<CompositeCommand>();
    for (const auto& e : chart.notes) batch->add(mirror_note(e));
    // 插入位于所有原音之前；后续嵌套批次必须看到新容器，不可沿用旧索引。
    batch->add(
        std::make_unique<PutNoteCommand>(NoteRef{0, Rational(0, 1), Lane{0, LaneKind::Key, 5}, 2}));
    auto nested = std::make_unique<CompositeCommand>();
    for (auto e : chart.notes) {
        e.value.lane = transform_lane(e.value.lane, true, 0, 9);
        nested->add(mirror_note(e));
    }
    batch->add(std::move(nested));
    ASSERT_TRUE(session.exec(std::move(batch)));
    EXPECT_EQ(session.chart().notes.size(), chart.notes.size() + 1);
    expect_pairs(session.chart(), 1);
    const auto after = full_notes(session.chart());
    ASSERT_TRUE(session.undo());
    EXPECT_EQ(full_notes(session.chart()), full_notes(chart));
    expect_pairs(session.chart(), 1);
    ASSERT_TRUE(session.redo());
    EXPECT_EQ(full_notes(session.chart()), after);
    expect_pairs(session.chart(), 1);
}

TEST(TransformQuantize, TransformUndoRebindsAfterLaterMoveUndoReordersNotes) {
    Chart chart = collision_chart(false);
    std::stable_sort(chart.notes.begin(), chart.notes.end(), [](const auto& a, const auto& b) {
        if (a.pos != b.pos) return a.pos < b.pos;
        return a.value.ln_channel && !b.value.ln_channel;
    });
    EditorSession session;
    session.load(chart);
    auto batch = std::make_unique<CompositeCommand>();
    for (const auto& e : chart.notes) batch->add(mirror_note(e));
    ASSERT_TRUE(session.exec(std::move(batch)));
    const auto transformed = full_notes(session.chart());
    ASSERT_TRUE(session.exec(std::make_unique<MoveNoteCommand>(
        NoteRef{0, Rational(1, 4), Lane{0, LaneKind::Key, 1}, 1}, 1, Rational(0, 1))));
    ASSERT_TRUE(session.undo());
    EXPECT_EQ(full_notes(session.chart()), transformed);
    // Move 撤销在同拍段首重插，此时原 Transform 的索引已过期。
    EXPECT_FALSE(session.chart().notes[2].value.ln_channel);
    ASSERT_TRUE(session.undo());
    EXPECT_EQ(full_notes(session.chart()), full_notes(chart));
    expect_pairs(session.chart(), 1);
    ASSERT_TRUE(session.redo());
    EXPECT_EQ(full_notes(session.chart()), transformed);
    expect_pairs(session.chart(), 9);
}

TEST(TransformQuantize, IdenticalSourceValuesAreClaimedOncePerObject) {
    Chart chart = collision_chart(false);
    // 同源值重复物件，kind/sub_line 各异；旧代码只会反复命中首项。
    Event<Note> extra{0, Rational(0, 1), {}};
    extra.value.lane = {0, LaneKind::Key, 1};
    extra.value.sample.id = 1;
    extra.value.sub_line = 3;
    chart.notes.push_back(extra);
    extra.value.kind = NoteKind::Landmine;
    chart.notes.push_back(extra);
    chart.notes.push_back(extra);  // 全特征也相同的重复物件仍需逐一认领
    EditorSession session;
    session.load(chart);
    auto batch = std::make_unique<CompositeCommand>();
    for (const auto& e : chart.notes) batch->add(mirror_note(e));
    ASSERT_TRUE(session.exec(std::move(batch)));
    Chart expected = chart;
    for (auto& e : expected.notes) e.value.lane = transform_lane(e.value.lane, true, 0, 9);
    EXPECT_EQ(full_notes(session.chart()), full_notes(expected));
    for (std::size_t i = 4; i < 7; ++i) EXPECT_EQ(session.chart().notes[i].value.sub_line, 3u);
    ASSERT_TRUE(session.undo());
    EXPECT_EQ(full_notes(session.chart()), full_notes(chart));
    expect_pairs(session.chart(), 1);
    ASSERT_TRUE(session.redo());
    EXPECT_EQ(full_notes(session.chart()), full_notes(expected));
}

TEST(TransformQuantize, ProtocolTransformSelectsNonFirstSubLineOnly) {
    Chart chart = collision_chart(false);
    Event<Note> extra{0, Rational(0, 1), {}};
    extra.value.lane = {0, LaneKind::Key, 1};
    extra.value.sample.id = 1;
    extra.value.sub_line = 3;
    chart.notes.push_back(extra);
    auto& session = global_editor_session();
    session.load(chart);
    const auto response = global_registry().dispatch(transform_request(chart, {4}, true, 0));
    ASSERT_TRUE(response.at("ok").as_bool()) << response.dump();
    EXPECT_EQ(session.chart().notes[4].value.lane.index, 9);
    EXPECT_EQ(session.chart().notes[4].value.sub_line, 3u);
    expect_pairs(session.chart(), 1);  // 不应误选同拍同sample的首行 LN 头
    EXPECT_EQ(response.at("result").at("selection").as_array()[0].at("sub_line").as_i64(), 3);
    ASSERT_TRUE(session.undo());
    EXPECT_EQ(full_notes(session.chart()), full_notes(chart));
    expect_pairs(session.chart(), 1);
    ASSERT_TRUE(session.redo());
    EXPECT_EQ(session.chart().notes[4].value.lane.index, 9);
    expect_pairs(session.chart(), 1);
}

TEST(TransformQuantize, FailedMixedBatchRollsBackAndPreservesRedo) {
    const Chart chart = collision_chart(false);
    EditorSession session;
    session.load(chart);
    auto successful = std::make_unique<CompositeCommand>();
    for (const auto& e : chart.notes) successful->add(mirror_note(e));
    ASSERT_TRUE(session.exec(std::move(successful)));
    ASSERT_TRUE(session.undo());
    auto failing = std::make_unique<CompositeCommand>();
    for (const auto& e : chart.notes) failing->add(mirror_note(e));
    failing->add(std::make_unique<FailingTransformBatchCommand>());
    EXPECT_FALSE(session.exec(std::move(failing)));
    EXPECT_EQ(full_notes(session.chart()), full_notes(chart));
    expect_pairs(session.chart(), 1);
    EXPECT_EQ(session.undo_depth(), 0u);
    EXPECT_EQ(session.redo_depth(), 1u);
    ASSERT_TRUE(session.redo());
    expect_pairs(session.chart(), 9);
}
