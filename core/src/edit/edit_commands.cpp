// SPDX-License-Identifier: GPL-3.0-only
// 编辑命令实现：note.put / note.move / note.delete + EditorSession（undo/redo 栈）。
// 设计（doc/core-model.md 二、）：命令 = 文档变更唯一操作面，apply/invert 精确互逆，
// merge_with 合并连续操作；Selection/Viewport 不入栈（调用方自持）。
//
// LN 配对一致性策略（下标易错，改为值级重定位）：
//   - ln_pair 存容器下标；任何插入/删除/移动后，受影响端按
//     「伙伴的 (measure,pos,lane,sample) 值」重新定位下标。
//   - 配对关系本身（谁和谁是一对）由命令快照携带：apply 记录被操作 note
//     的完整状态（含 ln_pair 指向的伙伴快照），invert 用快照精确恢复。
#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <iomanip>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <vector>

#include "beatbench/core/ChartMode.hpp"
#include "beatbench/core/bms/BmsUtil.hpp"  // c36/c62 id 转换（LNOBJ 文本 → 数值 id）
#include "beatbench/core/codec/BmsChannelMaps.hpp"
#include "beatbench/core/edit/EditorSession.hpp"
#include "beatbench/core/edit/Selection.hpp"

namespace beatbench::edit {

// ---------- TimingKind ----------

std::string_view timing_kind_name(TimingKind kind) {
    switch (kind) {
        case TimingKind::Bpm:
            return "bpm";
        case TimingKind::Stop:
            return "stop";
        case TimingKind::Measure:
            return "measure";
    }
    return "";
}

std::optional<TimingKind> timing_kind_from_name(std::string_view name) {
    if (name == "bpm") return TimingKind::Bpm;
    if (name == "stop") return TimingKind::Stop;
    if (name == "measure") return TimingKind::Measure;
    return std::nullopt;
}

// ---------- 工具 ----------

namespace {

// 在 (measure, pos) 升序容器中找首个 >= (measure,pos) 的位置（插入点）
std::size_t lower_bound_pos(const std::vector<Event<Note>>& notes, std::uint32_t measure,
                            const Rational& pos) {
    const Event<Note> probe{measure, pos, {}};
    return static_cast<std::size_t>(
        std::lower_bound(notes.begin(), notes.end(), probe,
                         [](const Event<Note>& a, const Event<Note>& b) {
                             if (a.measure != b.measure) return a.measure < b.measure;
                             return a.pos < b.pos;
                         }) -
        notes.begin());
}

// 找 (measure, pos, lane, sample) 匹配的 note 下标（首个命中）
// ⚠️ 不做 (measure,pos) 二分：parser 按通道行序 push，notes 在同一 measure 内不保证
// pos 升序（doc/development.md 六、 的「事件按 (measure,pos) 升序」约定未落实；2026-09 实测 Del 失效：
// lower_bound 定位错位导致 1/4 之类 note 永远匹配不上）。改为 measure 段线性扫描
// （行序 measure 单调递增 → 同 measure 连续段；命中率与代价可控）。
// sub_line：BGM 行序号消歧（同 (measure,pos,lane,sample) 的 Bgm note 靠它区分；
// 非 Bgm 传 0 且不参与匹配）。
std::optional<std::size_t> find_note(const std::vector<Event<Note>>& notes, std::uint32_t measure,
                                     const Rational& pos, const Lane& lane, std::uint32_t sample,
                                     std::uint32_t sub_line = 0) {
    auto it = std::lower_bound(notes.begin(), notes.end(), measure,
                               [](const Event<Note>& e, std::uint32_t m) { return e.measure < m; });
    for (; it != notes.end() && it->measure == measure; ++it) {
        const auto& n = it->value;
        if (it->pos == pos && n.lane == lane && n.sample.id == sample &&
            (lane.kind != LaneKind::Bgm || n.sub_line == sub_line))
            return static_cast<std::size_t>(it - notes.begin());
    }
    return std::nullopt;
}

// 按值定位伙伴下标（互为配对 + 同 (measure,pos,lane,sample)）；找不到 → nullopt
std::optional<std::size_t> find_partner(const std::vector<Event<Note>>& notes,
                                        const Event<Note>& me, std::size_t my_index) {
    const auto p = me.value.ln_pair;
    if (!p) return std::nullopt;
    // 伙伴的旧值快照不可靠（下标可能变）；按「互指 + 值」扫描
    for (std::size_t i = 0; i < notes.size(); ++i) {
        if (i == my_index) continue;
        const auto& v = notes[i].value;
        if (v.ln_pair && *v.ln_pair == my_index) return i;
    }
    return std::nullopt;
}

// ---------- LN 推导（自下而上：数据 → 配对；编辑命令不维护 ln_pair） ----------

// 重建全谱 ln_pair（2026-09 用户原则：ln_pair 是「文件数据 → 解析 → 效果呈现」的派生，
// 不是编辑真相源）。规则与 parser 的 LNTYPE 1 一致：
//   - 只处理 ln_channel==true 的 note（来自 51-69/61-69 LN 通道）；
//   - 按 (lane, sample) 分组（同侧同键同采样），组内按 (measure,pos) 时间序
//     严格交替：第 1 个=头、第 2 个=尾、第 3 个=新头、第 4 个=新尾…；
//   - 未配对（单数/不成对/与其它通道同值冲突）→ ln_pair 留空（lint 提示）。
// 调用时机：EditorSession::exec / undo / redo 之后（每次文档变更后统一推导）。
// NOLINTNEXTLINE(readability-function-cognitive-complexity)——LN 配对区间重连状态链
void rebuild_ln_pairs(Chart& chart) {
    for (auto& n : chart.notes) n.value.ln_pair.reset();

    // LNTYPE 1（含未声明，默认）：只处理 ln_channel==true（来自 51-69/61-69 LN 通道），
    // 按 (lane, sample) 分组，组内按 (measure,pos) 时间序严格交替头尾。
    // ⚠️ 无论 LNTYPE 1/2 都运行：LNTYPE 2 下 51-69 通道物件仍按 LNTYPE 1 规则交替配对
    //（用户确认两机制不冲突、可同时运行——轨道数据可同时含 51-69 LN 通道件与
    // 普通通道 #LNOBJ 件）。若不想双机制，此组可回归「仅 LNTYPE 1」时运行。
    {
        std::map<std::pair<Lane, std::uint32_t>, std::vector<std::size_t>> groups;
        for (std::size_t i = 0; i < chart.notes.size(); ++i) {
            const auto& n = chart.notes[i].value;
            if (!n.ln_channel) continue;
            groups[{n.lane, n.sample.id}].push_back(i);
        }
        for (auto& [key, idxs] : groups) {
            (void)key;
            std::sort(idxs.begin(), idxs.end(), [&](std::size_t a, std::size_t b) {
                const auto& ea = chart.notes[a];
                const auto& eb = chart.notes[b];
                if (ea.measure != eb.measure) return ea.measure < eb.measure;
                return ea.pos < eb.pos;
            });
            for (std::size_t k = 0; k + 1 < idxs.size(); k += 2) {
                const auto a = idxs[k], b = idxs[k + 1];
                chart.notes[a].value.ln_pair = static_cast<std::uint32_t>(b);
                chart.notes[b].value.ln_pair = static_cast<std::uint32_t>(a);
            }
        }
    }

    // LNTYPE 2（#LNOBJ）：头尾同在普通通道（11-29）。值 == #LNOBJ 的物件 = 尾，
    // 头 = 同 lane 最近未配对的先前物件（head_stacks 式）。与 parser（bms_parser.cpp
    // LN 配对块）语义保持一致——编辑后重新推导应复现「重新解析」得到的配对。
    // ⚠️ 缺 #LNTYPE 2 时走上方 LNTYPE 1；此分支只在 chart.meta["LNTYPE"]=="2" 时进入。
    std::uint32_t lnojb_id = 0;
    if (const auto it = chart.meta.find("LNTYPE"); it != chart.meta.end() && it->second == "2") {
        if (const auto lj = chart.meta.find("LNOBJ");
            lj != chart.meta.end() && !lj->second.empty()) {
            lnojb_id = chart.id_base == IdBase::Base62 ? bms::c62_to_u32(lj->second, 2)
                                                       : bms::c36_to_u32(lj->second, 2);
        } else {
            lnojb_id = chart.id_base == IdBase::Base62 ? 3843u : 1295u;  // 无 #LNOBJ → 默认 ZZ
        }

        // 按 (measure,pos) 升序处理（头 = 同 lane 最近先前未配对物件）
        std::vector<std::size_t> order(chart.notes.size());
        for (std::size_t i = 0; i < order.size(); ++i) order[i] = i;
        std::sort(order.begin(), order.end(), [&](std::size_t a, std::size_t b) {
            const auto& ea = chart.notes[a];
            const auto& eb = chart.notes[b];
            if (ea.measure != eb.measure) return ea.measure < eb.measure;
            return ea.pos < eb.pos;
        });
        std::map<Lane, std::vector<std::size_t>> head_stack;
        for (const std::size_t i : order) {
            const auto& n = chart.notes[i].value;
            if (n.ln_channel) continue;  // 51-69 已由上方 LNTYPE 1 组配对；此分支不再处理
            if (n.sample.id == lnojb_id) {
                auto& stk = head_stack[n.lane];
                if (stk.empty()) continue;  // 未配对尾（缺少头；lint 提示）
                const auto h = stk.back();
                stk.pop_back();
                chart.notes[h].value.ln_pair = static_cast<std::uint32_t>(i);
                chart.notes[i].value.ln_pair = static_cast<std::uint32_t>(h);
            } else {
                head_stack[n.lane].push_back(i);
            }
        }
    }
}

// 互指一致性检查（测试辅助）
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

// 给容器中所有 ln_pair 加/减偏移（插入 at 后 +1，删除 at 后 -1），跳过 skip 下标
void shift_pairs_after(std::vector<Event<Note>>& notes, std::size_t at, int delta,
                       std::optional<std::size_t> skip = std::nullopt) {
    for (std::size_t i = 0; i < notes.size(); ++i) {
        if (skip && *skip == i) continue;
        auto& p = notes[i].value.ln_pair;
        if (!p) continue;
        if (delta > 0 && *p >= at) ++*p;
        if (delta < 0 && *p > at) --*p;
    }
}

// —— timing 事件通用工具（BPM/STOP/节拍；按 (measure,pos) 升序，同 pos 保留次序） ——

// 值读取（按种类取 value/count/beats）
double timing_value(const Event<Bpm>& e) { return e.value.value; }
double timing_value(const Event<Stop>& e) { return static_cast<double>(e.value.count); }
double timing_value(const Event<MeasureLen>& e) { return e.value.beats; }

// 在按 (measure,pos) 升序的容器中找首个 >= (measure,pos) 的位置（插入点）
template <typename T>
std::size_t lower_bound_event(const std::vector<Event<T>>& evs, std::uint32_t measure,
                              const Rational& pos) {
    const Event<T> probe{measure, pos, {}};
    return static_cast<std::size_t>(std::lower_bound(evs.begin(), evs.end(), probe,
                                                     [](const Event<T>& a, const Event<T>& b) {
                                                         if (a.measure != b.measure)
                                                             return a.measure < b.measure;
                                                         return a.pos < b.pos;
                                                     }) -
                                    evs.begin());
}

// 找 (measure, pos) 处事件区间 [begin, end)（容器有序）
template <typename T>
std::pair<std::size_t, std::size_t> find_event_range(const std::vector<Event<T>>& evs,
                                                     std::uint32_t measure, const Rational& pos) {
    const std::size_t lo = lower_bound_event(evs, measure, pos);
    std::size_t hi = lo;
    while (hi < evs.size() && evs[hi].measure == measure && evs[hi].pos == pos) ++hi;
    return {lo, hi};
}

// 快照值写入事件（按种类；Measure 的 pos 恒归 0）
template <typename T>
void set_timing_value(Event<T>& ev, double v) {
    if constexpr (std::is_same_v<T, Bpm>) {
        ev.value.value = v;
    } else if constexpr (std::is_same_v<T, Stop>) {
        ev.value.count = static_cast<std::int64_t>(std::llround(v));
    } else {
        ev.value.beats = v;
    }
}

}  // namespace

// ---------- EditorSession ----------

EditorSession::EditorSession() = default;

void EditorSession::load(Chart chart) {
    m_chart = std::make_unique<Chart>(std::move(chart));
    m_undo.clear();
    m_redo.clear();
    m_selection.clear();
    m_path.clear();
    m_vertical_move_mode = beatbench::edit::VerticalMoveMode::
        Free;  // 新文档 core 默认自由（CLI 兼容）；GUI 在 openChart/new 后置 LaneOnly
    m_generation = 0;
    m_next_generation = 1;
    m_clean_generation = 0;
    m_undo_generation.clear();
    m_redo_generation.clear();
}

void EditorSession::load(Chart chart, std::string path) {
    load(std::move(chart));
    m_path = std::move(path);
}

bool EditorSession::exec(std::unique_ptr<EditCommand> cmd) {
    m_last_error = EditError::None;
    m_last_error_message.clear();
    if (!m_chart || !cmd) return false;
    // 三态方向锁：仅横向拦改时间，仅纵向拦截换轨；自由不拦。
    if (m_vertical_move_mode == VerticalMoveMode::LaneOnly && cmd->changes_time()) {
        m_last_error = EditError::TimeLocked;
        m_last_error_message = "仅横向：时间不可改（Ctrl+L 切换三态）";
        return false;
    }
    if (m_vertical_move_mode == VerticalMoveMode::TimeOnly && cmd->changes_lane()) {
        m_last_error = EditError::LaneLocked;
        m_last_error_message = "仅纵向：轨道不可改（Ctrl+L 切换三态）";
        return false;
    }
    Chart before = *m_chart;  // 失败回滚快照（防御性；命令应极少失败）
    try {
        cmd->apply(*m_chart);
    } catch (...) {
        *m_chart = std::move(before);
        return false;
    }
    // LN 推导（自下而上）：文档变更后统一重建 ln_pair（编辑命令不再各自维护）。
    rebuild_ln_pairs(*m_chart);
    // 连续操作可并入栈顶，但：刚保存的状态、以及 undo/redo 之后的新编辑都不能合并，
    // 否则无法撤回到保存点，且代际号会与清洁点撞车（保存→撤销→再改被误判干净）。
    const bool can_merge = m_redo.empty() && !m_undo.empty() &&
                           m_undo_generation.back() == m_generation &&
                           m_generation != m_clean_generation && m_undo.back()->merge_with(*cmd);
    m_generation = m_next_generation++;
    if (can_merge) {
        m_undo_generation.back() = m_generation;
        maybe_persist();
        return true;
    }
    m_undo.push_back(std::move(cmd));
    m_undo_generation.push_back(m_generation);
    m_redo.clear();
    m_redo_generation.clear();
    maybe_persist();
    return true;
}

bool EditorSession::undo() {
    m_last_error = EditError::None;
    m_last_error_message.clear();
    if (m_undo.empty() || !m_chart || m_undo.size() != m_undo_generation.size()) return false;
    auto cmd = std::move(m_undo.back());
    const std::uint64_t gen_after = m_undo_generation.back();
    m_undo.pop_back();
    m_undo_generation.pop_back();
    cmd->invert(*m_chart);
    rebuild_ln_pairs(*m_chart);
    m_redo.push_back(std::move(cmd));
    m_redo_generation.push_back(gen_after);
    m_generation = m_undo_generation.empty() ? 0 : m_undo_generation.back();
    maybe_persist();
    return true;
}

bool EditorSession::redo() {
    m_last_error = EditError::None;
    m_last_error_message.clear();
    if (m_redo.empty() || !m_chart || m_redo.size() != m_redo_generation.size()) return false;
    auto cmd = std::move(m_redo.back());
    const std::uint64_t gen_after = m_redo_generation.back();
    m_redo.pop_back();
    m_redo_generation.pop_back();
    cmd->apply(*m_chart);
    rebuild_ln_pairs(*m_chart);
    m_undo.push_back(std::move(cmd));
    m_undo_generation.push_back(gen_after);
    m_generation = gen_after;
    maybe_persist();
    return true;
}

void EditorSession::maybe_persist() {
    if (!m_chart || !m_persist_hook || m_path.empty()) return;
    if (m_backup) {
        std::string err;
        if (!m_persist_hook(*m_chart, m_path + ".bak", &err)) {
            m_last_backup_error = err.empty() ? ("崩溃备份写入失败: " + m_path + ".bak") : err;
        } else {
            m_last_backup_error.clear();
        }
    }
    if (m_autosave) {
        (void)m_persist_hook(*m_chart, m_path,
                             nullptr);  // 自动保存失败下次再试；不覆盖 backup 错误
    }
}

std::string EditorSession::undo_label() const {
    return m_undo.empty() ? std::string() : m_undo.back()->describe();
}

std::string EditorSession::redo_label() const {
    return m_redo.empty() ? std::string() : m_redo.back()->describe();
}

// ---------- PutNoteCommand ----------

PutNoteCommand::PutNoteCommand(NoteRef at, bool ln_kind, NoteKind kind)
    : m_at(at), m_ln_kind(ln_kind), m_kind(kind) {}

void PutNoteCommand::apply(Chart& chart) {
    const std::size_t at = lower_bound_pos(chart.notes, m_at.measure, m_at.pos);
    Event<Note> ev;
    ev.measure = m_at.measure;
    ev.pos = m_at.pos;
    ev.value.lane = m_at.lane;
    ev.value.sample.id = m_at.sample;
    ev.value.kind = m_kind;
    ev.value.sub_line = (m_at.lane.kind == LaneKind::Bgm) ? m_at.sub_line : 0;
    // LN 放置：note 数据落位后标记「位于 LN 通道」（51-69）——配对由 EditorSession
    // 的统一 rebuild 推导（2026-09 用户原则：ln_pair 是派生的，命令不维护）。
    ev.value.ln_channel = m_ln_kind && m_kind == NoteKind::Normal;
    chart.notes.insert(chart.notes.begin() + static_cast<std::ptrdiff_t>(at), ev);
    m_applied_index = at;
    m_paired_head.reset();
}

void PutNoteCommand::invert(Chart& chart) {
    if (!m_applied_index || *m_applied_index >= chart.notes.size()) return;
    const std::size_t at = *m_applied_index;
    // 解除配对（若 apply 配对了）：头 ln_pair 清空
    if (m_paired_head && *m_paired_head < chart.notes.size()) {
        chart.notes[*m_paired_head].value.ln_pair.reset();
    }
    shift_pairs_after(chart.notes, at, -1);
    chart.notes.erase(chart.notes.begin() + static_cast<std::ptrdiff_t>(at));
    m_applied_index.reset();
    m_paired_head.reset();
}

std::string PutNoteCommand::describe() const {
    std::string s = "放置 note (m" + std::to_string(m_at.measure) + " @" +
                    std::to_string(m_at.pos.num) + "/" + std::to_string(m_at.pos.den) + ")";
    if (m_kind == NoteKind::Landmine)
        s += " 地雷";
    else if (m_ln_kind)
        s += " LN";
    return s;
}

// ---------- MoveNoteCommand ----------

MoveNoteCommand::MoveNoteCommand(NoteRef from, std::uint32_t to_measure, Rational to_pos,
                                 std::optional<Lane> to_lane,
                                 std::optional<std::uint32_t> to_sub_line)
    : m_from(from),
      m_to_measure(to_measure),
      m_to_pos(to_pos),
      m_to_lane(to_lane),
      m_to_sub_line(to_sub_line) {}

// NOLINTNEXTLINE(readability-function-cognitive-complexity)——编辑命令语义分支（配对/地雷/BGM 行）
void MoveNoteCommand::apply(Chart& chart) {
    const auto idx = find_note(chart.notes, m_from.measure, m_from.pos, m_from.lane, m_from.sample,
                               m_from.sub_line);
    if (!idx) return;             // 找不到 → 无操作
    m_moved = chart.notes[*idx];  // 快照（invert 恢复）
    m_partner.reset();
    m_paired_at.reset();
    m_paired_with.reset();

    // LN 语义（2026-09 用户最终确认）：移动**只移动选中的 note**，不移动伙伴、
    // **不自动重连**（前几版「成对移动/向前找最近重连」导致串 note，已删除）。
    // ln_pair 唯二作用：① LN 中段接线绘制 ②（BMS 用不着的）note 类型判断。
    // 这里只做「按伙伴值重定位下标」：伙伴原地不动，主 note 移走后仍与伙伴互指。
    std::optional<std::tuple<std::uint32_t, Rational, Lane, std::uint32_t, std::uint32_t>>
        partner_val;
    if (m_moved->value.ln_pair && *m_moved->value.ln_pair < chart.notes.size()) {
        const auto& p = chart.notes[*m_moved->value.ln_pair];
        partner_val =
            std::make_tuple(p.measure, p.pos, p.value.lane, p.value.sample.id, p.value.sub_line);
    }

    // 从容器取走主 note（只这一个；伙伴不动）
    Event<Note> main_ev = chart.notes[*idx];
    const std::size_t removed_at = *idx;
    chart.notes.erase(chart.notes.begin() + static_cast<std::ptrdiff_t>(removed_at));
    // 其余配对端下标修正（删除点之后的 -1；主 note 自身 ln_pair 稍后按值重设）
    for (std::size_t i = 0; i < chart.notes.size(); ++i) {
        auto& p = chart.notes[i].value.ln_pair;
        if (!p) continue;
        if (*p > removed_at) --*p;
        if (*p == removed_at)
            p.reset();  // 指向被删 note 的伙伴（partner_val 场景重设；无伙伴=无关）
    }

    // 主 note 落位：目标 (measure,pos) + 可选换轨 + BGM 行号更新
    main_ev.measure = m_to_measure;
    main_ev.pos = m_to_pos;
    if (m_to_lane) main_ev.value.lane = *m_to_lane;  // 跨通道：改到目标轨道
    if (main_ev.value.lane.kind == LaneKind::Bgm) {
        if (m_to_sub_line) {
            main_ev.value.sub_line = *m_to_sub_line;
        } else {
            // 自动分配（2026-09 用户反馈问题1）：目标小节 ch01 行号 = FIFO 虚拟子通道。
            // 优先填目标小节中该 (pos) 未占用的最小行号；否则追加行尾（max+1）。
            // ⚠️ 2026-09 多选全转 BGM：占用判定看**位置**而非 (pos,sample)——每个 note 保留
            // 自身 sample，同位置不同采样的多个 note 须落在**不同行**（ch01 每行一个事件，
            // 同位置同 sample 是重复、同位置不同 sample 须并列），否则写出会同格覆盖。
            std::uint32_t used = 0;
            std::uint32_t max_line = 0;
            for (const auto& n : chart.notes) {
                if (n.measure != m_to_measure || n.value.lane.kind != LaneKind::Bgm) continue;
                max_line = std::max(max_line, n.value.sub_line + 1);
                if (n.value.sub_line < 32 && n.pos == m_to_pos) used |= (1u << n.value.sub_line);
            }
            std::uint32_t line = 0;
            while (line < 32 && (used & (1u << line))) ++line;
            main_ev.value.sub_line = (line < 32) ? line : max_line;
        }
    } else {
        main_ev.value.sub_line = 0;
    }
    // LN 通道标记（2026-09 用户验证）：
    // - **换轨移动**（cross_lane）→ 目标 = 普通游玩通道：本 note ln_channel=false，
    //   **同时原伙伴也 ln_channel=false**（数据层两组都退出 LN 通道——用户场景 2：
    //   「移走头后头尾都回退普通通道，移回不再连成 LN」）；
    // - **同通道时间移动**（不换轨）→ 保持 ln_channel（LN 不因时间平移丢失；
    //   rebuild 按同组时间序重排，场景 1 的「仍 LN 通道 + lint 深色」由未配对覆盖）。
    const bool cross_lane =
        m_to_lane && *m_to_lane != m_from.lane &&
        !(m_to_lane->kind == LaneKind::Bgm && m_from.lane.kind == LaneKind::Bgm &&
          m_to_lane->player == m_from.lane.player && m_to_lane->index == m_from.lane.index);
    if (cross_lane) {
        main_ev.value.ln_channel = false;
        // 原伙伴（若存在 ln_pair）也退出 LN 通道（数据层；rebuild 会清其配对）
        if (m_moved->value.ln_pair && *m_moved->value.ln_pair < chart.notes.size() &&
            chart.notes.size() > *m_moved->value.ln_pair) {
            // 注意：主 note 已删除、下标已调——伙伴下标在删除前记录，这里用 partner_val 找
        }
        if (partner_val) {
            const auto& [pm, pp, pl, ps, pbl] = *partner_val;
            const auto partner_idx = find_note(chart.notes, pm, pp, pl, ps, pbl);
            if (partner_idx && *partner_idx < chart.notes.size())
                chart.notes[*partner_idx].value.ln_channel = false;
        }
    }
    // 其余（同通道/BGM 同行）保持 main_ev 自带 ln_channel（moved 快照原值）
    main_ev.value.ln_pair.reset();  // 稍后按伙伴值重设
    const std::size_t main_at = lower_bound_pos(chart.notes, m_to_measure, m_to_pos);
    chart.notes.insert(chart.notes.begin() + static_cast<std::ptrdiff_t>(main_at), main_ev);

    // 主 note 插入后：其余配对端下标修正（插入点后 +1）
    for (std::size_t i = 0; i < chart.notes.size(); ++i) {
        auto& p = chart.notes[i].value.ln_pair;
        if (!p) continue;
        if (i == main_at) continue;  // 主 note 自身稍后重设
        if (main_at <= i && *p >= main_at) ++*p;
    }

    // 配对：编辑命令不维护 ln_pair——EditorSession 的 rebuild_ln_pairs 统一推导
    // （按 ln_channel 分组、时间序交替）。跨通道后本 note+伙伴 ln_channel 已在上面
    // 置 false → rebuild 不会配；同通道时间移动保持 ln_channel → rebuild 仍配。
    m_last_delta = (m_to_pos - m_from.pos);
}

// NOLINTNEXTLINE(readability-function-cognitive-complexity,readability-function-size)——逆操作精确回滚分支
void MoveNoteCommand::invert(Chart& chart) {
    if (!m_moved) return;  // apply 未执行（找不到 note）→ 无操作
    // 反向：把 (m_to_measure, m_to_pos, 当前lane, sample) 移回 (m_from.measure, m_from.pos)
    // 当前 lane：跨通道后 note 在 to_lane；纯时间移动 = m_from.lane。
    const Lane cur_lane = m_to_lane.value_or(m_from.lane);
    // 当前 BGM 行：apply 后的行号 = 显式 to_sub_line 或 apply 自动分配。invert 时无法
    // 精确重建自动分配值 → 用「目标小节内同 (pos,lane,sample) 的 Bgm note」消歧：
    // 遍历该 measure 的 Bgm note 找匹配的 sub_line（通常唯一）。
    std::uint32_t cur_sub_line = 0;
    if (cur_lane.kind == LaneKind::Bgm) {
        if (m_to_sub_line) {
            cur_sub_line = *m_to_sub_line;
        } else {
            for (const auto& n : chart.notes) {
                if (n.measure == m_to_measure && n.value.lane == cur_lane &&
                    n.value.sample.id == m_from.sample && n.pos == m_to_pos) {
                    cur_sub_line = n.value.sub_line;
                    break;
                }
            }
        }
    }
    const auto idx =
        find_note(chart.notes, m_to_measure, m_to_pos, cur_lane, m_from.sample, cur_sub_line);
    if (!idx) return;

    // LN 语义（与 apply 对称）：只移回选中 note，保持与伙伴互指（按伙伴值重定位）。
    // 伙伴原地不动（apply 期间伙伴从未移动）。
    std::optional<std::tuple<std::uint32_t, Rational, Lane, std::uint32_t, std::uint32_t>>
        partner_val;
    const auto ln = chart.notes[*idx].value.ln_pair;
    if (ln && *ln < chart.notes.size()) {
        const auto& p = chart.notes[*ln];
        partner_val =
            std::make_tuple(p.measure, p.pos, p.value.lane, p.value.sample.id, p.value.sub_line);
    }

    Event<Note> main_ev = chart.notes[*idx];
    const std::size_t removed_at = *idx;
    chart.notes.erase(chart.notes.begin() + static_cast<std::ptrdiff_t>(removed_at));
    // 其余配对端下标修正（删除点之后的 -1；主 note 自身 ln_pair 稍后按值重设）
    for (std::size_t i = 0; i < chart.notes.size(); ++i) {
        auto& p = chart.notes[i].value.ln_pair;
        if (!p) continue;
        if (*p > removed_at) --*p;
        if (*p == removed_at) p.reset();
    }

    // 主 note 移回源位置 + 恢复源 lane + 源 BGM 行 + **源 ln_channel**（apply 换轨时清了）
    main_ev.measure = m_from.measure;
    main_ev.pos = m_from.pos;
    if (m_to_lane) main_ev.value.lane = m_from.lane;
    main_ev.value.sub_line = (main_ev.value.lane.kind == LaneKind::Bgm) ? m_from.sub_line : 0;
    main_ev.value.ln_channel = m_moved->value.ln_channel;  // undo 恢复原 LN 通道标记
    main_ev.value.ln_pair.reset();
    const std::size_t main_at = lower_bound_pos(chart.notes, m_from.measure, m_from.pos);
    chart.notes.insert(chart.notes.begin() + static_cast<std::ptrdiff_t>(main_at), main_ev);

    // 插入后其余配对端下标修正
    for (std::size_t i = 0; i < chart.notes.size(); ++i) {
        auto& p = chart.notes[i].value.ln_pair;
        if (!p) continue;
        if (i == main_at) continue;
        if (main_at <= i && *p >= main_at) ++*p;
    }

    // 按伙伴值重定位互指（伙伴原地未动）；**伙伴 ln_channel 也恢复**（apply 换轨时清了）
    if (partner_val) {
        const auto& [pm, pp, pl, ps, pbl] = *partner_val;
        const auto partner_idx = find_note(chart.notes, pm, pp, pl, ps, pbl);
        if (partner_idx && *partner_idx != main_at && main_at < chart.notes.size()) {
            chart.notes[main_at].value.ln_pair = static_cast<std::uint32_t>(*partner_idx);
            chart.notes[*partner_idx].value.ln_pair = static_cast<std::uint32_t>(main_at);
            chart.notes[*partner_idx].value.ln_channel =
                m_moved->value.ln_channel;  // 伙伴恢复 LN 通道（与主同源状态）
        }
    }
    m_moved.reset();
    m_partner.reset();
    m_paired_at.reset();
    m_paired_with.reset();
}

bool MoveNoteCommand::merge_with(const EditCommand& next) {
    // 连续移动同一 note：next 的 from == 本命令的 to 且 lane/sample/目标轨道一致 → 并入
    const auto* mv = dynamic_cast<const MoveNoteCommand*>(&next);
    if (!mv) return false;
    if (mv->m_from.measure == m_to_measure && mv->m_from.pos == m_to_pos &&
        mv->m_from.lane == m_from.lane && mv->m_from.sample == m_from.sample &&
        mv->m_to_lane == m_to_lane) {
        m_to_measure = mv->m_to_measure;
        m_to_pos = mv->m_to_pos;
        m_to_lane = mv->m_to_lane;
        m_to_sub_line = mv->m_to_sub_line;
        return true;
    }
    return false;
}

std::string MoveNoteCommand::describe() const {
    std::string s = "移动 note (m" + std::to_string(m_from.measure) + " → m" +
                    std::to_string(m_to_measure) + ")";
    if (m_to_lane) s += "（换轨）";
    return s;
}

// ---------- MoveLnPairCommand（LN 头尾整体换轨） ----------

MoveLnPairCommand::MoveLnPairCommand(NoteRef from, Lane to_lane)
    : m_from(from), m_to_lane(to_lane) {}

void MoveLnPairCommand::apply(Chart& chart) {
    const auto idx = find_note(chart.notes, m_from.measure, m_from.pos, m_from.lane, m_from.sample,
                               m_from.sub_line);
    if (!idx) return;
    const auto pair = chart.notes[*idx].value.ln_pair;
    if (!pair || *pair >= chart.notes.size()) return;  // 无配对：调用方应先拒绝
    m_a = chart.notes[*idx];
    m_b = chart.notes[*pair];
    const std::size_t lo = std::min(*idx, static_cast<std::size_t>(*pair));
    const std::size_t hi = std::max(*idx, static_cast<std::size_t>(*pair));
    chart.notes.erase(chart.notes.begin() + static_cast<std::ptrdiff_t>(hi));
    chart.notes.erase(chart.notes.begin() + static_cast<std::ptrdiff_t>(lo));

    auto move_event = [&](Event<Note> ev) {
        ev.value.lane = m_to_lane;
        ev.value.sub_line = 0;
        const auto at = lower_bound_pos(chart.notes, ev.measure, ev.pos);
        chart.notes.insert(chart.notes.begin() + static_cast<std::ptrdiff_t>(at), ev);
    };
    move_event(*m_a);
    move_event(*m_b);
    m_applied = true;
}

void MoveLnPairCommand::invert(Chart& chart) {
    if (!m_applied || !m_a || !m_b) return;
    const auto idx_a =
        find_note(chart.notes, m_a->measure, m_a->pos, m_to_lane, m_a->value.sample.id, 0);
    const auto idx_b =
        find_note(chart.notes, m_b->measure, m_b->pos, m_to_lane, m_b->value.sample.id, 0);
    if (!idx_a || !idx_b) return;
    const std::size_t lo = std::min(*idx_a, *idx_b);
    const std::size_t hi = std::max(*idx_a, *idx_b);
    chart.notes.erase(chart.notes.begin() + static_cast<std::ptrdiff_t>(hi));
    chart.notes.erase(chart.notes.begin() + static_cast<std::ptrdiff_t>(lo));
    auto restore = [&](const Event<Note>& ev) {
        const auto at = lower_bound_pos(chart.notes, ev.measure, ev.pos);
        chart.notes.insert(chart.notes.begin() + static_cast<std::ptrdiff_t>(at), ev);
    };
    restore(*m_a);
    restore(*m_b);
    m_applied = false;
}

std::string MoveLnPairCommand::describe() const {
    return "移动 LN（m" + std::to_string(m_from.measure) + "，换轨）";
}

// ---------- ToggleLnCommand（单点 ↔ LN；只改 ln_channel，rebuild 接管配对） ----------

ToggleLnCommand::ToggleLnCommand(NoteRef ref) : m_ref(ref) {}

void ToggleLnCommand::apply(Chart& chart) {
    const auto idx =
        find_note(chart.notes, m_ref.measure, m_ref.pos, m_ref.lane, m_ref.sample, m_ref.sub_line);
    if (!idx) return;
    // 2026-09 用户最终确认：T-Ln 工具 = **按当前 LNTYPE 切换选中 note 的通道**——
    //   LNTYPE 1：单点→LN = 通道 +40（11→51，模型 = ln_channel=true）；LN→单点 = -40。
    //   LNTYPE 2（#LNOBJ）：单点→LN = id 变 LNOBJ；LN→单点 = id 变 Ln 头 id。
    //   模型层用 ln_channel 表达（LNTYPE 1 语义）；**不向前查询、不标记伙伴**——
    //   干净、便于 debug。配对交给 rebuild（仅 ln_channel note 参与）。
    //   ⚠️ 旧逻辑（向前找最近同 lane 同 sample 并标记为 LN）已注释保留在下方，勿删。
    //   // 单点 → LN：找候选伙伴（同 lane 同 sample；优先最近）
    //   // for (std::size_t i = *idx; i-- > 0;) {
    //   //     const auto& c = chart.notes[i].value;
    //   //     if (c.lane != m_ref.lane || c.sample.id != m_ref.sample) continue;
    //   //     if (!c.ln_channel) chart.notes[i].value.ln_channel = true;
    //   //     m_applied_partner = static_cast<std::uint32_t>(i);
    //   //     break;
    //   // }
    Note& n = chart.notes[*idx].value;
    m_was_ln = n.ln_channel;
    n.ln_channel = !m_was_ln;
    m_did_change = true;
}

void ToggleLnCommand::invert(Chart& chart) {
    if (!m_did_change) return;
    const auto idx =
        find_note(chart.notes, m_ref.measure, m_ref.pos, m_ref.lane, m_ref.sample, m_ref.sub_line);
    if (!idx) return;
    // 反向：只翻回本 note 的 ln_channel（原伙伴标记逻辑已去——与 apply 对称）
    chart.notes[*idx].value.ln_channel = m_was_ln;
    m_did_change = false;
    m_was_ln = false;
    m_applied_partner.reset();
}

std::string ToggleLnCommand::describe() const {
    return "单点/LN 转换 (m" + std::to_string(m_ref.measure) + ")";
}

// ---------- ConvertNoteCommand（note ↔ BGA/BPM/STOP 跨命名空间转换） ----------

ConvertNoteCommand::ConvertNoteCommand(NoteRef from, ConvertTarget target, std::uint32_t to_measure,
                                       Rational to_pos)
    : m_from(from), m_target(target), m_to_measure(to_measure), m_to_pos(to_pos) {}

void ConvertNoteCommand::apply(Chart& chart) {
    const auto idx = find_note(chart.notes, m_from.measure, m_from.pos, m_from.lane, m_from.sample,
                               m_from.sub_line);
    if (!idx) return;  // 找不到 → 无操作
    m_removed = chart.notes[*idx];
    m_partner.reset();
    // 移除 note（LN 配对若存在：断开伙伴 ln_pair，快照保存供 invert 恢复）
    if (const auto p = m_removed->value.ln_pair) {
        if (*p < chart.notes.size()) {  // 先验范围，再解引用（悬挂 ln_pair 不得越界读）
            const auto pp = chart.notes[*p].value.ln_pair;
            if (pp && *pp == *idx) {
                m_partner = chart.notes[*p];
                chart.notes[*p].value.ln_pair.reset();
            }
        }
    }
    chart.notes.erase(chart.notes.begin() + static_cast<std::ptrdiff_t>(*idx));
    shift_pairs_after(chart.notes, *idx, -1);
    m_insert_index.reset();
    m_ref_id.reset();
    m_bga_layer = -1;
    m_value = 0.0;

    switch (m_target) {
        case ConvertTarget::BgaBase:
        case ConvertTarget::BgaPoor:
        case ConvertTarget::BgaLayer:
        case ConvertTarget::BgaLayer2: {
            const int layer = static_cast<int>(m_target);
            Bga bga;
            bga.image.id = m_from.sample;  // id 不变：note 的 #WAVxx → #BMPxx（同文本 id）
            bga.layer = layer;
            chart.bga_events.push_back({m_to_measure, m_to_pos, bga});
            m_insert_index = chart.bga_events.size() - 1;
            m_bga_layer = layer;
            break;
        }
        case ConvertTarget::Bpm: {
            // id → #BPMxx 引用：ref_id = m_from.sample；value 由定义表解析（缺省 130）
            double v = 130.0;
            if (const auto it = chart.samples.find({SampleKind::Bpm, m_from.sample});
                it != chart.samples.end()) {
                char* end = nullptr;
                const double d = std::strtod(it->second.value.c_str(), &end);
                if (end != it->second.value.c_str() && *end == '\0') v = d;
            }
            Bpm bpm;
            bpm.value = v;
            bpm.ref_id = m_from.sample;
            chart.bpm_events.push_back({m_to_measure, m_to_pos, bpm});
            m_insert_index = chart.bpm_events.size() - 1;
            m_ref_id = m_from.sample;
            m_value = v;
            break;
        }
        case ConvertTarget::Stop: {
            // id → #STOPxx 引用：ref_id = m_from.sample；count = 原始计数（解析定义表文本）
            std::int64_t count = 0;
            if (const auto it = chart.samples.find({SampleKind::Stop, m_from.sample});
                it != chart.samples.end()) {
                char* end = nullptr;
                const double d = std::strtod(it->second.value.c_str(), &end);
                if (end != it->second.value.c_str() && *end == '\0')
                    count = static_cast<std::int64_t>(std::llround(d));
            }
            Stop stop;
            stop.count = count;
            stop.ref_id = m_from.sample;
            chart.stop_events.push_back({m_to_measure, m_to_pos, stop});
            m_insert_index = chart.stop_events.size() - 1;
            m_ref_id = m_from.sample;
            m_value = static_cast<double>(count);
            break;
        }
    }
}

void ConvertNoteCommand::invert(Chart& chart) {
    if (!m_removed) return;  // apply 未执行 → 无操作
    // 移除转换出的目标事件（按 m_insert_index；若容器已变（后续编辑）→ 按值定位）
    auto erase_target = [&](auto& evs) {
        if (m_insert_index && *m_insert_index < evs.size()) {
            const auto& e = evs[*m_insert_index];
            if (e.measure == m_to_measure && e.pos == m_to_pos) {
                evs.erase(evs.begin() + static_cast<std::ptrdiff_t>(*m_insert_index));
                return;
            }
        }
        for (std::size_t i = 0; i < evs.size(); ++i) {
            if (evs[i].measure == m_to_measure && evs[i].pos == m_to_pos) {
                evs.erase(evs.begin() + static_cast<std::ptrdiff_t>(i));
                return;
            }
        }
    };
    switch (m_target) {
        case ConvertTarget::BgaBase:
        case ConvertTarget::BgaPoor:
        case ConvertTarget::BgaLayer:
        case ConvertTarget::BgaLayer2:
            erase_target(chart.bga_events);
            break;
        case ConvertTarget::Bpm:
            erase_target(chart.bpm_events);
            break;
        case ConvertTarget::Stop:
            erase_target(chart.stop_events);
            break;
    }
    // 恢复 note（原容器位置近似；配对按快照重连）
    const std::size_t at = lower_bound_pos(chart.notes, m_from.measure, m_from.pos);
    Event<Note> ev = *m_removed;
    ev.value.ln_pair.reset();
    chart.notes.insert(chart.notes.begin() + static_cast<std::ptrdiff_t>(at), ev);
    shift_pairs_after(chart.notes, at, +1);
    if (m_partner) {
        // 伙伴按快照值重定位（伙伴未移动；快照 (measure,pos,lane,sample,sub_line) 可定位）
        const auto pp =
            find_note(chart.notes, m_partner->measure, m_partner->pos, m_partner->value.lane,
                      m_partner->value.sample.id, m_partner->value.sub_line);
        if (pp && *pp != at && at < chart.notes.size()) {
            chart.notes[at].value.ln_pair = *pp;
            chart.notes[*pp].value.ln_pair = at;
        }
    }
    m_removed.reset();
    m_partner.reset();
    m_insert_index.reset();
    m_ref_id.reset();
    m_bga_layer = -1;
}

std::string ConvertNoteCommand::describe() const {
    std::string s = "转换 note (m" + std::to_string(m_from.measure) + " @" +
                    std::to_string(m_from.pos.num) + "/" + std::to_string(m_from.pos.den) + ")";
    switch (m_target) {
        case ConvertTarget::BgaBase:
            s += " → BGA";
            break;
        case ConvertTarget::BgaPoor:
            s += " → POOR";
            break;
        case ConvertTarget::BgaLayer:
            s += " → LAYER";
            break;
        case ConvertTarget::BgaLayer2:
            s += " → LAYER2";
            break;
        case ConvertTarget::Bpm:
            s += " → BPM";
            break;
        case ConvertTarget::Stop:
            s += " → STOP";
            break;
    }
    return s;
}

// ---------- DeleteNoteCommand ----------

DeleteNoteCommand::DeleteNoteCommand(NoteRef ref) : m_ref(ref) {}

void DeleteNoteCommand::apply(Chart& chart) {
    const auto idx =
        find_note(chart.notes, m_ref.measure, m_ref.pos, m_ref.lane, m_ref.sample, m_ref.sub_line);
    if (!idx) return;
    m_removed = chart.notes[*idx];
    m_removed_index = *idx;
    m_partner.reset();
    // 记录伙伴快照（invert 恢复配对用；伙伴 ln_pair 将被清空，需保存原状）
    if (const auto p = m_removed->value.ln_pair) {
        if (*p < chart.notes.size()) {
            m_partner = chart.notes[*p];
            chart.notes[*p].value.ln_pair.reset();
        }
    }
    chart.notes.erase(chart.notes.begin() + static_cast<std::ptrdiff_t>(*idx));
    shift_pairs_after(chart.notes, *idx, -1);
}

void DeleteNoteCommand::invert(Chart& chart) {
    if (!m_removed) return;
    std::size_t at = m_removed_index;
    if (at > chart.notes.size()) at = chart.notes.size();
    // 恢复前，原下标 >= at 的配对端 +1（插入位移）
    shift_pairs_after(chart.notes, at, +1);
    chart.notes.insert(chart.notes.begin() + static_cast<std::ptrdiff_t>(at), *m_removed);
    // 恢复配对：伙伴按快照值重定位（伙伴未移动，快照 (measure,pos,lane,sample) 可直接定位）
    if (m_partner) {
        const auto pp = find_note(chart.notes, m_partner->measure, m_partner->pos,
                                  m_partner->value.lane, m_partner->value.sample.id);
        if (pp && *pp != at && at < chart.notes.size()) {
            chart.notes[at].value.ln_pair = *pp;
            chart.notes[*pp].value.ln_pair = at;
        }
    }
    m_removed.reset();
    m_partner.reset();
}

std::string DeleteNoteCommand::describe() const {
    return "删除 note (m" + std::to_string(m_ref.measure) + " @" + std::to_string(m_ref.pos.num) +
           "/" + std::to_string(m_ref.pos.den) + ")";
}

// ---------- CompositeCommand ----------

void CompositeCommand::add(std::unique_ptr<EditCommand> cmd) {
    if (cmd) m_commands.push_back(std::move(cmd));
}

std::string CompositeCommand::name() const {
    if (m_commands.empty()) return "composite";
    return "composite." + m_commands.front()->name();
}

bool CompositeCommand::changes_time() const {
    for (const auto& c : m_commands)
        if (c->changes_time()) return true;
    return false;
}

bool CompositeCommand::changes_lane() const {
    for (const auto& c : m_commands)
        if (c->changes_lane()) return true;
    return false;
}

void CompositeCommand::apply(Chart& chart) {
    for (std::size_t begin = 0; begin < m_commands.size();) {
        auto* transform = dynamic_cast<TransformNoteCommand*>(m_commands[begin].get());
        if (!transform) {
            m_commands[begin++]->apply(chart);
            continue;
        }
        // 连续变换构成一个批次：先在未改写的容器上认领源，再统一写目标。
        // 只改 lane 不增删/重排 notes，因此读写之间索引稳定。
        std::unordered_set<std::size_t> claimed;
        std::size_t end = begin;
        for (; end < m_commands.size(); ++end) {
            auto* c = dynamic_cast<TransformNoteCommand*>(m_commands[end].get());
            if (!c) break;
            c->bind_source(chart, claimed);
        }
        for (; begin < end; ++begin) {
            static_cast<TransformNoteCommand*>(m_commands[begin].get())->apply_bound(chart);
        }
    }
}

void CompositeCommand::invert(Chart& chart) {
    for (std::size_t end = m_commands.size(); end > 0;) {
        if (!dynamic_cast<TransformNoteCommand*>(m_commands[end - 1].get())) {
            m_commands[--end]->invert(chart);
            continue;
        }
        // 其它命令的逆操作可能改变同拍顺序，撤销同样先统一定位再写回。
        std::unordered_set<std::size_t> claimed;
        std::size_t begin = end;
        while (begin > 0) {
            auto* c = dynamic_cast<TransformNoteCommand*>(m_commands[begin - 1].get());
            if (!c) break;
            c->bind_target(chart, claimed);
            --begin;
        }
        while (end > begin) {
            static_cast<TransformNoteCommand*>(m_commands[--end].get())->invert_bound(chart);
        }
    }
}

std::string CompositeCommand::describe() const {
    if (m_commands.empty()) return "批量操作";
    if (m_commands.size() == 1) return m_commands.front()->describe();
    return "批量操作（" + std::to_string(m_commands.size()) + " 项）";
}

// ---------- ShiftMarkerCommand ----------

ShiftMarkerCommand::ShiftMarkerCommand(std::unique_ptr<EditCommand> inner, bool time, bool lane)
    : m_inner(std::move(inner)), m_time(time), m_lane(lane) {}

std::string ShiftMarkerCommand::name() const { return m_inner ? m_inner->name() : "shift.marker"; }

void ShiftMarkerCommand::apply(Chart& chart) {
    if (m_inner) m_inner->apply(chart);
}

void ShiftMarkerCommand::invert(Chart& chart) {
    if (m_inner) m_inner->invert(chart);
}

std::string ShiftMarkerCommand::describe() const {
    return m_inner ? m_inner->describe() : "维度变更";
}

// ---------- Selection ----------

// NOLINTNEXTLINE(readability-function-size)——固有分支密度（解析/协议形状），有意保留
void Selection::add_rect(std::uint32_t measure_lo, std::uint32_t measure_hi,
                         const std::vector<Lane>& lanes, Rational pos_lo, Rational pos_hi,
                         const std::vector<NoteRef>& candidates) {
    for (const auto& ref : candidates) {
        if (ref.measure < measure_lo || ref.measure > measure_hi) continue;
        if (ref.pos < pos_lo || pos_hi < ref.pos) continue;  // 用 < 表达区间（Rational 无 <=）
        if (!lanes.empty()) {
            bool in_lanes = false;
            for (const auto& l : lanes) {
                if (l == ref.lane) {
                    in_lanes = true;
                    break;
                }
            }
            if (!in_lanes) continue;
        }
        m_refs.insert(ref);
    }
}

// ---------- PutTimingCommand ----------

PutTimingCommand::PutTimingCommand(TimingKind kind, std::uint32_t measure, Rational pos,
                                   double value, std::optional<std::uint32_t> ref)
    : m_kind(kind), m_measure(measure), m_pos(pos), m_value(value), m_ref(ref) {
    if (m_kind == TimingKind::Measure) m_pos = Rational(0, 1);  // 节拍恒 pos 0
}

// 设置 (kind 事件) 的 ref_id（Bpm/Stop；Measure/Bga 等无 → no-op）。
// 只对「有 ref_id 字段」的事件类型生效；via 传入指针辅助 lambda。
template <typename T>
void set_ref_if_supported(Event<T>& ev, std::optional<std::uint32_t> ref) {
    if constexpr (std::is_same_v<T, Bpm> || std::is_same_v<T, Stop>) ev.value.ref_id = ref;
}

/// 数值 → 紧凑文本（与 bms_writer::format_num 同规：整数无小数点，小数 17 位有效数字）。
inline std::string format_number(double v) {
    if (std::isfinite(v) && v == std::floor(v) && std::fabs(v) < 1e15)
        return std::to_string(static_cast<long long>(v));
    std::ostringstream os;
    os << std::setprecision(17) << v;
    std::string s = os.str();
    if (s.find('e') == std::string::npos && s.find('.') != std::string::npos) {
        while (!s.empty() && s.back() == '0') s.pop_back();
        if (!s.empty() && s.back() == '.') s.pop_back();
    }
    return s;
}

// NOLINTNEXTLINE(readability-function-cognitive-complexity)——编辑命令语义分支（配对/地雷/BGM 行）
void PutTimingCommand::apply(Chart& chart) {
    m_existed = false;
    m_applied_index.reset();
    m_old_ref.reset();
    m_sync_ref.reset();
    m_sync_bpm.clear();
    m_sync_stop.clear();
    m_edited_index = 0;
    const auto do_apply = [&](auto& evs) {
        using Ev = typename std::remove_reference_t<decltype(evs)>::value_type;
        using V = decltype(std::declval<Ev>().value);  // Bpm / Stop / MeasureLen
        const auto [lo, hi] = find_event_range(evs, m_measure, m_pos);
        if (lo != hi) {  // 同位替换（同 pos 多值：改最后一个 = 引擎「后者覆盖」语义）
            m_existed = true;
            m_edited_index = hi - 1;
            m_old_value = timing_value(evs[hi - 1]);
            if constexpr (std::is_same_v<V, Bpm> || std::is_same_v<V, Stop>)
                m_old_ref = evs[hi - 1].value.ref_id;
            set_timing_value(evs[hi - 1], m_value);
            if (m_ref) set_ref_if_supported(evs[hi - 1], m_ref);
            return;
        }
        Ev ev;
        ev.measure = m_measure;
        ev.pos = m_pos;
        set_timing_value(ev, m_value);
        if (m_ref) set_ref_if_supported(ev, m_ref);
        evs.insert(evs.begin() + static_cast<std::ptrdiff_t>(lo), std::move(ev));
        m_applied_index = lo;
    };
    switch (m_kind) {
        case TimingKind::Bpm:
            do_apply(chart.bpm_events);
            break;
        case TimingKind::Stop:
            do_apply(chart.stop_events);
            break;
        case TimingKind::Measure:
            do_apply(chart.measure_events);
            break;
    }
    // 交叉同步（共享 id 语义，2026-09 用户）：替换的是 ref 绑定事件（含显式新 ref）→
    // **该 #BPMxx/#STOPxx 的定义值 = 新事件值**，所有引用该 id 的事件同步刷新。
    // （BMS 语义：ch08/ch09 行引用一个定义，行本身无独立数值——编辑一个对象 = 编辑定义。）
    const std::optional<std::uint32_t> sync_ref = m_ref ? m_ref : m_old_ref;
    if (m_kind != TimingKind::Bpm && m_kind != TimingKind::Stop) return;
    if (!(sync_ref && *sync_ref != 0)) return;
    if (!m_existed) return;  // 新增事件：值即事件值（写回自动派生），不动已有定义
    // 取替换后的事件值（Bpm=数值 / Stop=计数）
    double val = 0;
    if (m_kind == TimingKind::Bpm) {
        const auto [lo, hi] = find_event_range(chart.bpm_events, m_measure, m_pos);
        if (lo == hi) return;
        val = chart.bpm_events[hi - 1].value.value;
    } else {
        const auto [lo, hi] = find_event_range(chart.stop_events, m_measure, m_pos);
        if (lo == hi) return;
        val = static_cast<double>(chart.stop_events[hi - 1].value.count);
    }
    const std::string text = (m_kind == TimingKind::Stop)
                                 ? format_number(static_cast<double>(std::llround(val)))
                                 : format_number(val);
    const SampleKind sk = (m_kind == TimingKind::Bpm) ? SampleKind::Bpm : SampleKind::Stop;
    const auto key = std::make_pair(sk, *sync_ref);
    const auto it = chart.samples.find(key);
    m_def_existed = it != chart.samples.end();
    if (m_def_existed) m_old_def_text = it->second.value;
    if (m_def_existed && m_old_def_text == text) return;  // 定义值未变 → 无需同步
    m_sync_ref = sync_ref;
    chart.samples[key].value = text;
    // 传播：所有引用该 id 的事件值 = 定义值
    double dv = 0;
    char* end = nullptr;
    const double parsed = std::strtod(text.c_str(), &end);
    if (end != text.c_str() && *end == '\0' && std::isfinite(parsed)) dv = parsed;
    if (sk == SampleKind::Bpm) {
        for (std::size_t i = 0; i < chart.bpm_events.size(); ++i) {
            if (i == m_edited_index) continue;  // 被替换事件由 do_invert 恢复（m_old_value）
            auto& ev = chart.bpm_events[i].value;
            if (ev.ref_id && *ev.ref_id == *sync_ref) {
                m_sync_bpm.emplace_back(i, ev.value);
                ev.value = dv;
            }
        }
    } else {
        for (std::size_t i = 0; i < chart.stop_events.size(); ++i) {
            if (i == m_edited_index) continue;
            auto& ev = chart.stop_events[i].value;
            if (ev.ref_id && *ev.ref_id == *sync_ref) {
                m_sync_stop.emplace_back(i, ev.count);
                ev.count = static_cast<std::int64_t>(std::llround(dv));
            }
        }
    }
}

// NOLINTNEXTLINE(readability-function-cognitive-complexity,readability-function-size)——逆操作精确回滚分支
void PutTimingCommand::invert(Chart& chart) {
    const auto do_invert = [&](auto& evs) {
        if (m_existed) {
            const auto [lo, hi] = find_event_range(evs, m_measure, m_pos);
            if (lo != hi) {
                set_timing_value(evs[hi - 1], m_old_value);  // 恢复旧值
                using Ev = typename std::remove_reference_t<decltype(evs)>::value_type;
                using V = decltype(std::declval<Ev>().value);
                if constexpr (std::is_same_v<V, Bpm> || std::is_same_v<V, Stop>) {
                    evs[hi - 1].value.ref_id = m_old_ref;
                }
            }
            return;
        }
        if (m_applied_index && *m_applied_index < evs.size()) {
            evs.erase(evs.begin() + static_cast<std::ptrdiff_t>(*m_applied_index));
        }
        m_applied_index.reset();
    };
    switch (m_kind) {
        case TimingKind::Bpm:
            do_invert(chart.bpm_events);
            break;
        case TimingKind::Stop:
            do_invert(chart.stop_events);
            break;
        case TimingKind::Measure:
            do_invert(chart.measure_events);
            break;
    }
    // 交叉同步 invert：恢复定义 + 各引用者旧值
    if (m_sync_ref) {
        const SampleKind sk = (m_kind == TimingKind::Bpm) ? SampleKind::Bpm : SampleKind::Stop;
        const auto key = std::make_pair(sk, *m_sync_ref);
        if (m_def_existed) {
            chart.samples[key].value = m_old_def_text;
        } else {
            chart.samples.erase(key);
        }
        for (const auto& [idx, old] : m_sync_bpm)
            if (idx < chart.bpm_events.size()) chart.bpm_events[idx].value.value = old;
        for (const auto& [idx, old] : m_sync_stop)
            if (idx < chart.stop_events.size()) chart.stop_events[idx].value.count = old;
        m_sync_ref.reset();
        m_sync_bpm.clear();
        m_sync_stop.clear();
    }
}

std::string PutTimingCommand::describe() const {
    std::string name(timing_kind_name(m_kind));
    return "设置 " + name + " (m" + std::to_string(m_measure) + " @" + std::to_string(m_pos.num) +
           "/" + std::to_string(m_pos.den) + ")";
}

// ---------- DeleteTimingCommand ----------

DeleteTimingCommand::DeleteTimingCommand(TimingKind kind, std::uint32_t measure, Rational pos)
    : m_kind(kind), m_measure(measure), m_pos(pos) {
    if (m_kind == TimingKind::Measure) m_pos = Rational(0, 1);  // 节拍恒 pos 0
}

void DeleteTimingCommand::apply(Chart& chart) {
    m_bpm.clear();
    m_stop.clear();
    m_measure_evs.clear();
    m_index = 0;
    const auto do_apply = [&](auto& evs, auto& snap) {
        const auto [lo, hi] = find_event_range(evs, m_measure, m_pos);
        for (std::size_t i = lo; i < hi; ++i) snap.push_back(evs[i]);
        if (lo == hi) return;
        m_index = lo;
        evs.erase(evs.begin() + static_cast<std::ptrdiff_t>(lo),
                  evs.begin() + static_cast<std::ptrdiff_t>(hi));
    };
    switch (m_kind) {
        case TimingKind::Bpm:
            do_apply(chart.bpm_events, m_bpm);
            break;
        case TimingKind::Stop:
            do_apply(chart.stop_events, m_stop);
            break;
        case TimingKind::Measure:
            do_apply(chart.measure_events, m_measure_evs);
            break;
    }
}

void DeleteTimingCommand::invert(Chart& chart) {
    const auto do_invert = [&](auto& evs, const auto& snap) {
        if (snap.empty()) return;
        std::size_t at = m_index;
        if (at > evs.size()) at = evs.size();
        for (std::size_t i = 0; i < snap.size(); ++i) {
            evs.insert(evs.begin() + static_cast<std::ptrdiff_t>(at + i), snap[i]);
        }
    };
    switch (m_kind) {
        case TimingKind::Bpm:
            do_invert(chart.bpm_events, m_bpm);
            break;
        case TimingKind::Stop:
            do_invert(chart.stop_events, m_stop);
            break;
        case TimingKind::Measure:
            do_invert(chart.measure_events, m_measure_evs);
            break;
    }
    m_bpm.clear();
    m_stop.clear();
    m_measure_evs.clear();
    m_index = 0;
}

std::string DeleteTimingCommand::describe() const {
    std::string name(timing_kind_name(m_kind));
    return "删除 " + name + " (m" + std::to_string(m_measure) + " @" + std::to_string(m_pos.num) +
           "/" + std::to_string(m_pos.den) + ")";
}

// ---------- BGA 事件命令（bga.put / bga.delete） ----------
// BGA 事件 = (measure, pos, layer) 一个 BMP 切换（0=base 1=poor 2=layer 3=layer2）。
// 线性扫描定位（bga 事件通常不多；不依赖容器严格有序）；写入由 writer 的
// add_cell(measure, channel) 按层号映射通道（04/06/07/0A）分组，容器顺序不影响输出。

namespace {

std::optional<std::size_t> find_bga(const std::vector<Event<Bga>>& evs, std::uint32_t measure,
                                    const Rational& pos, int layer) {
    for (std::size_t i = 0; i < evs.size(); ++i)
        if (evs[i].measure == measure && evs[i].pos == pos && evs[i].value.layer == layer) return i;
    return std::nullopt;
}

}  // namespace

BgaPutCommand::BgaPutCommand(std::uint32_t measure, Rational pos, int layer, std::uint32_t sample)
    : m_measure(measure), m_pos(pos), m_layer(layer), m_sample(sample) {}

void BgaPutCommand::apply(Chart& chart) {
    m_existed = false;
    m_applied_index.reset();
    if (const auto idx = find_bga(chart.bga_events, m_measure, m_pos, m_layer)) {
        m_existed = true;
        m_old_sample = chart.bga_events[*idx].value.image.id;
        chart.bga_events[*idx].value.image.id = m_sample;
        return;
    }
    Event<Bga> ev;
    ev.measure = m_measure;
    ev.pos = m_pos;
    ev.value.layer = m_layer;
    ev.value.image.id = m_sample;
    chart.bga_events.push_back(ev);
    m_applied_index = chart.bga_events.size() - 1;
}

void BgaPutCommand::invert(Chart& chart) {
    if (m_existed) {
        if (const auto idx = find_bga(chart.bga_events, m_measure, m_pos, m_layer))
            chart.bga_events[*idx].value.image.id = m_old_sample;
        return;
    }
    if (m_applied_index && *m_applied_index < chart.bga_events.size())
        chart.bga_events.erase(chart.bga_events.begin() +
                               static_cast<std::ptrdiff_t>(*m_applied_index));
    m_applied_index.reset();
}

std::string BgaPutCommand::describe() const {
    return "设置 BGA (m" + std::to_string(m_measure) + " @" + std::to_string(m_pos.num) + "/" +
           std::to_string(m_pos.den) + ")";
}

BgaDeleteCommand::BgaDeleteCommand(std::uint32_t measure, Rational pos, int layer)
    : m_measure(measure), m_pos(pos), m_layer(layer) {}

void BgaDeleteCommand::apply(Chart& chart) {
    m_removed.reset();
    if (const auto idx = find_bga(chart.bga_events, m_measure, m_pos, m_layer)) {
        m_removed = chart.bga_events[*idx];
        m_removed_index = *idx;
        chart.bga_events.erase(chart.bga_events.begin() + static_cast<std::ptrdiff_t>(*idx));
    }
}

void BgaDeleteCommand::invert(Chart& chart) {
    if (!m_removed) return;
    std::size_t at = m_removed_index;
    if (at > chart.bga_events.size()) at = chart.bga_events.size();
    chart.bga_events.insert(chart.bga_events.begin() + static_cast<std::ptrdiff_t>(at), *m_removed);
    m_removed.reset();
}

std::string BgaDeleteCommand::describe() const {
    return "删除 BGA (m" + std::to_string(m_measure) + " @" + std::to_string(m_pos.num) + "/" +
           std::to_string(m_pos.den) + ")";
}

// ---------- ConvertMetaToNoteCommand（BGA/BPM/STOP → note 反转换） ----------

ConvertMetaToNoteCommand::ConvertMetaToNoteCommand(std::string kind, std::uint32_t measure,
                                                   Rational pos, int layer, Lane to_lane,
                                                   std::uint32_t to_measure, Rational to_pos)
    : m_kind(std::move(kind)),
      m_measure(measure),
      m_pos(pos),
      m_layer(layer),
      m_to_lane(to_lane),
      m_to_measure(to_measure),
      m_to_pos(to_pos) {}

// NOLINTNEXTLINE(readability-function-cognitive-complexity)——编辑命令语义分支（配对/地雷/BGM 行）
void ConvertMetaToNoteCommand::apply(Chart& chart) {
    m_did = false;
    m_bga.reset();
    m_bpm.reset();
    m_stop.reset();
    m_note_index.reset();
    // 定位并删除元事件（bga 按 layer；bpm/stop 无 layer，只按 (measure,pos)）
    if (m_kind == "bga") {
        for (std::size_t i = 0; i < chart.bga_events.size(); ++i) {
            const auto& e = chart.bga_events[i];
            if (e.measure != m_measure || e.pos != m_pos || e.value.layer != m_layer) continue;
            m_bga = chart.bga_events[i];
            m_sample = m_bga->value.image.id;
            m_meta_index = i;
            chart.bga_events.erase(chart.bga_events.begin() + static_cast<std::ptrdiff_t>(i));
            break;
        }
        if (!m_bga) return;
    } else if (m_kind == "bpm") {
        for (std::size_t i = 0; i < chart.bpm_events.size(); ++i) {
            const auto& e = chart.bpm_events[i];
            if (e.measure != m_measure || e.pos != m_pos) continue;
            m_bpm = chart.bpm_events[i];
            if (!m_bpm->value.ref_id) return;  // 内联无 id → 无法反转
            m_sample = *m_bpm->value.ref_id;
            m_meta_index = i;
            chart.bpm_events.erase(chart.bpm_events.begin() + static_cast<std::ptrdiff_t>(i));
            break;
        }
        if (!m_bpm) return;
    } else if (m_kind == "stop") {
        for (std::size_t i = 0; i < chart.stop_events.size(); ++i) {
            const auto& e = chart.stop_events[i];
            if (e.measure != m_measure || e.pos != m_pos) continue;
            m_stop = chart.stop_events[i];
            if (!m_stop->value.ref_id) return;
            m_sample = *m_stop->value.ref_id;
            m_meta_index = i;
            chart.stop_events.erase(chart.stop_events.begin() + static_cast<std::ptrdiff_t>(i));
            break;
        }
        if (!m_stop) return;
    } else {
        return;
    }
    // 插入 note（拖动终点 (to_measure,to_pos)，lane = 目标游玩轨，sample = 原 id）
    const std::size_t at = lower_bound_pos(chart.notes, m_to_measure, m_to_pos);
    Event<Note> ev;
    ev.measure = m_to_measure;
    ev.pos = m_to_pos;
    ev.value.lane = m_to_lane;
    ev.value.sample.id = m_sample;
    ev.value.kind = NoteKind::Normal;
    ev.value.ln_channel = false;
    ev.value.sub_line = 0;
    chart.notes.insert(chart.notes.begin() + static_cast<std::ptrdiff_t>(at), ev);
    m_note_index = at;
    m_did = true;
}

void ConvertMetaToNoteCommand::invert(Chart& chart) {
    if (!m_did) return;
    // 删除转换出的 note（按值/下标）
    std::optional<std::size_t> ni = m_note_index;
    if (ni && *ni < chart.notes.size()) {
        const auto& e = chart.notes[*ni];
        if (e.measure == m_to_measure && e.pos == m_to_pos && e.value.lane == m_to_lane &&
            e.value.sample.id == m_sample) {
            chart.notes.erase(chart.notes.begin() + static_cast<std::ptrdiff_t>(*ni));
        }
    } else {
        for (std::size_t i = 0; i < chart.notes.size(); ++i) {
            const auto& e = chart.notes[i];
            if (e.measure == m_to_measure && e.pos == m_to_pos && e.value.lane == m_to_lane &&
                e.value.sample.id == m_sample) {
                chart.notes.erase(chart.notes.begin() + static_cast<std::ptrdiff_t>(i));
                break;
            }
        }
    }
    m_note_index.reset();
    // 恢复原元事件（在原容器位置；容器已变则追加到尾部）
    if (m_bga) {
        std::size_t pos =
            m_meta_index <= chart.bga_events.size() ? m_meta_index : chart.bga_events.size();
        chart.bga_events.insert(chart.bga_events.begin() + static_cast<std::ptrdiff_t>(pos),
                                *m_bga);
    } else if (m_bpm) {
        std::size_t pos =
            m_meta_index <= chart.bpm_events.size() ? m_meta_index : chart.bpm_events.size();
        chart.bpm_events.insert(chart.bpm_events.begin() + static_cast<std::ptrdiff_t>(pos),
                                *m_bpm);
    } else if (m_stop) {
        std::size_t pos =
            m_meta_index <= chart.stop_events.size() ? m_meta_index : chart.stop_events.size();
        chart.stop_events.insert(chart.stop_events.begin() + static_cast<std::ptrdiff_t>(pos),
                                 *m_stop);
    }
    m_bga.reset();
    m_bpm.reset();
    m_stop.reset();
    m_did = false;
}

std::string ConvertMetaToNoteCommand::describe() const {
    return "转换 " + m_kind + " → note (m" + std::to_string(m_measure) + " @" +
           std::to_string(m_pos.num) + "/" + std::to_string(m_pos.den) + ")";
}

// ---------- QuantizeNoteCommand ----------

QuantizeNoteCommand::QuantizeNoteCommand(NoteRef ref, std::int64_t snap_num, std::int64_t snap_den)
    : m_ref(ref), m_snap_num(snap_num), m_snap_den(snap_den) {
    if (m_snap_num <= 0) m_snap_num = 1;
    if (m_snap_den <= 0) m_snap_den = 1;
}

void QuantizeNoteCommand::apply(Chart& chart) {
    const auto idx = find_note(chart.notes, m_ref.measure, m_ref.pos, m_ref.lane, m_ref.sample);
    if (!idx) return;  // 找不到 → 无操作
    // 量化：k = round(pos * snapDen / snapNum)，目标 pos = k*snapNum/snapDen
    // （Rational 构造自动约分，如 2/4 → 1/2；网格上 pos 本身不变）
    const double k =
        std::round(static_cast<double>(m_ref.pos.num) * static_cast<double>(m_snap_den) /
                   (static_cast<double>(m_ref.pos.den) * static_cast<double>(m_snap_num)));
    const Rational target(static_cast<std::int64_t>(k) * m_snap_num, m_snap_den);
    m_changed = (target != m_ref.pos);
    m_new_pos = target;
    if (m_changed) chart.notes[*idx].pos = target;
}

void QuantizeNoteCommand::invert(Chart& chart) {
    if (!m_changed) return;  // 未改变 → 无操作
    const auto idx = find_note(chart.notes, m_ref.measure, m_new_pos, m_ref.lane, m_ref.sample);
    if (idx) chart.notes[*idx].pos = m_ref.pos;  // 恢复原 pos
    m_changed = false;
}

std::string QuantizeNoteCommand::describe() const {
    return "量化 note (m" + std::to_string(m_ref.measure) + " @" + std::to_string(m_ref.pos.num) +
           "/" + std::to_string(m_ref.pos.den) + ")";
}

// ---------- TransformNoteCommand ----------

// 变换 lane（见 EditorSession.hpp 声明）：mirror = key i ↔ key (max+1-i)；
// rotate = key 循环移位。键号上限由调用方按谱面模式传入（chart_mode_key_count）——
// pms9k 镜像键1↔键9、7key 镜像键1↔键7；DP 两侧各自变换（player 保留，只动 index）。
// 非 Key lane（scratch/pedal/bgm）不参与（LR2 惯例：镜像/旋转只动按键轨）。
Lane transform_lane(const Lane& lane, bool mirror, int rotate, int max_key) {
    if (lane.kind != LaneKind::Key) return lane;
    int idx = lane.index;
    if (mirror) idx = max_key + 1 - idx;
    if (rotate != 0) {
        // 循环移位：1→2→…→max_key→1（rotate 步）
        idx = ((idx - 1 + rotate) % max_key + max_key) % max_key + 1;
    }
    // 超出 1..max_key 的键号（异常输入）夹回
    if (idx < 1) idx = 1;
    if (idx > max_key) idx = max_key;
    Lane out = lane;
    out.index = static_cast<std::uint8_t>(idx);
    return out;
}

TransformNoteCommand::TransformNoteCommand(NoteRef ref, bool mirror, int rotate)
    : m_ref(ref), m_mirror(mirror), m_rotate(rotate) {}

void TransformNoteCommand::bind_source(const Chart& chart,
                                       std::unordered_set<std::size_t>& claimed) {
    m_index.reset();
    m_new_lane.reset();
    m_source.reset();
    // 与 find_note 一致，只对 measure 二分；同小节 pos 可能无序，必须线性扫描。
    auto it = std::lower_bound(chart.notes.begin(), chart.notes.end(), m_ref.measure,
                               [](const Event<Note>& e, std::uint32_t m) { return e.measure < m; });
    for (; it != chart.notes.end() && it->measure == m_ref.measure; ++it) {
        const auto& e = *it;
        const auto i = static_cast<std::size_t>(it - chart.notes.begin());
        if (e.pos != m_ref.pos || e.value.lane != m_ref.lane || e.value.sample.id != m_ref.sample ||
            e.value.sub_line != m_ref.sub_line || claimed.contains(i))
            continue;
        // Validate while binding, before any lane in a transform batch is written.
        // PMS retains some legacy 2P LN/mine lanes, but not every nine-key target
        // has a BMS representation. Include ln_channel even for orphan LN notes:
        // the writer can retain those using its LN-channel fallback.
        const auto mode = chart.mode_id.value_or("sp7k");
        const auto max_key = chart_mode_key_count(mode);
        if (!max_key) throw std::invalid_argument("Unknown chart mode: " + mode);
        const auto target = transform_lane(m_ref.lane, m_mirror, m_rotate, *max_key);
        if (target != m_ref.lane &&
            !bms::bms_can_represent(mode, target, e.value.ln_channel, e.value.kind)) {
            throw std::invalid_argument(
                "Transform target cannot be represented in this chart mode");
        }
        m_index = i;
        m_source = e.value;
        claimed.insert(i);
        break;
    }
}

void TransformNoteCommand::bind_target(const Chart& chart,
                                       std::unordered_set<std::size_t>& claimed) {
    m_index.reset();
    if (!m_new_lane || !m_source) return;
    auto it = std::lower_bound(chart.notes.begin(), chart.notes.end(), m_ref.measure,
                               [](const Event<Note>& e, std::uint32_t m) { return e.measure < m; });
    for (; it != chart.notes.end() && it->measure == m_ref.measure; ++it) {
        const auto& e = *it;
        const auto i = static_cast<std::size_t>(it - chart.notes.begin());
        // ln_pair 是重建的派生索引，不参与身份；其余物件特征必须完全一致。
        if (e.pos != m_ref.pos || e.value.lane != *m_new_lane ||
            e.value.sample != m_source->sample || e.value.kind != m_source->kind ||
            e.value.ln_channel != m_source->ln_channel || e.value.sub_line != m_source->sub_line ||
            claimed.contains(i))
            continue;
        m_index = i;
        claimed.insert(i);
        break;
    }
}

void TransformNoteCommand::apply(Chart& chart) {
    std::unordered_set<std::size_t> claimed;
    bind_source(chart, claimed);
    apply_bound(chart);
}

void TransformNoteCommand::apply_bound(Chart& chart) {
    if (!m_index) return;
    // 键号上限按谱面模式（apply 时刻的 chart.mode_id；undo 侧恢复原 lane 不重算）。
    // bind_source 已校验模式已知；此处防御性无操作兜底不可达路径。
    const auto max_key = chart_mode_key_count(chart.mode_id.value_or("sp7k"));
    if (!max_key) return;
    m_new_lane = transform_lane(m_ref.lane, m_mirror, m_rotate, *max_key);
    if (*m_new_lane == m_ref.lane) {
        m_new_lane.reset();  // 无变化 → invert 无操作
        return;
    }
    chart.notes[*m_index].value.lane = *m_new_lane;
}

void TransformNoteCommand::invert(Chart& chart) {
    std::unordered_set<std::size_t> claimed;
    bind_target(chart, claimed);
    invert_bound(chart);
}

void TransformNoteCommand::invert_bound(Chart& chart) {
    if (!m_new_lane || !m_index) return;
    chart.notes.at(*m_index).value.lane = m_ref.lane;
    m_new_lane.reset();
    m_index.reset();
    m_source.reset();
}

std::string TransformNoteCommand::describe() const {
    std::string s = "变换 note (m" + std::to_string(m_ref.measure) + " @" +
                    std::to_string(m_ref.pos.num) + "/" + std::to_string(m_ref.pos.den) + ")";
    if (m_mirror) s += " 镜像";
    if (m_rotate != 0) s += " 旋转" + std::to_string(m_rotate);
    return s;
}

// ---------- MetaEditCommand ----------

namespace {

// ASCII 大写（meta 键名规范：TITLE/ARTIST…）
std::string upper_ascii(std::string_view s) {
    std::string out(s);
    for (auto& c : out) {
        if (c >= 'a' && c <= 'z') c = static_cast<char>(c - 'a' + 'A');
    }
    return out;
}

}  // namespace

MetaEditCommand::MetaEditCommand(const std::string& key, std::string value)
    : m_key(upper_ascii(key)), m_value(std::move(value)) {}

void MetaEditCommand::apply(Chart& chart) {
    const auto it = chart.meta.find(m_key);
    m_existed = it != chart.meta.end();
    if (m_existed) m_old_value = it->second;
    m_changed = m_existed ? (it->second != m_value) : !m_value.empty();
    // #BASE：id_base 为结构化状态（parser 不入 meta），修改前先快照供 invert 恢复
    //（2026-09 审查：曾靠 meta["BASE"] 推断 → #BASE 62 谱面改 36 后 undo 错回 Base36）。
    if (m_key == "BASE") m_old_id_base = chart.id_base;
    if (!m_changed) return;
    if (m_value.empty()) {
        chart.meta.erase(m_key);  // 空值 = 删除
    } else {
        chart.meta[m_key] = m_value;
    }
    // 特殊处理 #BASE 字段：同步更新 chart.id_base
    if (m_key == "BASE") {
        if (m_value == "62") {
            chart.id_base = IdBase::Base62;
        } else {
            chart.id_base = IdBase::Base36;  // 默认或其它值都回退到 Base36
        }
    }
}

void MetaEditCommand::invert(Chart& chart) {
    if (!m_changed) return;
    if (m_existed) {
        chart.meta[m_key] = m_old_value;  // 恢复旧值
    } else {
        chart.meta.erase(m_key);  // 原不存在 → 移除
    }
    // 特殊处理 #BASE 字段：恢复 chart.id_base——**快照优先**（invert 时不可依赖 meta["BASE"]，
    // parser 从不把它写入 meta；meta 推断仅作历史兼容兜底）。
    if (m_key == "BASE") {
        if (m_old_id_base) {
            chart.id_base = *m_old_id_base;
            m_old_id_base.reset();
        } else {
            const auto it = chart.meta.find("BASE");
            if (it != chart.meta.end() && it->second == "62") {
                chart.id_base = IdBase::Base62;
            } else {
                chart.id_base = IdBase::Base36;
            }
        }
    }
    m_changed = false;
}

std::string MetaEditCommand::describe() const {
    std::string s = "编辑头部 #" + m_key;
    if (m_value.empty()) s += "（删除）";
    return s;
}

RawLinesEditCommand::RawLinesEditCommand(std::vector<std::string> lines)
    : m_lines(std::move(lines)) {}

void RawLinesEditCommand::apply(Chart& chart) {
    m_old = chart.raw_lines;       // 快照旧原始行
    m_changed = m_old != m_lines;  // 无变化 → invert 无操作
    if (!m_changed) return;
    chart.raw_lines = m_lines;  // 整组替换（格式兜底）
}

void RawLinesEditCommand::invert(Chart& chart) {
    if (!m_changed) return;
    chart.raw_lines = m_old;  // 恢复旧原始行
    m_changed = false;
}

std::string RawLinesEditCommand::describe() const {
    return "编辑扩展代码（原始行 " + std::to_string(m_lines.size()) + " 行）";
}

RenameSampleCommand::RenameSampleCommand(SampleKind kind, std::uint32_t from_id,
                                         std::uint32_t to_id)
    : m_kind(kind), m_from_id(from_id), m_to_id(to_id) {}

// NOLINTNEXTLINE(readability-function-cognitive-complexity)——编辑命令语义分支（配对/地雷/BGM 行）
void RenameSampleCommand::apply(Chart& chart) {
    const auto key_from = std::make_pair(m_kind, m_from_id);
    const auto key_to = std::make_pair(m_kind, m_to_id);
    const auto it_from = chart.samples.find(key_from);
    const auto it_to = chart.samples.find(key_to);
    m_had_from = it_from != chart.samples.end();
    if (m_had_from) m_old_def = it_from->second;
    m_had_to = it_to != chart.samples.end() && it_to != it_from;
    if (m_had_to) m_old_to_def = it_to->second;

    // 收集引用 from_id 的对象下标（按 kind 只用对应容器）
    m_note_idx.clear();
    m_bga_idx.clear();
    m_bpm_idx.clear();
    m_stop_idx.clear();
    switch (m_kind) {
        case SampleKind::Wav:
            for (std::size_t i = 0; i < chart.notes.size(); ++i)
                if (chart.notes[i].value.sample.id == m_from_id) m_note_idx.push_back(i);
            break;
        case SampleKind::Bmp:
            for (std::size_t i = 0; i < chart.bga_events.size(); ++i)
                if (chart.bga_events[i].value.image.id == m_from_id) m_bga_idx.push_back(i);
            break;
        case SampleKind::Bpm:
            for (std::size_t i = 0; i < chart.bpm_events.size(); ++i)
                if (const auto rid = chart.bpm_events[i].value.ref_id; rid && *rid == m_from_id)
                    m_bpm_idx.push_back(i);
            break;
        case SampleKind::Stop:
            for (std::size_t i = 0; i < chart.stop_events.size(); ++i)
                if (const auto rid = chart.stop_events[i].value.ref_id; rid && *rid == m_from_id)
                    m_stop_idx.push_back(i);
            break;
    }
    m_changed = m_had_from || !m_note_idx.empty() || !m_bga_idx.empty() || !m_bpm_idx.empty() ||
                !m_stop_idx.empty();
    if (!m_changed) return;

    // 定义表：移除旧键、设置新键（覆盖碰撞）
    if (m_had_from) chart.samples.erase(key_from);
    if (m_had_from) chart.samples[key_to] = m_old_def;

    // 更新引用
    for (const auto i : m_note_idx) chart.notes[i].value.sample.id = m_to_id;
    for (const auto i : m_bga_idx) chart.bga_events[i].value.image.id = m_to_id;
    for (const auto i : m_bpm_idx) chart.bpm_events[i].value.ref_id = m_to_id;
    for (const auto i : m_stop_idx) chart.stop_events[i].value.ref_id = m_to_id;
}

void RenameSampleCommand::invert(Chart& chart) {
    if (!m_changed) return;
    chart.samples.erase({m_kind, m_to_id});
    if (m_had_from) chart.samples[{m_kind, m_from_id}] = m_old_def;
    if (m_had_to) chart.samples[{m_kind, m_to_id}] = m_old_to_def;
    for (const auto i : m_note_idx) chart.notes[i].value.sample.id = m_from_id;
    for (const auto i : m_bga_idx) chart.bga_events[i].value.image.id = m_from_id;
    for (const auto i : m_bpm_idx) chart.bpm_events[i].value.ref_id = m_from_id;
    for (const auto i : m_stop_idx) chart.stop_events[i].value.ref_id = m_from_id;
    m_changed = false;
}

std::string RenameSampleCommand::describe() const {
    return std::string("重命名定义表 id（") + std::to_string(m_from_id) + " → " +
           std::to_string(m_to_id) + "）";
}

SetSampleFileCommand::SetSampleFileCommand(SampleKind kind, std::uint32_t id, std::string file)
    : m_kind(kind), m_id(id), m_file(std::move(file)) {}

void SetSampleFileCommand::apply(Chart& chart) {
    const auto key = std::make_pair(m_kind, m_id);
    const auto it = chart.samples.find(key);
    m_existed = it != chart.samples.end();
    if (m_existed) m_old_file = it->second.file;
    m_changed = !m_existed || m_old_file != m_file;
    if (!m_changed) return;
    chart.samples[key].file = m_file;  // 不存在则创建
}

void SetSampleFileCommand::invert(Chart& chart) {
    if (!m_changed) return;
    const auto key = std::make_pair(m_kind, m_id);
    if (m_existed) {
        chart.samples[key].file = m_old_file;
    } else {
        chart.samples.erase(key);
    }
    m_changed = false;
}

std::string SetSampleFileCommand::describe() const { return "设置采样文件（#" + m_file + "）"; }

SetSampleValueCommand::SetSampleValueCommand(SampleKind kind, std::uint32_t id, std::string value)
    : m_kind(kind), m_id(id), m_value(std::move(value)) {}

// NOLINTNEXTLINE(readability-function-cognitive-complexity,readability-function-size)——编辑命令语义分支（配对/地雷/BGM 行）
void SetSampleValueCommand::apply(Chart& chart) {
    m_bpm_evs.clear();
    m_stop_evs.clear();
    const auto key = std::make_pair(m_kind, m_id);
    const auto it = chart.samples.find(key);
    m_existed = it != chart.samples.end();
    if (m_existed) m_old_value = it->second.value;
    m_changed = !m_existed || m_old_value != m_value;
    if (!m_changed) return;
    chart.samples[key].value = m_value;  // 不存在则创建
    // 交叉同步（定义 id → 引用者）：所有 ref_id == m_id 的事件同步到新定义值
    //（BMS 语义：一个 #BPMxx/#STOPxx 定义 = 一个值，全部引用者共享）。值 = 定义文本解析。
    double v = 0;
    char* end = nullptr;
    const double parsed = std::strtod(m_value.c_str(), &end);
    const bool parse_ok = end != m_value.c_str() && *end == '\0' && std::isfinite(parsed);
    if (parse_ok) v = parsed;
    if (m_kind == SampleKind::Bpm) {
        for (std::size_t i = 0; i < chart.bpm_events.size(); ++i) {
            auto& ev = chart.bpm_events[i].value;
            if (ev.ref_id && *ev.ref_id == m_id) {
                if (parse_ok) {
                    m_bpm_evs.emplace_back(i, ev.value);
                    ev.value = v;
                }
            }
        }
    } else if (m_kind == SampleKind::Stop) {
        for (std::size_t i = 0; i < chart.stop_events.size(); ++i) {
            auto& ev = chart.stop_events[i].value;
            if (ev.ref_id && *ev.ref_id == m_id) {
                if (parse_ok) {
                    m_stop_evs.emplace_back(i, ev.count);
                    ev.count = static_cast<std::int64_t>(std::llround(v));
                }
            }
        }
    }
}

void SetSampleValueCommand::invert(Chart& chart) {
    if (!m_changed) return;
    const auto key = std::make_pair(m_kind, m_id);
    if (m_existed) {
        chart.samples[key].value = m_old_value;
    } else {
        chart.samples.erase(key);
    }
    // 交叉同步 invert：引用者恢复旧值
    for (const auto& [idx, old] : m_bpm_evs)
        if (idx < chart.bpm_events.size()) chart.bpm_events[idx].value.value = old;
    for (const auto& [idx, old] : m_stop_evs)
        if (idx < chart.stop_events.size()) chart.stop_events[idx].value.count = old;
    m_bpm_evs.clear();
    m_stop_evs.clear();
    m_changed = false;
}

std::string SetSampleValueCommand::describe() const { return "设置定义值（#" + m_value + "）"; }

DeleteSampleCommand::DeleteSampleCommand(SampleKind kind, std::uint32_t id)
    : m_kind(kind), m_id(id) {}

void DeleteSampleCommand::apply(Chart& chart) {
    const auto key = std::make_pair(m_kind, m_id);
    const auto it = chart.samples.find(key);
    m_existed = it != chart.samples.end();
    if (!m_existed) {
        m_changed = false;
        return;
    }
    m_old_def = it->second;
    chart.samples.erase(key);
    m_changed = true;
}

void DeleteSampleCommand::invert(Chart& chart) {
    if (!m_changed) return;
    if (m_existed) chart.samples[{m_kind, m_id}] = m_old_def;
    m_changed = false;
}

std::string DeleteSampleCommand::describe() const {
    return "删除定义表 id（" + std::to_string(m_id) + "）";
}

SetNoteSampleCommand::SetNoteSampleCommand(NoteRef ref, std::uint32_t to) : m_ref(ref), m_to(to) {}

void SetNoteSampleCommand::apply(Chart& chart) {
    const auto idx =
        find_note(chart.notes, m_ref.measure, m_ref.pos, m_ref.lane, m_ref.sample, m_ref.sub_line);
    if (!idx) return;
    m_applied_index = idx;
    m_did_change = chart.notes[*idx].value.sample.id != m_to;
    if (m_did_change) chart.notes[*idx].value.sample.id = m_to;
}

void SetNoteSampleCommand::invert(Chart& chart) {
    if (!m_did_change || !m_applied_index) return;
    chart.notes[*m_applied_index].value.sample.id = m_ref.sample;
    m_did_change = false;
}

std::string SetNoteSampleCommand::describe() const { return "修改 note 引用采样 id"; }

}  // namespace beatbench::edit
