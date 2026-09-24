// SPDX-License-Identifier: GPL-3.0-only
// 编辑命令测试：note.put/move/delete 的 apply/invert 精确往返、merge 合并、
// undo/redo 栈、随机命令序列可逆性（01 §5.6「命令可逆性」守卫）。
// 全部合成 Chart，不依赖 local/。
#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <filesystem>
#include <fstream>
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
    // 构造 LN 对：两个 key1 note（m1 pos0 与 m1 pos1/2）互指，同 sample
    Chart c = make_chart();
    c.notes[0].value.lane = {0, LaneKind::Key, 1};  // 确保同 lane
    c.notes[1].value.lane = {0, LaneKind::Key, 1};
    c.notes[1].value.sample.id = 1;  // 与 n1 同 sample（rebuild 分组前提）
    c.notes[0].value.ln_channel = true;
    c.notes[1].value.ln_channel = true;
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
    // 2026-09 用户最终确认：移动只移动选中 note，ln_pair 保持（不自动重连也不断开）。
    // 默认（move_ln_pair=false）：只移主 note 到 m3，配对端留原位，互指按伙伴值保持。
    // ⚠️ n1/n2 须同 sample（rebuild 按 (lane,sample) 分组交替配对）。
    EditorSession s;
    Chart c = make_chart();
    c.notes[0].value.lane = {0, LaneKind::Key, 1};
    c.notes[1].value.lane = {0, LaneKind::Key, 1};
    c.notes[1].value.sample.id = 1;  // 与 n1 同 sample（关键）
    c.notes[0].value.ln_channel = true;
    c.notes[1].value.ln_channel = true;
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
    // 配对保持（被移动 note 与留原位的伙伴仍互指）
    for (const auto& e : s.chart().notes) {
        EXPECT_TRUE(e.value.ln_pair.has_value());
    }
    // undo 精确还原（配对保持）
    ASSERT_TRUE(s.undo());
    EXPECT_TRUE(ln_consistent(s.chart().notes));
    EXPECT_TRUE(s.chart().notes[0].value.ln_pair.has_value());
    EXPECT_TRUE(s.chart().notes[1].value.ln_pair.has_value());
}

TEST(EditCommands, MoveLnPairModeMovesBothEnds) {
    // 2026-09 用户最终确认：移动只移动选中 note，ln_pair 保持（不成对随动）。
    // move_ln_pair=true 参数保留（旧 API），但只移主 note、配对互指按伙伴值保持。
    EditorSession s;
    Chart c = make_chart();
    c.notes[0].value.lane = {0, LaneKind::Key, 1};
    c.notes[1].value.lane = {0, LaneKind::Key, 1};
    c.notes[1].value.sample.id = 1;  // 与 n1 同 sample
    c.notes[0].value.ln_channel = true;
    c.notes[1].value.ln_channel = true;
    c.notes[0].value.ln_pair = 1;
    c.notes[1].value.ln_pair = 0;
    s.load(std::move(c));
    ASSERT_TRUE(s.exec(std::make_unique<MoveNoteCommand>(1, Rational(0, 1),
                                                         Lane{0, LaneKind::Key, 1}, 1, 3,
                                                         Rational(0, 1), true)));
    EXPECT_TRUE(ln_consistent(s.chart().notes));
    // 头在 m3（pos0），尾留 m1（pos1/2）；两者仍互指
    bool head_at_m3 = false, tail_at_m1 = false;
    for (const auto& e : s.chart().notes) {
        if (e.measure == 3 && e.pos == Rational(0, 1) &&
            e.value.lane == (Lane{0, LaneKind::Key, 1}))
            head_at_m3 = true;
        if (e.measure == 1 && e.pos == Rational(1, 2) &&
            e.value.lane == (Lane{0, LaneKind::Key, 1}))
            tail_at_m1 = true;
    }
    EXPECT_TRUE(head_at_m3);
    EXPECT_TRUE(tail_at_m1);
    // undo → 完全还原（含配对）
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

TEST(EditCommands, DirtyFlagTracksEditUndoSave) {
    EditorSession s;
    s.load(make_chart());
    EXPECT_FALSE(s.is_dirty());

    ASSERT_TRUE(s.exec(std::make_unique<PutNoteCommand>(
        2, Rational(0, 1), Lane{0, LaneKind::Key, 3}, 7)));
    EXPECT_TRUE(s.is_dirty());

    ASSERT_TRUE(s.undo());
    EXPECT_FALSE(s.is_dirty());  // 撤回到 load 点

    ASSERT_TRUE(s.redo());
    EXPECT_TRUE(s.is_dirty());
    s.mark_clean();
    EXPECT_FALSE(s.is_dirty());

    ASSERT_TRUE(s.undo());
    EXPECT_TRUE(s.is_dirty());  // 从已保存点再 undo 仍脏
    ASSERT_TRUE(s.redo());
    EXPECT_FALSE(s.is_dirty());
}

TEST(EditCommands, DirtyAfterSaveUndoThenNewEdit) {
    // 保存 → 撤销一步 → 再编辑：新内容与保存点不同，必须仍脏（代际不得复用清洁点号）。
    EditorSession s;
    s.load(make_chart());
    ASSERT_TRUE(s.exec(std::make_unique<PutNoteCommand>(
        2, Rational(0, 1), Lane{0, LaneKind::Key, 3}, 7)));
    ASSERT_TRUE(s.exec(std::make_unique<PutNoteCommand>(
        3, Rational(0, 1), Lane{0, LaneKind::Key, 4}, 8)));
    EXPECT_EQ(s.undo_depth(), 2u);
    s.mark_clean();
    EXPECT_FALSE(s.is_dirty());
    ASSERT_TRUE(s.undo());
    EXPECT_TRUE(s.is_dirty());
    ASSERT_TRUE(s.exec(std::make_unique<PutNoteCommand>(
        4, Rational(0, 1), Lane{0, LaneKind::Key, 5}, 9)));
    EXPECT_TRUE(s.is_dirty());
    EXPECT_EQ(s.undo_depth(), 2u);  // 新编辑独立一步，不并入保存点前的命令
}

TEST(EditCommands, ProtocolDirtyQueryAndSaveClears) {
    using beatbench::cmd::global_registry;
    auto& session = beatbench::edit::global_editor_session();
    session.load(make_chart(), "");
    Json dreq = Json::object();
    dreq.set("command", "session.dirty");
    dreq.set("args", Json::object());
    Json dresp = global_registry().dispatch(dreq);
    ASSERT_TRUE(dresp.at("ok").as_bool()) << dresp.dump();
    EXPECT_FALSE(dresp.at("result").at("dirty").as_bool());

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
    Json preq = Json::object();
    preq.set("command", "note.put");
    preq.set("args", std::move(args));
    ASSERT_TRUE(global_registry().dispatch(preq).at("ok").as_bool());
    dresp = global_registry().dispatch(dreq);
    ASSERT_TRUE(dresp.at("ok").as_bool());
    EXPECT_TRUE(dresp.at("result").at("dirty").as_bool());

    namespace fs = std::filesystem;
    const auto dir = fs::temp_directory_path() / "bb_dirty_save";
    fs::create_directories(dir);
    const auto path = (dir / "dirty.bms").string();
    Json sargs = Json::object();
    sargs.set("path", path);
    sargs.set("overwrite", true);
    Json sreq = Json::object();
    sreq.set("command", "session.save");
    sreq.set("args", std::move(sargs));
    Json sresp = global_registry().dispatch(sreq);
    ASSERT_TRUE(sresp.at("ok").as_bool()) << sresp.dump();
    EXPECT_FALSE(sresp.at("result").at("dirty").as_bool());
    dresp = global_registry().dispatch(dreq);
    EXPECT_FALSE(dresp.at("result").at("dirty").as_bool());
    fs::remove_all(dir);
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
    const auto& placed = resp.at("result").at("selection").as_array();
    ASSERT_EQ(placed.size(), 2u);
    EXPECT_EQ(placed[0].at("measure").as_i64(), 5);
    EXPECT_EQ(placed[1].at("measure").as_i64(), 5);
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

TEST(EditCommands, ClipboardPasteWavDefsAndMeasures) {
    // 2026-09：切音导出 raw 整段粘贴——#WAV 定义 + #NNN02: 小节长 + ch01 数据行（一个 undo 步）
    using beatbench::cmd::global_registry;
    auto& session = beatbench::edit::global_editor_session();
    session.load(make_chart());
    const std::size_t before = session.chart().notes.size();

    Json args = Json::object();
    args.set("text",
             "#WAV01 kick.wav\n"
             "#WAVAA slices/slice_170.wav\n"
             "#00002:2\n"
             "#00201:0100AA\n");
    args.set("target_measure", 5);
    Json req = Json::object();
    req.set("command", "clipboard.paste");
    req.set("args", std::move(args));
    const Json resp = global_registry().dispatch(req);
    ASSERT_TRUE(resp.at("ok").as_bool()) << resp.dump();
    EXPECT_EQ(resp.at("result").at("notes").as_i64(), 2);
    EXPECT_EQ(resp.at("result").at("wavs").as_i64(), 2);
    EXPECT_EQ(resp.at("result").at("measures").as_i64(), 1);

    const auto& chart = session.chart();
    // #WAV 定义创建（01 → id1；AA → id370）
    auto it = chart.samples.find({SampleKind::Wav, 1});
    ASSERT_NE(it, chart.samples.end());
    EXPECT_EQ(it->second.file, "kick.wav");
    it = chart.samples.find({SampleKind::Wav, 370});
    ASSERT_NE(it, chart.samples.end());
    EXPECT_EQ(it->second.file, "slices/slice_170.wav");
    // 小节长：ch02 值 2 = 2×4 四分拍；原 measure 0 + 偏移 5 → m5
    bool found_mea = false;
    for (const auto& e : chart.measure_events) {
        if (e.measure == 5 && e.pos == Rational(0, 1)) {
            EXPECT_DOUBLE_EQ(e.value.beats, 8.0);
            found_mea = true;
        }
    }
    EXPECT_TRUE(found_mea);
    // note：m2（偏移后 m7）两个 ch01 note（sample 1 / AA）
    std::size_t in_m7 = 0;
    for (const auto& e : chart.notes)
        if (e.measure == 7 && e.value.lane.kind == LaneKind::Bgm) ++in_m7;
    EXPECT_EQ(in_m7, 2u);
    // 撤销一次 = 全部移除（note + #WAV 定义 + 小节长）
    ASSERT_TRUE(session.undo());
    EXPECT_EQ(session.chart().notes.size(), before);
    EXPECT_EQ(session.chart().samples.count({SampleKind::Wav, 1}), 0u);
    EXPECT_EQ(session.chart().samples.count({SampleKind::Wav, 370}), 0u);
    EXPECT_TRUE(session.chart().measure_events.empty());
}

TEST(EditCommands, ClipboardPasteBgmSubLineFifo) {
    // 2026-09：同小节多行 ch01 = 子行（sub_line FIFO 与原解析器一致），粘贴不挤压成一行
    using beatbench::cmd::global_registry;
    auto& session = beatbench::edit::global_editor_session();
    session.load(make_chart());
    Json args = Json::object();
    args.set("text", "#00001:0100\n#00001:0200\n");
    Json req = Json::object();
    req.set("command", "clipboard.paste");
    req.set("args", std::move(args));
    const Json resp = global_registry().dispatch(req);
    ASSERT_TRUE(resp.at("ok").as_bool()) << resp.dump();
    EXPECT_EQ(resp.at("result").at("notes").as_i64(), 2);

    std::size_t s0 = 0, s1 = 0;
    for (const auto& e : session.chart().notes) {
        if (e.measure == 0 && e.value.lane.kind == LaneKind::Bgm && e.pos == Rational(0, 1)) {
            if (e.value.sub_line == 0) ++s0;
            if (e.value.sub_line == 1) ++s1;
        }
    }
    EXPECT_EQ(s0, 1u);
    EXPECT_EQ(s1, 1u);
}

TEST(EditCommands, ClipboardPasteSubLineContinueUniform) {
    // M6.3 铺放（2026-09 用户「同时铺入编辑区」）：sub_line_mode="uniform" = 子行接续——
    // 目标小节段已有 BGM 最高子行 +1 起统一抬升：新行不挤旧行（旧行结构不动）、同 tick 共存；
    // 空段行为与 fifo 相同（从 0 起）。单命令单撤销步。
    using beatbench::cmd::global_registry;
    auto& session = beatbench::edit::global_editor_session();
    session.load(make_chart());

    // 第一次粘贴（默认 fifo）：m0 两行（sub_line 0/1，同 tick pos0）
    Json a1 = Json::object();
    a1.set("text", "#00001:0100\n#00001:0200\n");
    Json r1 = Json::object();
    r1.set("command", "clipboard.paste");
    r1.set("args", std::move(a1));
    const Json resp1 = global_registry().dispatch(r1);
    ASSERT_TRUE(resp1.at("ok").as_bool()) << resp1.dump();
    EXPECT_EQ(resp1.at("result").at("notes").as_i64(), 2);

    // 第二次粘贴（uniform）：同 m0 两行 → 子行接续 2/3（与旧行 0/1 同 tick 共存）
    Json a2 = Json::object();
    a2.set("text", "#00001:0A00\n#00001:0B00\n");
    a2.set("sub_line_mode", "uniform");
    Json r2 = Json::object();
    r2.set("command", "clipboard.paste");
    r2.set("args", std::move(a2));
    const Json resp2 = global_registry().dispatch(r2);
    ASSERT_TRUE(resp2.at("ok").as_bool()) << resp2.dump();
    EXPECT_EQ(resp2.at("result").at("notes").as_i64(), 2);

    std::array<int, 4> perSubLine{};
    for (const auto& e : session.chart().notes) {
        if (e.measure == 0 && e.value.lane.kind == LaneKind::Bgm &&
            e.pos == Rational(0, 1) && e.value.sub_line < 4)
            ++perSubLine[e.value.sub_line];
    }
    EXPECT_EQ(perSubLine[0], 1) << "旧行 0 应保留";
    EXPECT_EQ(perSubLine[1], 1) << "旧行 1 应保留";
    EXPECT_EQ(perSubLine[2], 1) << "新行 2 = 接续起点";
    EXPECT_EQ(perSubLine[3], 1) << "新行 3 = 接续第二行";

    // 撤销 = 第二次粘贴全部回滚（单 undo 步：note + #WAV 定义一起）
    ASSERT_TRUE(session.undo());
    std::size_t total = 0;
    for (const auto& e : session.chart().notes)
        if (e.measure == 0 && e.value.lane.kind == LaneKind::Bgm) ++total;
    EXPECT_EQ(total, 2u);
}

TEST(EditCommands, ClipboardPasteSubLineContinueUniformEmptyAndOverflow) {
    // 空段：uniform 与 fifo 相同（从 0 起）；超 12 行上限 → 报错不写入。
    using beatbench::cmd::global_registry;
    auto& session = beatbench::edit::global_editor_session();
    session.load(make_chart());

    Json a = Json::object();
    a.set("text", "#00001:0100\n#00001:0200\n");
    a.set("sub_line_mode", "uniform");
    Json req = Json::object();
    req.set("command", "clipboard.paste");
    req.set("args", std::move(a));
    const Json resp = global_registry().dispatch(req);
    ASSERT_TRUE(resp.at("ok").as_bool()) << resp.dump();
    std::array<int, 2> perSubLine{};
    for (const auto& e : session.chart().notes)
        if (e.measure == 0 && e.value.lane.kind == LaneKind::Bgm &&
            e.value.sub_line < 2)
            ++perSubLine[e.value.sub_line];
    EXPECT_EQ(perSubLine[0], 1);
    EXPECT_EQ(perSubLine[1], 1);

    // 上限 = beatoraja 默认 64 采样序列（行号 0..63）。本场景已有 2 行（sub 0/1）：
    // 再贴 61 行（sub 2-62）= 63 行 → 第 64 行（base=63）通过；>64 → 超限报错不写入
    // （2026-09 修正：原 12 为 iBMSC 显示惯例，不是播放器/格式限制）。
    const std::size_t before = session.chart().notes.size();
    Json a2 = Json::object();
    std::string big;
    for (int i = 0; i < 61; ++i) big += "#00001:" + std::to_string(16 + i) + "0000\n";
    a2.set("text", big);
    a2.set("sub_line_mode", "uniform");
    Json req2 = Json::object();
    req2.set("command", "clipboard.paste");
    req2.set("args", std::move(a2));
    const Json resp2 = global_registry().dispatch(req2);
    ASSERT_TRUE(resp2.at("ok").as_bool()) << resp2.dump();

    // 63 行（sub 0-62）已有 + 贴 1 行 → base=63，正好 64 行（sub_line 63）→ 通过
    Json a3 = Json::object();
    a3.set("text", "#00001:800000\n");
    a3.set("sub_line_mode", "uniform");
    Json req3 = Json::object();
    req3.set("command", "clipboard.paste");
    req3.set("args", std::move(a3));
    const Json resp3 = global_registry().dispatch(req3);
    ASSERT_TRUE(resp3.at("ok").as_bool()) << resp3.dump();
    bool saw63 = false;
    for (const auto& e : session.chart().notes)
        if (e.measure == 0 && e.value.lane.kind == LaneKind::Bgm &&
            e.value.sub_line == 63)
            saw63 = true;
    EXPECT_TRUE(saw63) << "第 64 行（sub_line 63）应存在";

    // 上限已取消（2026-09 用户：剪贴板限制 → lint 提示）：65 行（sub 0-64）也放行
    const std::size_t before2 = session.chart().notes.size();
    Json a4 = Json::object();
    a4.set("text", "#00001:810000\n#00001:820000\n");
    a4.set("sub_line_mode", "uniform");
    Json req4 = Json::object();
    req4.set("command", "clipboard.paste");
    req4.set("args", std::move(a4));
    const Json resp4 = global_registry().dispatch(req4);
    ASSERT_TRUE(resp4.at("ok").as_bool()) << resp4.dump();
    bool saw64 = false, saw65 = false;
    for (const auto& e : session.chart().notes)
        if (e.measure == 0 && e.value.lane.kind == LaneKind::Bgm) {
            if (e.value.sub_line == 64) saw64 = true;
            if (e.value.sub_line == 65) saw65 = true;
        }
    EXPECT_TRUE(saw64) << "超出 64 上限仍应写入（lint 提示，不阻止）";
    EXPECT_TRUE(saw65) << "超出 64 上限仍应写入（lint 提示，不阻止）";
    EXPECT_GT(session.chart().notes.size(), before2);
}

namespace {

Json note_sel_item(const Event<Note>& e) {
    Json item = Json::object();
    item.set("measure", static_cast<std::int64_t>(e.measure));
    Json pos = Json::object();
    pos.set("num", e.pos.num);
    pos.set("den", e.pos.den);
    item.set("pos", std::move(pos));
    Json lane = Json::object();
    lane.set("player", static_cast<std::int64_t>(e.value.lane.player));
    lane.set("index", static_cast<std::int64_t>(e.value.lane.index));
    const char* kind = "key";
    if (e.value.lane.kind == LaneKind::Bgm) kind = "bgm";
    else if (e.value.lane.kind == LaneKind::Scratch) kind = "scratch";
    else if (e.value.lane.kind == LaneKind::Pedal) kind = "pedal";
    lane.set("kind", kind);
    item.set("lane", std::move(lane));
    item.set("sample", static_cast<std::int64_t>(e.value.sample.id));
    item.set("sub_line", static_cast<std::int64_t>(e.value.sub_line));
    return item;
}

}  // namespace

TEST(EditCommands, ClipboardCopyMineAndBgmPreserveChannels) {
    auto& session = global_editor_session();
    Chart c;
    c.meta["BPM"] = "130";
    Event<Note> mine{1, Rational(0, 1), {}};
    mine.value.lane = {0, LaneKind::Key, 1};
    mine.value.sample.id = 3;
    mine.value.kind = NoteKind::Landmine;
    Event<Note> bgm0{1, Rational(0, 1), {}};
    bgm0.value.lane = {0, LaneKind::Bgm, 0};
    bgm0.value.sample.id = 4;
    bgm0.value.sub_line = 0;
    Event<Note> bgm1{1, Rational(0, 1), {}};
    bgm1.value.lane = {0, LaneKind::Bgm, 0};
    bgm1.value.sample.id = 5;
    bgm1.value.sub_line = 1;
    c.notes = {mine, bgm0, bgm1};
    session.load(std::move(c));

    Json args = Json::object();
    Json sel = Json::array();
    for (const auto& e : session.chart().notes) sel.push_back(note_sel_item(e));
    args.set("selection", std::move(sel));
    Json req = Json::object();
    req.set("command", "clipboard.copy");
    req.set("args", std::move(args));
    const Json resp = global_registry().dispatch(req);
    ASSERT_TRUE(resp.at("ok").as_bool()) << resp.dump();
    const auto& lines = resp.at("result").at("lines").as_array();
    ASSERT_EQ(lines.size(), 3u);
    bool saw_mine = false, saw_bgm_a = false, saw_bgm_b = false;
    for (const auto& line : lines) {
        const auto& s = line.as_str();
        if (s.find("#001D1:") == 0) saw_mine = true;
        if (s == "#00101:04") saw_bgm_a = true;
        if (s == "#00101:05") saw_bgm_b = true;
    }
    EXPECT_TRUE(saw_mine);
    EXPECT_TRUE(saw_bgm_a);
    EXPECT_TRUE(saw_bgm_b);

    const std::size_t before = session.chart().notes.size();
    Json paste = Json::object();
    paste.set("lines", resp.at("result").at("lines"));
    paste.set("target_measure", 8);
    Json preq = Json::object();
    preq.set("command", "clipboard.paste");
    preq.set("args", std::move(paste));
    const Json presp = global_registry().dispatch(preq);
    ASSERT_TRUE(presp.at("ok").as_bool()) << presp.dump();
    EXPECT_EQ(presp.at("result").at("notes").as_i64(), 3);
    std::size_t mines = 0, bgms = 0;
    for (const auto& e : session.chart().notes) {
        if (e.measure != 8) continue;
        if (e.value.kind == NoteKind::Landmine) ++mines;
        if (e.value.lane.kind == LaneKind::Bgm) ++bgms;
    }
    EXPECT_EQ(mines, 1u);
    EXPECT_EQ(bgms, 2u);
    const auto& placed = presp.at("result").at("selection").as_array();
    ASSERT_EQ(placed.size(), 3u);
    EXPECT_EQ(placed[0].at("measure").as_i64(), 8);
    ASSERT_TRUE(session.undo());
    EXPECT_EQ(session.chart().notes.size(), before);
}

TEST(EditCommands, ClipboardCopyRejectsHalfSelectedLn) {
    auto& session = global_editor_session();
    Chart c;
    c.meta["BPM"] = "130";
    c.meta["LNTYPE"] = "1";
    session.load(std::move(c));
    ASSERT_TRUE(session.exec(std::make_unique<PutNoteCommand>(
        1, Rational(0, 1), Lane{0, LaneKind::Key, 1}, 1, true)));
    ASSERT_TRUE(session.exec(std::make_unique<PutNoteCommand>(
        1, Rational(1, 2), Lane{0, LaneKind::Key, 1}, 1, true)));
    ASSERT_EQ(session.chart().notes.size(), 2u);
    const auto before = session.chart();

    Json args = Json::object();
    Json sel = Json::array();
    sel.push_back(note_sel_item(session.chart().notes[0]));  // 只选头
    args.set("selection", std::move(sel));
    Json req = Json::object();
    req.set("command", "clipboard.copy");
    req.set("args", std::move(args));
    const Json resp = global_registry().dispatch(req);
    EXPECT_FALSE(resp.at("ok").as_bool());
    EXPECT_EQ(resp.at("error").at("code").as_str(), "incomplete_ln");
    EXPECT_EQ(session.chart().notes.size(), before.notes.size());
}

TEST(EditCommands, ClipboardCopyPasteLnPairRoundtrip) {
    auto& session = global_editor_session();
    Chart c;
    c.meta["BPM"] = "130";
    c.meta["LNTYPE"] = "1";
    session.load(std::move(c));
    ASSERT_TRUE(session.exec(std::make_unique<PutNoteCommand>(
        2, Rational(0, 1), Lane{0, LaneKind::Key, 3}, 7, true)));
    ASSERT_TRUE(session.exec(std::make_unique<PutNoteCommand>(
        2, Rational(1, 2), Lane{0, LaneKind::Key, 3}, 7, true)));
    Json args = Json::object();
    Json sel = Json::array();
    for (const auto& e : session.chart().notes) sel.push_back(note_sel_item(e));
    args.set("selection", std::move(sel));
    Json req = Json::object();
    req.set("command", "clipboard.copy");
    req.set("args", std::move(args));
    const Json resp = global_registry().dispatch(req);
    ASSERT_TRUE(resp.at("ok").as_bool()) << resp.dump();
    const auto& lines = resp.at("result").at("lines").as_array();
    ASSERT_EQ(lines.size(), 1u);
    EXPECT_NE(lines[0].as_str().find("#00253:"), std::string::npos);

    const std::size_t before = session.chart().notes.size();
    Json paste = Json::object();
    paste.set("lines", resp.at("result").at("lines"));
    paste.set("target_measure", 6);
    Json preq = Json::object();
    preq.set("command", "clipboard.paste");
    preq.set("args", std::move(paste));
    const Json presp = global_registry().dispatch(preq);
    ASSERT_TRUE(presp.at("ok").as_bool()) << presp.dump();
    std::size_t ln_notes = 0;
    for (const auto& e : session.chart().notes) {
        if (e.measure == 6 && e.value.ln_channel) ++ln_notes;
    }
    EXPECT_EQ(ln_notes, 2u);
    ASSERT_TRUE(session.undo());
    EXPECT_EQ(session.chart().notes.size(), before);
}

TEST(EditCommands, NoteDeleteSelectionIsSingleUndo) {
    auto& session = global_editor_session();
    session.load(make_chart());
    const std::size_t before = session.chart().notes.size();
    const std::size_t undo_before = session.undo_depth();
    Json args = Json::object();
    Json sel = Json::array();
    for (const auto& e : session.chart().notes) sel.push_back(note_sel_item(e));
    args.set("selection", std::move(sel));
    Json req = Json::object();
    req.set("command", "note.delete");
    req.set("args", std::move(args));
    const Json resp = global_registry().dispatch(req);
    ASSERT_TRUE(resp.at("ok").as_bool()) << resp.dump();
    EXPECT_EQ(resp.at("result").at("deleted").as_i64(), static_cast<std::int64_t>(before));
    EXPECT_EQ(session.chart().notes.size(), 0u);
    EXPECT_EQ(session.undo_depth(), undo_before + 1);
    ASSERT_TRUE(session.undo());
    EXPECT_EQ(session.chart().notes.size(), before);
}
