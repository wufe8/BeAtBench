// SPDX-License-Identifier: GPL-3.0-only
// 编辑命令测试：note.put/move/delete 的 apply/invert 精确往返、merge 合并、
// undo/redo 栈、随机命令序列可逆性（01 §5.6「命令可逆性」守卫）。
// 全部合成 Chart，不依赖 local/。
#include <gtest/gtest.h>

#include <memory>
#include <random>
#include <string>
#include <vector>

#include "beatbench/core/Chart.hpp"
#include "beatbench/core/command/Command.hpp"
#include "beatbench/core/edit/EditorSession.hpp"
#include "beatbench/core/edit/Selection.hpp"
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
    // 两个 note：m1 pos0 key1 sample1；m1 pos1/2 key2 sample2
    Event<Note> n1{1, Rational(0, 1), {}};
    n1.value.lane = {0, LaneKind::Key, 1};
    n1.value.sample.id = 1;
    Event<Note> n2{1, Rational(1, 2), {}};
    n2.value.lane = {0, LaneKind::Key, 2};
    n2.value.sample.id = 2;
    c.notes = {n1, n2};
    return c;
}

// 容器规范化：按 (measure,pos,lane,sample) 排序（忽略 ln_pair，配对单独验证）
std::vector<std::tuple<std::uint32_t, Rational, Lane, std::uint32_t>> norm_notes(
    const std::vector<Event<Note>>& notes) {
    std::vector<std::tuple<std::uint32_t, Rational, Lane, std::uint32_t>> out;
    for (const auto& e : notes) {
        out.emplace_back(e.measure, e.pos, e.value.lane, e.value.sample.id);
    }
    std::sort(out.begin(), out.end());
    return out;
}

bool ln_consistent(const std::vector<Event<Note>>& notes) {
    // 互指一致性：a.ln_pair==b 则 b.ln_pair==a
    for (std::size_t i = 0; i < notes.size(); ++i) {
        const auto p = notes[i].value.ln_pair;
        if (!p) continue;
        if (*p >= notes.size()) return false;
        const auto q = notes[*p].value.ln_pair;
        if (!q || *q != i) return false;
    }
    return true;
}

}  // namespace

// —— put / delete 往返 ——

TEST(EditCommands, PutThenUndoRestores) {
    EditorSession s;
    s.load(make_chart());
    const auto before = s.chart();

    ASSERT_TRUE(s.exec(std::make_unique<PutNoteCommand>(2, Rational(0, 1),
                                                        Lane{0, LaneKind::Key, 3}, 7)));
    ASSERT_EQ(s.chart().notes.size(), before.notes.size() + 1);
    EXPECT_TRUE(ln_consistent(s.chart().notes));

    ASSERT_TRUE(s.undo());
    EXPECT_EQ(norm_notes(s.chart().notes), norm_notes(before.notes));
    EXPECT_EQ(s.chart().meta, before.meta);
}

TEST(EditCommands, PutRedoReapplies) {
    EditorSession s;
    s.load(make_chart());
    ASSERT_TRUE(s.exec(std::make_unique<PutNoteCommand>(2, Rational(0, 1),
                                                        Lane{0, LaneKind::Key, 3}, 7)));
    ASSERT_TRUE(s.undo());
    ASSERT_TRUE(s.redo());
    EXPECT_EQ(s.chart().notes.size(), 3u);
    EXPECT_TRUE(ln_consistent(s.chart().notes));
}

TEST(EditCommands, DeleteThenUndoRestores) {
    EditorSession s;
    s.load(make_chart());
    // 删 m1 pos0 key1 sample1
    ASSERT_TRUE(s.exec(std::make_unique<DeleteNoteCommand>(1, Rational(0, 1),
                                                           Lane{0, LaneKind::Key, 1}, 1)));
    EXPECT_EQ(s.chart().notes.size(), 1u);
    ASSERT_TRUE(s.undo());
    EXPECT_EQ(norm_notes(s.chart().notes), norm_notes(make_chart().notes));
}

TEST(EditCommands, DeleteMissingNoteNoop) {
    EditorSession s;
    s.load(make_chart());
    const auto before = norm_notes(s.chart().notes);
    // 不存在的 note → 命令成功但无变化
    ASSERT_TRUE(s.exec(std::make_unique<DeleteNoteCommand>(9, Rational(0, 1),
                                                           Lane{0, LaneKind::Key, 1}, 1)));
    EXPECT_EQ(norm_notes(s.chart().notes), before);
    // undo 也无变化（空命令）
    ASSERT_TRUE(s.undo());
    EXPECT_EQ(norm_notes(s.chart().notes), before);
}

// —— move 往返 + merge ——

TEST(EditCommands, MoveThenUndoRestores) {
    EditorSession s;
    s.load(make_chart());
    const auto before = norm_notes(s.chart().notes);
    // 移 m1 pos0 key1 → m2 pos1/4
    ASSERT_TRUE(s.exec(std::make_unique<MoveNoteCommand>(1, Rational(0, 1),
                                                         Lane{0, LaneKind::Key, 1}, 1, 2,
                                                         Rational(1, 4))));
    EXPECT_EQ(s.chart().notes.size(), 2u);
    EXPECT_TRUE(ln_consistent(s.chart().notes));
    ASSERT_TRUE(s.undo());
    EXPECT_EQ(norm_notes(s.chart().notes), before);
    ASSERT_TRUE(s.redo());
    // 重做后 note 在目标位置
    bool found = false;
    for (const auto& e : s.chart().notes) {
        if (e.measure == 2 && e.pos == Rational(1, 4) && e.value.lane == (Lane{0, LaneKind::Key, 1})) {
            found = true;
        }
    }
    EXPECT_TRUE(found);
}

TEST(EditCommands, MoveMergeConsecutiveDrags) {
    EditorSession s;
    s.load(make_chart());
    // 连续拖动同一 note：m1 pos0 → m1 pos1/4 → m1 pos1/2，应合并为一个 undo 步
    ASSERT_TRUE(s.exec(std::make_unique<MoveNoteCommand>(1, Rational(0, 1),
                                                         Lane{0, LaneKind::Key, 1}, 1, 1,
                                                         Rational(1, 4))));
    ASSERT_TRUE(s.exec(std::make_unique<MoveNoteCommand>(1, Rational(1, 4),
                                                         Lane{0, LaneKind::Key, 1}, 1, 1,
                                                         Rational(1, 2))));
    EXPECT_EQ(s.undo_depth(), 1u);  // 合并成一个 undo 步
    // undo 一次直接回到原位
    ASSERT_TRUE(s.undo());
    EXPECT_EQ(norm_notes(s.chart().notes), norm_notes(make_chart().notes));
}

TEST(EditCommands, MoveDoesNotMergeDifferentNote) {
    EditorSession s;
    s.load(make_chart());
    ASSERT_TRUE(s.exec(std::make_unique<MoveNoteCommand>(1, Rational(0, 1),
                                                         Lane{0, LaneKind::Key, 1}, 1, 1,
                                                         Rational(1, 4))));
    // 不同 note（key2）→ 不合并
    ASSERT_TRUE(s.exec(std::make_unique<MoveNoteCommand>(1, Rational(1, 2),
                                                         Lane{0, LaneKind::Key, 2}, 2, 1,
                                                         Rational(3, 4))));
    EXPECT_EQ(s.undo_depth(), 2u);
}

// —— LN 配对一致性 ——

TEST(EditCommands, DeleteLnBreaksPair) {
    EditorSession s;
    // 构造 LN 对：两个 key1 note（m1 pos0 与 m1 pos1/2）互指
    Chart c = make_chart();
    c.notes[0].value.lane = {0, LaneKind::Key, 1};  // 确保同 lane
    c.notes[1].value.lane = {0, LaneKind::Key, 1};
    c.notes[0].value.ln_pair = 1;
    c.notes[1].value.ln_pair = 0;
    s.load(std::move(c));
    EXPECT_TRUE(ln_consistent(s.chart().notes));
    // 删头 → 尾配对清空
    ASSERT_TRUE(s.exec(std::make_unique<DeleteNoteCommand>(1, Rational(0, 1),
                                                           Lane{0, LaneKind::Key, 1}, 1)));
    ASSERT_EQ(s.chart().notes.size(), 1u);
    EXPECT_FALSE(s.chart().notes[0].value.ln_pair.has_value());
    EXPECT_TRUE(ln_consistent(s.chart().notes));
    // undo → 配对恢复
    ASSERT_TRUE(s.undo());
    EXPECT_TRUE(ln_consistent(s.chart().notes));
    EXPECT_TRUE(s.chart().notes[0].value.ln_pair.has_value());
    EXPECT_TRUE(s.chart().notes[1].value.ln_pair.has_value());
}

TEST(EditCommands, MoveLnDefaultSingleNoteBreaksPair) {
    // 默认（move_ln_pair=false）：LN 当作单个 note，移动后配对解除
    EditorSession s;
    Chart c = make_chart();
    c.notes[0].value.lane = {0, LaneKind::Key, 1};
    c.notes[1].value.lane = {0, LaneKind::Key, 1};
    c.notes[0].value.ln_pair = 1;
    c.notes[1].value.ln_pair = 0;
    s.load(std::move(c));
    ASSERT_TRUE(s.exec(std::make_unique<MoveNoteCommand>(1, Rational(0, 1),
                                                         Lane{0, LaneKind::Key, 1}, 1, 3,
                                                         Rational(0, 1))));
    EXPECT_TRUE(ln_consistent(s.chart().notes));
    // 只有 1 个 note 到 m3（配对端留在 m1 pos1/2，未随动）
    std::size_t in_m3 = 0;
    for (const auto& e : s.chart().notes) {
        if (e.measure == 3 && e.value.lane == (Lane{0, LaneKind::Key, 1})) ++in_m3;
    }
    EXPECT_EQ(in_m3, 1u);
    // 配对已解除（被移动 note 与留原位的伙伴都无 ln_pair）
    for (const auto& e : s.chart().notes) {
        EXPECT_FALSE(e.value.ln_pair.has_value());
    }
    // undo 精确还原（配对也恢复）
    ASSERT_TRUE(s.undo());
    EXPECT_TRUE(ln_consistent(s.chart().notes));
    EXPECT_TRUE(s.chart().notes[0].value.ln_pair.has_value());
    EXPECT_TRUE(s.chart().notes[1].value.ln_pair.has_value());
}

TEST(EditCommands, MoveLnPairModeMovesBothEnds) {
    // move_ln_pair=true：识别 LN 整体头尾移动（保持相对位置），配对保留
    EditorSession s;
    Chart c = make_chart();
    c.notes[0].value.lane = {0, LaneKind::Key, 1};
    c.notes[1].value.lane = {0, LaneKind::Key, 1};
    c.notes[0].value.ln_pair = 1;
    c.notes[1].value.ln_pair = 0;
    s.load(std::move(c));
    ASSERT_TRUE(s.exec(std::make_unique<MoveNoteCommand>(1, Rational(0, 1),
                                                         Lane{0, LaneKind::Key, 1}, 1, 3,
                                                         Rational(0, 1), true)));
    EXPECT_TRUE(ln_consistent(s.chart().notes));
    // 两个配对 note 都在 m3
    std::size_t in_m3 = 0;
    for (const auto& e : s.chart().notes) {
        if (e.measure == 3 && e.value.lane == (Lane{0, LaneKind::Key, 1})) ++in_m3;
    }
    EXPECT_EQ(in_m3, 2u);
    // undo → 回到 m1/m2，配对保留
    ASSERT_TRUE(s.undo());
    EXPECT_TRUE(ln_consistent(s.chart().notes));
    bool at_m1 = false, at_m2 = false;
    for (const auto& e : s.chart().notes) {
        if (e.measure == 1 && e.pos == Rational(0, 1) && e.value.lane == (Lane{0, LaneKind::Key, 1})) at_m1 = true;
        if (e.measure == 1 && e.pos == Rational(1, 2) && e.value.lane == (Lane{0, LaneKind::Key, 1})) at_m2 = true;
    }
    EXPECT_TRUE(at_m1);
    EXPECT_TRUE(at_m2);
}

// —— 随机序列可逆性（01 §5.6） ——

TEST(EditCommands, RandomSequenceReversible) {
    EditorSession s;
    s.load(make_chart());
    const auto initial = s.chart();
    std::mt19937 rng(42);
    std::uniform_int_distribution<int> measure(1, 5);
    std::uniform_int_distribution<int> pos_den(2, 8);
    std::uniform_int_distribution<int> pos_num(0, 7);
    std::uniform_int_distribution<int> sample(1, 4);
    std::uniform_int_distribution<int> op(0, 2);  // 0=put 1=move 2=delete

    constexpr int kOps = 60;
    for (int i = 0; i < kOps; ++i) {
        const std::uint32_t m = static_cast<std::uint32_t>(measure(rng));
        const Rational pos(pos_num(rng), pos_den(rng));
        const Lane lane{0, LaneKind::Key, static_cast<std::uint8_t>(1 + pos_num(rng) % 7)};
        const std::uint32_t smp = static_cast<std::uint32_t>(sample(rng));
        const int o = op(rng);
        switch (o) {
            case 0:
                ASSERT_TRUE(s.exec(std::make_unique<PutNoteCommand>(m, pos, lane, smp)));
                break;
            case 1: {
                // move 需要 from 存在：找容器里第一个 note 移走
                if (!s.chart().notes.empty()) {
                    const auto& src = s.chart().notes.front();
                    ASSERT_TRUE(s.exec(std::make_unique<MoveNoteCommand>(
                        src.measure, src.pos, src.value.lane, src.value.sample.id, m, pos)));
                }
                break;
            }
            case 2:
                if (!s.chart().notes.empty()) {
                    const auto& src = s.chart().notes.front();
                    ASSERT_TRUE(s.exec(std::make_unique<DeleteNoteCommand>(
                        src.measure, src.pos, src.value.lane, src.value.sample.id)));
                }
                break;
        }
        EXPECT_TRUE(ln_consistent(s.chart().notes)) << "第 " << i << " 步后 ln 不一致";
    }

    // 全部 undo 回初始状态
    while (s.can_undo()) {
        ASSERT_TRUE(s.undo());
        EXPECT_TRUE(ln_consistent(s.chart().notes));
    }
    EXPECT_EQ(norm_notes(s.chart().notes), norm_notes(initial.notes));
    EXPECT_EQ(s.chart().meta, initial.meta);

    // 全部 redo 后再全部 undo（重做路径可逆）
    while (s.can_redo()) {
        ASSERT_TRUE(s.redo());
        EXPECT_TRUE(ln_consistent(s.chart().notes));
    }
    while (s.can_undo()) {
        ASSERT_TRUE(s.undo());
    }
    EXPECT_EQ(norm_notes(s.chart().notes), norm_notes(initial.notes));
}

// —— 协议命令（经 dispatch 走 session） ——

TEST(EditCommands, ProtocolPutUndoViaDispatch) {
    using beatbench::cmd::global_registry;
    using beatbench::json::Json;
    // 先 load 临时谱面到 session
    auto& session = beatbench::edit::global_editor_session();
    session.load(make_chart());
    const auto before = session.chart();

    // note.put
    Json args = Json::object();
    args.set("measure", 2);
    Json pos = Json::object();
    pos.set("num", 0);
    pos.set("den", 1);
    args.set("pos", std::move(pos));
    Json lane = Json::object();
    lane.set("player", 0);
    lane.set("kind", "key");
    lane.set("index", 3);
    args.set("lane", std::move(lane));
    args.set("sample", 9);
    Json req = Json::object();
    req.set("command", "note.put");
    req.set("args", std::move(args));
    const Json resp = global_registry().dispatch(req);
    ASSERT_TRUE(resp.at("ok").as_bool()) << resp.dump();
    EXPECT_EQ(session.chart().notes.size(), before.notes.size() + 1);

    // session.undo
    Json ureq = Json::object();
    ureq.set("command", "session.undo");
    const Json uresp = global_registry().dispatch(ureq);
    ASSERT_TRUE(uresp.at("ok").as_bool()) << uresp.dump();
    EXPECT_EQ(session.chart().notes.size(), before.notes.size());
}

// —— Selection ——

TEST(EditCommands, SelectionBasicOps) {
    Selection sel;
    const NoteRef a{1, Rational(0, 1), {0, LaneKind::Key, 1}, 1};
    const NoteRef b{2, Rational(1, 2), {0, LaneKind::Key, 2}, 2};
    sel.add(a);
    sel.add(b);
    EXPECT_EQ(sel.size(), 2u);
    EXPECT_TRUE(sel.contains(a));
    EXPECT_TRUE(sel.contains(b));
    sel.remove(a);
    EXPECT_FALSE(sel.contains(a));
    EXPECT_EQ(sel.size(), 1u);
    sel.clear();
    EXPECT_TRUE(sel.empty());
}

TEST(EditCommands, SelectionRectFilter) {
    Selection sel;
    std::vector<NoteRef> candidates = {
        {1, Rational(0, 1), {0, LaneKind::Key, 1}, 1},
        {1, Rational(1, 2), {0, LaneKind::Key, 2}, 2},
        {2, Rational(0, 1), {0, LaneKind::Key, 1}, 3},
        {2, Rational(3, 4), {0, LaneKind::Scratch, 0}, 4},
    };
    // measure 1..2，lane 仅 key1，pos 0..1/2
    sel.add_rect(1, 2, {Lane{0, LaneKind::Key, 1}}, Rational(0, 1), Rational(1, 2), candidates);
    EXPECT_EQ(sel.size(), 2u);  // candidates[0] 和 [2]
    EXPECT_TRUE(sel.contains(candidates[0]));
    EXPECT_TRUE(sel.contains(candidates[2]));
    EXPECT_FALSE(sel.contains(candidates[1]));  // key2
    EXPECT_FALSE(sel.contains(candidates[3]));  // scratch
}

TEST(EditCommands, SessionSelectionLifecycle) {
    EditorSession s;
    s.load(make_chart());
    EXPECT_TRUE(s.selection().empty());
    Selection sel;
    sel.add({1, Rational(0, 1), {0, LaneKind::Key, 1}, 1});
    s.set_selection(std::move(sel));
    EXPECT_EQ(s.selection().size(), 1u);
    // load 新谱面清空选择
    s.load(make_chart());
    EXPECT_TRUE(s.selection().empty());
}

// —— CompositeCommand ——

TEST(EditCommands, CompositeBatchDeleteOneUndoStep) {
    EditorSession s;
    s.load(make_chart());  // 2 notes
    // 批量删除两个 note（Composite → 一个 undo 步）
    auto comp = std::make_unique<CompositeCommand>();
    comp->add(std::make_unique<DeleteNoteCommand>(1, Rational(0, 1), Lane{0, LaneKind::Key, 1}, 1));
    comp->add(std::make_unique<DeleteNoteCommand>(1, Rational(1, 2), Lane{0, LaneKind::Key, 2}, 2));
    ASSERT_TRUE(s.exec(std::move(comp)));
    EXPECT_TRUE(s.chart().notes.empty());
    EXPECT_EQ(s.undo_depth(), 1u);
    // 一次 undo 全部恢复
    ASSERT_TRUE(s.undo());
    EXPECT_EQ(norm_notes(s.chart().notes), norm_notes(make_chart().notes));
}

TEST(EditCommands, CompositeInvertReverseOrder) {
    // 复合命令 invert 逆序：先删后放 与 先放后删 的 undo 都精确
    EditorSession s;
    s.load(make_chart());
    auto comp = std::make_unique<CompositeCommand>();
    comp->add(std::make_unique<PutNoteCommand>(2, Rational(0, 1), Lane{0, LaneKind::Key, 3}, 7));
    comp->add(std::make_unique<DeleteNoteCommand>(1, Rational(0, 1), Lane{0, LaneKind::Key, 1}, 1));
    ASSERT_TRUE(s.exec(std::move(comp)));
    ASSERT_EQ(s.chart().notes.size(), 2u);  // 删 1 放 1
    ASSERT_TRUE(s.undo());
    EXPECT_EQ(norm_notes(s.chart().notes), norm_notes(make_chart().notes));
}

// —— 剪贴板（BMS 原始行文本） ——

TEST(EditCommands, ClipboardCopyRoundtrip) {
    // copy 选中 note → 文本行；paste 回 → note 集合一致
    using beatbench::cmd::global_registry;
    auto& session = beatbench::edit::global_editor_session();
    session.load(make_chart());
    const auto before = session.chart();

    // copy：选 notes[0]（m1 pos0 key1 s1）与 notes[1]（m1 pos1/2 key2 s2）
    Json args = Json::object();
    Json sel = Json::array();
    for (const auto& e : before.notes) {
        Json item = Json::object();
        item.set("measure", static_cast<std::int64_t>(e.measure));
        Json pos = Json::object();
        pos.set("num", e.pos.num);
        pos.set("den", e.pos.den);
        item.set("pos", std::move(pos));
        Json lane = Json::object();
        lane.set("player", static_cast<std::int64_t>(e.value.lane.player));
        lane.set("index", static_cast<std::int64_t>(e.value.lane.index));
        lane.set("kind", "key");
        item.set("lane", std::move(lane));
        item.set("sample", static_cast<std::int64_t>(e.value.sample.id));
        sel.push_back(std::move(item));
    }
    args.set("selection", std::move(sel));
    Json req = Json::object();
    req.set("command", "clipboard.copy");
    req.set("args", std::move(args));
    const Json resp = global_registry().dispatch(req);
    ASSERT_TRUE(resp.at("ok").as_bool()) << resp.dump();
    const auto& lines = resp.at("result").at("lines").as_array();
    ASSERT_EQ(lines.size(), 2u);  // key1 一行 + key2 一行
    EXPECT_EQ(lines[0].as_str(), "#00111:01");      // key1 pos0 sample1（n=1 槽位）
    EXPECT_EQ(lines[1].as_str(), "#00112:0002");    // key2 pos1/2 sample2（n=2 槽位）
}

TEST(EditCommands, ClipboardPasteInsertsAtTarget) {
    using beatbench::cmd::global_registry;
    auto& session = beatbench::edit::global_editor_session();
    session.load(make_chart());
    const std::size_t before = session.chart().notes.size();

    // paste 剪贴板文本到 target_measure=5
    Json args = Json::object();
    args.set("text", "#00111:0100\n#00112:0002\n");
    args.set("target_measure", 5);
    Json req = Json::object();
    req.set("command", "clipboard.paste");
    req.set("args", std::move(args));
    const Json resp = global_registry().dispatch(req);
    ASSERT_TRUE(resp.at("ok").as_bool()) << resp.dump();
    EXPECT_EQ(resp.at("result").at("notes").as_i64(), 2);
    EXPECT_EQ(session.chart().notes.size(), before + 2);
    // note 在 measure 5
    std::size_t in_m5 = 0;
    for (const auto& e : session.chart().notes) {
        if (e.measure == 5) ++in_m5;
    }
    EXPECT_EQ(in_m5, 2u);
    // undo 一次全部移除
    ASSERT_TRUE(session.undo());
    EXPECT_EQ(session.chart().notes.size(), before);
}

TEST(EditCommands, ClipboardPasteBadText) {
    using beatbench::cmd::global_registry;
    auto& session = beatbench::edit::global_editor_session();
    session.load(make_chart());
    Json args = Json::object();
    args.set("text", "garbage not bms\n");
    Json req = Json::object();
    req.set("command", "clipboard.paste");
    req.set("args", std::move(args));
    const Json resp = global_registry().dispatch(req);
    EXPECT_FALSE(resp.at("ok").as_bool());
    EXPECT_EQ(resp.at("error").at("code").as_str(), "bad_args");
}
