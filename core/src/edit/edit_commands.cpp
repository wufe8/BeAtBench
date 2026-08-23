// SPDX-License-Identifier: GPL-3.0-only
// 编辑命令实现：note.put / note.move / note.delete + EditorSession（undo/redo 栈）。
// 设计（02 §6.1 / 01 §5.6）：命令 = 文档变更唯一操作面，apply/invert 精确互逆，
// merge_with 合并连续操作；Selection/Viewport 不入栈（调用方自持）。
//
// LN 配对一致性策略（下标易错，改为值级重定位）：
//   - ln_pair 存容器下标；任何插入/删除/移动后，受影响端按
//     「伙伴的 (measure,pos,lane,sample) 值」重新定位下标。
//   - 配对关系本身（谁和谁是一对）由命令快照携带：apply 记录被操作 note
//     的完整状态（含 ln_pair 指向的伙伴快照），invert 用快照精确恢复。
#include "beatbench/core/edit/EditorSession.hpp"
#include "beatbench/core/edit/Selection.hpp"

#include <algorithm>
#include <cmath>
#include <stdexcept>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <vector>

namespace beatbench::edit {

// ---------- TimingKind ----------

std::string_view timing_kind_name(TimingKind kind) {
    switch (kind) {
        case TimingKind::Bpm: return "bpm";
        case TimingKind::Stop: return "stop";
        case TimingKind::Measure: return "measure";
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
// pos 升序（doc/04 §6 的「事件按 (measure,pos) 升序」约定未落实；2026-09 实测 Del 失效：
// lower_bound 定位错位导致 1/4 之类 note 永远匹配不上）。改为 measure 段线性扫描
// （行序 measure 单调递增 → 同 measure 连续段；命中率与代价可控）。
// bgm_line：BGM 行序号消歧（同 (measure,pos,lane,sample) 的 Bgm note 靠它区分；
// 非 Bgm 传 0 且不参与匹配）。
std::optional<std::size_t> find_note(const std::vector<Event<Note>>& notes,
                                     std::uint32_t measure, const Rational& pos,
                                     const Lane& lane, std::uint32_t sample,
                                     std::uint32_t bgm_line = 0) {
    auto it = std::lower_bound(notes.begin(), notes.end(), measure,
                               [](const Event<Note>& e, std::uint32_t m) {
                                   return e.measure < m;
                               });
    for (; it != notes.end() && it->measure == measure; ++it) {
        const auto& n = it->value;
        if (it->pos == pos && n.lane == lane && n.sample.id == sample &&
            (lane.kind != LaneKind::Bgm || n.bgm_line == bgm_line))
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

// 值读取（按种类取 value/duration_us/beats）
double timing_value(const Event<Bpm>& e) { return e.value.value; }
double timing_value(const Event<Stop>& e) {
    return static_cast<double>(e.value.duration_us);
}
double timing_value(const Event<MeasureLen>& e) { return e.value.beats; }

// 在按 (measure,pos) 升序的容器中找首个 >= (measure,pos) 的位置（插入点）
template <typename T>
std::size_t lower_bound_event(const std::vector<Event<T>>& evs, std::uint32_t measure,
                              const Rational& pos) {
    const Event<T> probe{measure, pos, {}};
    return static_cast<std::size_t>(
        std::lower_bound(evs.begin(), evs.end(), probe,
                         [](const Event<T>& a, const Event<T>& b) {
                             if (a.measure != b.measure) return a.measure < b.measure;
                             return a.pos < b.pos;
                         }) -
        evs.begin());
}

// 找 (measure, pos) 处事件区间 [begin, end)（容器有序）
template <typename T>
std::pair<std::size_t, std::size_t> find_event_range(const std::vector<Event<T>>& evs,
                                                     std::uint32_t measure,
                                                     const Rational& pos) {
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
        ev.value.duration_us = static_cast<std::int64_t>(v);
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
}

void EditorSession::load(Chart chart, std::string path) {
    load(std::move(chart));
    m_path = std::move(path);
}

bool EditorSession::exec(std::unique_ptr<EditCommand> cmd) {
    if (!m_chart || !cmd) return false;
    Chart before = *m_chart;  // 失败回滚快照（防御性；命令应极少失败）
    try {
        cmd->apply(*m_chart);
    } catch (...) {
        *m_chart = std::move(before);
        return false;
    }
    // 与栈顶合并（连续操作 → 一个 undo 步）
    if (!m_undo.empty() && m_undo.back()->merge_with(*cmd)) {
        m_redo.clear();
        maybe_persist();
        return true;
    }
    m_undo.push_back(std::move(cmd));
    m_redo.clear();
    maybe_persist();
    return true;
}

bool EditorSession::undo() {
    if (m_undo.empty() || !m_chart) return false;
    auto cmd = std::move(m_undo.back());
    m_undo.pop_back();
    cmd->invert(*m_chart);
    m_redo.push_back(std::move(cmd));
    maybe_persist();
    return true;
}

bool EditorSession::redo() {
    if (m_redo.empty() || !m_chart) return false;
    auto cmd = std::move(m_redo.back());
    m_redo.pop_back();
    cmd->apply(*m_chart);
    m_undo.push_back(std::move(cmd));
    maybe_persist();
    return true;
}

void EditorSession::maybe_persist() {
    if (!m_chart || !m_persist_hook || m_path.empty()) return;
    if (m_backup) {
        (void)m_persist_hook(*m_chart, m_path + ".bak");  // 崩溃备份（写失败静默）
    }
    if (m_autosave) {
        (void)m_persist_hook(*m_chart, m_path);  // 自动保存（写失败静默——下次再试）
    }
}

std::string EditorSession::undo_label() const {
    return m_undo.empty() ? std::string() : m_undo.back()->describe();
}

std::string EditorSession::redo_label() const {
    return m_redo.empty() ? std::string() : m_redo.back()->describe();
}

// ---------- PutNoteCommand ----------

PutNoteCommand::PutNoteCommand(std::uint32_t measure, Rational pos, Lane lane,
                               std::uint32_t sample, bool ln_kind, NoteKind kind,
                               std::uint32_t bgm_line)
    : m_measure(measure), m_pos(pos), m_lane(lane), m_sample(sample), m_ln_kind(ln_kind),
      m_kind(kind), m_bgm_line(bgm_line) {}

void PutNoteCommand::apply(Chart& chart) {
    const std::size_t at = lower_bound_pos(chart.notes, m_measure, m_pos);
    Event<Note> ev;
    ev.measure = m_measure;
    ev.pos = m_pos;
    ev.value.lane = m_lane;
    ev.value.sample.id = m_sample;
    ev.value.kind = m_kind;
    ev.value.bgm_line = (m_lane.kind == LaneKind::Bgm) ? m_bgm_line : 0;
    chart.notes.insert(chart.notes.begin() + static_cast<std::ptrdiff_t>(at), ev);
    m_applied_index = at;
    m_paired_head.reset();
    // 先修下标（插入点后所有 ln_pair +1），再配对（用新下标）
    shift_pairs_after(chart.notes, at, +1);
    // LN 放置（2026-09 交互最终确认）：向前找**最近一个**未配对同 lane 同 sample 的
    // Normal note 配成尾；**忽略中间其它通道/sample 的 note**（用户问题2：同轨道往后
    // 放置应配尾，即使中间隔了别的轨道 note）。但若向前遇到同 lane 同 sample 却是
    // **已配对**或**地雷**，则停止 —— 说明该通道已成型 LN 或混杂物件，不重复配。
    // 注意：普通 note 与 LN 头在 model 均 kind=Normal 无 ln_pair，无法区分；
    // 此处按「未配对 Normal」近似，用户用 LN 工具放置序列天然满足。
    if (m_ln_kind && m_kind == NoteKind::Normal) {
        std::optional<std::size_t> head;
        for (std::size_t i = at; i-- > 0;) {
            const auto& n = chart.notes[i].value;
            if (n.lane != m_lane || n.sample.id != m_sample) continue;  // 忽略其它通道/sample
            // 同 lane 同 sample 出现：未配对 Normal → 候选头；否则（已配对/地雷）→ 停止
            if (n.kind == NoteKind::Normal && !n.ln_pair) {
                head = i;
            }
            break;
        }
        if (head) {
            chart.notes[*head].value.ln_pair = at;  // 头 → 尾
            chart.notes[at].value.ln_pair = *head;  // 尾 → 头
            m_paired_head = *head;
        }
    }
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
    std::string s = "放置 note (m" + std::to_string(m_measure) + " @" +
                    std::to_string(m_pos.num) + "/" + std::to_string(m_pos.den) + ")";
    if (m_kind == NoteKind::Landmine) s += " 地雷";
    else if (m_ln_kind) s += " LN";
    return s;
}

// ---------- MoveNoteCommand ----------

MoveNoteCommand::MoveNoteCommand(std::uint32_t from_measure, Rational from_pos, Lane lane,
                                 std::uint32_t sample, std::uint32_t to_measure,
                                 Rational to_pos, bool move_ln_pair,
                                 std::optional<Lane> to_lane,
                                 std::uint32_t from_bgm_line,
                                 std::optional<std::uint32_t> to_bgm_line)
    : m_from_measure(from_measure), m_from_pos(from_pos), m_lane(lane), m_sample(sample),
      m_to_measure(to_measure), m_to_pos(to_pos), m_move_ln_pair(move_ln_pair),
      m_to_lane(to_lane), m_from_bgm_line(from_bgm_line), m_to_bgm_line(to_bgm_line) {}

void MoveNoteCommand::apply(Chart& chart) {
    const auto idx = find_note(chart.notes, m_from_measure, m_from_pos, m_lane, m_sample,
                               m_from_bgm_line);
    if (!idx) return;  // 找不到 → 无操作
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
        partner_val = std::make_tuple(p.measure, p.pos, p.value.lane, p.value.sample.id,
                                      p.value.bgm_line);
    }

    // 从容器取走主 note（只这一个；伙伴不动）
    Event<Note> main_ev = std::move(chart.notes[*idx]);
    const std::size_t removed_at = *idx;
    chart.notes.erase(chart.notes.begin() + static_cast<std::ptrdiff_t>(removed_at));
    // 其余配对端下标修正（删除点之后的 -1；主 note 自身 ln_pair 稍后按值重设）
    for (std::size_t i = 0; i < chart.notes.size(); ++i) {
        auto& p = chart.notes[i].value.ln_pair;
        if (!p) continue;
        if (*p > removed_at) --*p;
        if (*p == removed_at) p.reset();  // 指向被删 note 的伙伴（partner_val 场景重设；无伙伴=无关）
    }

    // 主 note 落位：目标 (measure,pos) + 可选换轨 + BGM 行号更新
    main_ev.measure = m_to_measure;
    main_ev.pos = m_to_pos;
    if (m_to_lane) main_ev.value.lane = *m_to_lane;  // 跨通道：改到目标轨道
    if (main_ev.value.lane.kind == LaneKind::Bgm) {
        if (m_to_bgm_line) {
            main_ev.value.bgm_line = *m_to_bgm_line;
        } else {
            // 自动分配（2026-09 用户反馈问题1）：目标小节 ch01 行号 = FIFO 虚拟子通道。
            // 优先填目标小节中该 (pos,sample) 未占用的最小行号；否则追加行尾（max+1）。
            std::uint32_t used = 0;
            std::uint32_t max_line = 0;
            for (const auto& n : chart.notes) {
                if (n.measure != m_to_measure || n.value.lane.kind != LaneKind::Bgm) continue;
                max_line = std::max(max_line, n.value.bgm_line + 1);
                if (n.value.bgm_line < 32 && n.pos == m_to_pos &&
                    n.value.sample.id == m_sample)
                    used |= (1u << n.value.bgm_line);
            }
            std::uint32_t line = 0;
            while (line < 32 && (used & (1u << line))) ++line;
            main_ev.value.bgm_line = (line < 32) ? line : max_line;
        }
    } else {
        main_ev.value.bgm_line = 0;
    }
    main_ev.value.ln_pair.reset();  // 稍后按伙伴值重设
    const std::size_t main_at = lower_bound_pos(chart.notes, m_to_measure, m_to_pos);
    chart.notes.insert(chart.notes.begin() + static_cast<std::ptrdiff_t>(main_at),
                       std::move(main_ev));

    // 主 note 插入后：其余配对端下标修正（插入点后 +1）
    for (std::size_t i = 0; i < chart.notes.size(); ++i) {
        auto& p = chart.notes[i].value.ln_pair;
        if (!p) continue;
        if (i == main_at) continue;  // 主 note 自身稍后重设
        if (main_at <= i && *p >= main_at) ++*p;
    }

    // 按伙伴值重定位互指（伙伴原地不动；找不到（被删/异常）→ 主 note 单点，伙伴 ln_pair 已清）
    if (partner_val) {
        const auto& [pm, pp, pl, ps, pbl] = *partner_val;
        const auto partner_idx =
            find_note(chart.notes, pm, pp, pl, ps, pbl);
        if (partner_idx && *partner_idx != main_at && main_at < chart.notes.size()) {
            chart.notes[main_at].value.ln_pair =
                static_cast<std::uint32_t>(*partner_idx);
            chart.notes[*partner_idx].value.ln_pair =
                static_cast<std::uint32_t>(main_at);
        }
    }
    m_last_delta = (m_to_pos - m_from_pos);
}

void MoveNoteCommand::invert(Chart& chart) {
    if (!m_moved) return;  // apply 未执行（找不到 note）→ 无操作
    // 反向：把 (m_to_measure, m_to_pos, 当前lane, sample) 移回 (m_from_measure, m_from_pos)
    // 当前 lane：跨通道后 note 在 to_lane；纯时间移动 = m_lane。
    const Lane cur_lane = m_to_lane.value_or(m_lane);
    // 当前 BGM 行：apply 后的行号 = 显式 to_bgm_line 或 apply 自动分配。invert 时无法
    // 精确重建自动分配值 → 用「目标小节内同 (pos,lane,sample) 的 Bgm note」消歧：
    // 遍历该 measure 的 Bgm note 找匹配的 bgm_line（通常唯一）。
    std::uint32_t cur_bgm_line = 0;
    if (cur_lane.kind == LaneKind::Bgm) {
        if (m_to_bgm_line) {
            cur_bgm_line = *m_to_bgm_line;
        } else {
            for (const auto& n : chart.notes) {
                if (n.measure == m_to_measure && n.value.lane == cur_lane &&
                    n.value.sample.id == m_sample && n.pos == m_to_pos) {
                    cur_bgm_line = n.value.bgm_line;
                    break;
                }
            }
        }
    }
    const auto idx = find_note(chart.notes, m_to_measure, m_to_pos, cur_lane, m_sample,
                               cur_bgm_line);
    if (!idx) return;

    // LN 语义（与 apply 对称）：只移回选中 note，保持与伙伴互指（按伙伴值重定位）。
    // 伙伴原地不动（apply 期间伙伴从未移动）。
    std::optional<std::tuple<std::uint32_t, Rational, Lane, std::uint32_t, std::uint32_t>>
        partner_val;
    if (chart.notes[*idx].value.ln_pair &&
        *chart.notes[*idx].value.ln_pair < chart.notes.size()) {
        const auto& p = chart.notes[*chart.notes[*idx].value.ln_pair];
        partner_val = std::make_tuple(p.measure, p.pos, p.value.lane, p.value.sample.id,
                                      p.value.bgm_line);
    }

    Event<Note> main_ev = std::move(chart.notes[*idx]);
    const std::size_t removed_at = *idx;
    chart.notes.erase(chart.notes.begin() + static_cast<std::ptrdiff_t>(removed_at));
    // 其余配对端下标修正（删除点之后的 -1；主 note 自身 ln_pair 稍后按值重设）
    for (std::size_t i = 0; i < chart.notes.size(); ++i) {
        auto& p = chart.notes[i].value.ln_pair;
        if (!p) continue;
        if (*p > removed_at) --*p;
        if (*p == removed_at) p.reset();
    }

    // 主 note 移回源位置 + 恢复源 lane + 源 BGM 行
    main_ev.measure = m_from_measure;
    main_ev.pos = m_from_pos;
    if (m_to_lane) main_ev.value.lane = m_lane;
    main_ev.value.bgm_line =
        (main_ev.value.lane.kind == LaneKind::Bgm) ? m_from_bgm_line : 0;
    main_ev.value.ln_pair.reset();
    const std::size_t main_at = lower_bound_pos(chart.notes, m_from_measure, m_from_pos);
    chart.notes.insert(chart.notes.begin() + static_cast<std::ptrdiff_t>(main_at),
                       std::move(main_ev));

    // 插入后其余配对端下标修正
    for (std::size_t i = 0; i < chart.notes.size(); ++i) {
        auto& p = chart.notes[i].value.ln_pair;
        if (!p) continue;
        if (i == main_at) continue;
        if (main_at <= i && *p >= main_at) ++*p;
    }

    // 按伙伴值重定位互指（伙伴原地未动）
    if (partner_val) {
        const auto& [pm, pp, pl, ps, pbl] = *partner_val;
        const auto partner_idx = find_note(chart.notes, pm, pp, pl, ps, pbl);
        if (partner_idx && *partner_idx != main_at && main_at < chart.notes.size()) {
            chart.notes[main_at].value.ln_pair =
                static_cast<std::uint32_t>(*partner_idx);
            chart.notes[*partner_idx].value.ln_pair =
                static_cast<std::uint32_t>(main_at);
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
    if (mv->m_from_measure == m_to_measure && mv->m_from_pos == m_to_pos &&
        mv->m_lane == m_lane && mv->m_sample == m_sample && mv->m_move_ln_pair == m_move_ln_pair &&
        mv->m_to_lane == m_to_lane) {
        m_to_measure = mv->m_to_measure;
        m_to_pos = mv->m_to_pos;
        m_to_lane = mv->m_to_lane;
        m_to_bgm_line = mv->m_to_bgm_line;
        return true;
    }
    return false;
}

std::string MoveNoteCommand::describe() const {
    std::string s = "移动 note (m" + std::to_string(m_from_measure) + " → m" +
                    std::to_string(m_to_measure) + ")";
    if (m_to_lane) s += "（换轨）";
    return s;
}

// ---------- ToggleLnCommand（单点 ↔ LN） ----------

ToggleLnCommand::ToggleLnCommand(std::uint32_t measure, Rational pos, Lane lane,
                                 std::uint32_t sample, std::uint32_t bgm_line)
    : m_measure(measure), m_pos(pos), m_lane(lane), m_sample(sample), m_bgm_line(bgm_line) {}

void ToggleLnCommand::apply(Chart& chart) {
    const auto idx = find_note(chart.notes, m_measure, m_pos, m_lane, m_sample, m_bgm_line);
    if (!idx) return;
    m_did_change = false;
    m_was_ln = false;
    m_applied_partner.reset();
    m_old_partner.reset();

    const auto& note = chart.notes[*idx].value;
    if (note.ln_pair && *note.ln_pair < chart.notes.size()) {
        // LN → 单点：断开（本端 + 伙伴端都清 ln_pair）
        const auto p = *note.ln_pair;
        m_was_ln = true;
        m_old_partner = chart.notes[p];  // 伙伴快照（invert 恢复）
        chart.notes[*idx].value.ln_pair.reset();
        chart.notes[p].value.ln_pair.reset();
        m_did_change = true;
        return;
    }
    // 单点 → LN：向前找最近同 lane 同 sample 未配对单点（忽略中间其它通道）
    std::optional<std::size_t> head;
    for (std::size_t i = *idx; i-- > 0;) {
        const auto& n = chart.notes[i].value;
        if (n.lane != m_lane || n.sample.id != m_sample) continue;
        // 同 lane 同 sample：未配对 Normal → 候选；已配对/地雷 → 停止（不重复配）
        if (n.kind == NoteKind::Normal && !n.ln_pair) {
            head = i;
        }
        break;
    }
    if (head) {
        chart.notes[*head].value.ln_pair = static_cast<std::uint32_t>(*idx);
        chart.notes[*idx].value.ln_pair = static_cast<std::uint32_t>(*head);
        m_applied_partner = static_cast<std::uint32_t>(*head);
        m_did_change = true;
    }
}

void ToggleLnCommand::invert(Chart& chart) {
    if (!m_did_change) return;
    const auto idx = find_note(chart.notes, m_measure, m_pos, m_lane, m_sample, m_bgm_line);
    if (!idx) return;
    if (m_was_ln) {
        // 恢复：本端与伙伴（按快照值）互指
        if (m_old_partner) {
            const auto pp = find_note(chart.notes, m_old_partner->measure,
                                      m_old_partner->pos, m_old_partner->value.lane,
                                      m_old_partner->value.sample.id,
                                      m_old_partner->value.bgm_line);
            if (pp && *pp != *idx) {
                chart.notes[*idx].value.ln_pair = static_cast<std::uint32_t>(*pp);
                chart.notes[*pp].value.ln_pair = static_cast<std::uint32_t>(*idx);
            }
        }
    } else if (m_applied_partner) {
        // 撤销配对：清除互指
        if (*m_applied_partner < chart.notes.size()) {
            chart.notes[*idx].value.ln_pair.reset();
            chart.notes[*m_applied_partner].value.ln_pair.reset();
        }
    }
    m_did_change = false;
    m_was_ln = false;
    m_applied_partner.reset();
    m_old_partner.reset();
}

std::string ToggleLnCommand::describe() const {
    return "单点/LN 转换 (m" + std::to_string(m_measure) + ")";
}

// ---------- ConvertNoteCommand（note ↔ BGA/BPM/STOP 跨命名空间转换） ----------

ConvertNoteCommand::ConvertNoteCommand(std::uint32_t measure, Rational pos, Lane lane,
                                       std::uint32_t sample, std::uint32_t bgm_line,
                                       ConvertTarget target, std::uint32_t to_measure,
                                       Rational to_pos)
    : m_measure(measure), m_pos(pos), m_lane(lane), m_sample(sample), m_bgm_line(bgm_line),
      m_target(target), m_to_measure(to_measure), m_to_pos(to_pos) {}

void ConvertNoteCommand::apply(Chart& chart) {
    const auto idx = find_note(chart.notes, m_measure, m_pos, m_lane, m_sample, m_bgm_line);
    if (!idx) return;  // 找不到 → 无操作
    m_removed = chart.notes[*idx];
    m_partner.reset();
    // 移除 note（LN 配对若存在：断开伙伴 ln_pair，快照保存供 invert 恢复）
    if (const auto p = m_removed->value.ln_pair) {
        if (*p < chart.notes.size() && chart.notes[*p].value.ln_pair &&
            *chart.notes[*p].value.ln_pair == *idx) {
            m_partner = chart.notes[*p];
            chart.notes[*p].value.ln_pair.reset();
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
            bga.image.id = m_sample;  // id 不变：note 的 #WAVxx → #BMPxx（同文本 id）
            bga.layer = layer;
            chart.bga_events.push_back({m_to_measure, m_to_pos, bga});
            m_insert_index = chart.bga_events.size() - 1;
            m_bga_layer = layer;
            break;
        }
        case ConvertTarget::Bpm: {
            // id → #BPMxx 引用：ref_id = m_sample；value 由定义表解析（缺省 130）
            double v = 130.0;
            if (const auto it = chart.samples.find({SampleKind::Bpm, m_sample});
                it != chart.samples.end()) {
                char* end = nullptr;
                const double d = std::strtod(it->second.value.c_str(), &end);
                if (end != it->second.value.c_str() && *end == '\0') v = d;
            }
            Bpm bpm;
            bpm.value = v;
            bpm.ref_id = m_sample;
            chart.bpm_events.push_back({m_to_measure, m_to_pos, bpm});
            m_insert_index = chart.bpm_events.size() - 1;
            m_ref_id = m_sample;
            m_value = v;
            break;
        }
        case ConvertTarget::Stop: {
            // id → #STOPxx 引用：ref_id = m_sample；value 由定义表解析（缺省 0）
            std::int64_t us = 0;
            if (const auto it = chart.samples.find({SampleKind::Stop, m_sample});
                it != chart.samples.end()) {
                char* end = nullptr;
                const double d = std::strtod(it->second.value.c_str(), &end);
                if (end != it->second.value.c_str() && *end == '\0')
                    us = static_cast<std::int64_t>(d * 1000000.0 / 192.0 + 0.5);
            }
            Stop stop;
            stop.duration_us = us;
            stop.ref_id = m_sample;
            chart.stop_events.push_back({m_to_measure, m_to_pos, stop});
            m_insert_index = chart.stop_events.size() - 1;
            m_ref_id = m_sample;
            m_value = static_cast<double>(us);
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
    const std::size_t at = lower_bound_pos(chart.notes, m_measure, m_pos);
    Event<Note> ev = *m_removed;
    ev.value.ln_pair.reset();
    chart.notes.insert(chart.notes.begin() + static_cast<std::ptrdiff_t>(at), ev);
    shift_pairs_after(chart.notes, at, +1);
    if (m_partner) {
        // 伙伴按快照值重定位（伙伴未移动；快照 (measure,pos,lane,sample,bgm_line) 可定位）
        const auto pp = find_note(chart.notes, m_partner->measure, m_partner->pos,
                                  m_partner->value.lane, m_partner->value.sample.id,
                                  m_partner->value.bgm_line);
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
    std::string s = "转换 note (m" + std::to_string(m_measure) + " @" +
                    std::to_string(m_pos.num) + "/" + std::to_string(m_pos.den) + ")";
    switch (m_target) {
        case ConvertTarget::BgaBase: s += " → BGA"; break;
        case ConvertTarget::BgaPoor: s += " → POOR"; break;
        case ConvertTarget::BgaLayer: s += " → LAYER"; break;
        case ConvertTarget::BgaLayer2: s += " → LAYER2"; break;
        case ConvertTarget::Bpm: s += " → BPM"; break;
        case ConvertTarget::Stop: s += " → STOP"; break;
    }
    return s;
}

// ---------- DeleteNoteCommand ----------

DeleteNoteCommand::DeleteNoteCommand(std::uint32_t measure, Rational pos, Lane lane,
                                     std::uint32_t sample, std::uint32_t bgm_line)
    : m_measure(measure), m_pos(pos), m_lane(lane), m_sample(sample), m_bgm_line(bgm_line) {}

void DeleteNoteCommand::apply(Chart& chart) {
    const auto idx = find_note(chart.notes, m_measure, m_pos, m_lane, m_sample, m_bgm_line);
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
    return "删除 note (m" + std::to_string(m_measure) + " @" + std::to_string(m_pos.num) +
           "/" + std::to_string(m_pos.den) + ")";
}

// ---------- CompositeCommand ----------

void CompositeCommand::add(std::unique_ptr<EditCommand> cmd) {
    if (cmd) m_commands.push_back(std::move(cmd));
}

std::string CompositeCommand::name() const {
    if (m_commands.empty()) return "composite";
    return "composite." + m_commands.front()->name();
}

void CompositeCommand::apply(Chart& chart) {
    for (auto& c : m_commands) c->apply(chart);
}

void CompositeCommand::invert(Chart& chart) {
    // 逆序反转
    for (auto it = m_commands.rbegin(); it != m_commands.rend(); ++it) {
        (*it)->invert(chart);
    }
}

std::string CompositeCommand::describe() const {
    if (m_commands.empty()) return "批量操作";
    if (m_commands.size() == 1) return m_commands.front()->describe();
    return "批量操作（" + std::to_string(m_commands.size()) + " 项）";
}

// ---------- Selection ----------

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
                                   double value)
    : m_kind(kind), m_measure(measure), m_pos(pos), m_value(value) {
    if (m_kind == TimingKind::Measure) m_pos = Rational(0, 1);  // 节拍恒 pos 0
}

void PutTimingCommand::apply(Chart& chart) {
    m_existed = false;
    m_applied_index.reset();
    const auto do_apply = [&](auto& evs) {
        using Ev = typename std::remove_reference_t<decltype(evs)>::value_type;
        const auto [lo, hi] = find_event_range(evs, m_measure, m_pos);
        if (lo != hi) {  // 同位替换（同 pos 多值：改最后一个 = 引擎「后者覆盖」语义）
            m_existed = true;
            m_old_value = timing_value(evs[hi - 1]);
            set_timing_value(evs[hi - 1], m_value);
            return;
        }
        Ev ev;
        ev.measure = m_measure;
        ev.pos = m_pos;
        set_timing_value(ev, m_value);
        evs.insert(evs.begin() + static_cast<std::ptrdiff_t>(lo), std::move(ev));
        m_applied_index = lo;
    };
    switch (m_kind) {
        case TimingKind::Bpm: do_apply(chart.bpm_events); break;
        case TimingKind::Stop: do_apply(chart.stop_events); break;
        case TimingKind::Measure: do_apply(chart.measure_events); break;
    }
}

void PutTimingCommand::invert(Chart& chart) {
    const auto do_invert = [&](auto& evs) {
        if (m_existed) {
            const auto [lo, hi] = find_event_range(evs, m_measure, m_pos);
            if (lo != hi) {
                set_timing_value(evs[hi - 1], m_old_value);  // 恢复旧值
            }
            return;
        }
        if (m_applied_index && *m_applied_index < evs.size()) {
            evs.erase(evs.begin() + static_cast<std::ptrdiff_t>(*m_applied_index));
        }
        m_applied_index.reset();
    };
    switch (m_kind) {
        case TimingKind::Bpm: do_invert(chart.bpm_events); break;
        case TimingKind::Stop: do_invert(chart.stop_events); break;
        case TimingKind::Measure: do_invert(chart.measure_events); break;
    }
}

std::string PutTimingCommand::describe() const {
    std::string name(timing_kind_name(m_kind));
    return "设置 " + name + " (m" + std::to_string(m_measure) + " @" +
           std::to_string(m_pos.num) + "/" + std::to_string(m_pos.den) + ")";
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
        case TimingKind::Bpm: do_apply(chart.bpm_events, m_bpm); break;
        case TimingKind::Stop: do_apply(chart.stop_events, m_stop); break;
        case TimingKind::Measure: do_apply(chart.measure_events, m_measure_evs); break;
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
        case TimingKind::Bpm: do_invert(chart.bpm_events, m_bpm); break;
        case TimingKind::Stop: do_invert(chart.stop_events, m_stop); break;
        case TimingKind::Measure: do_invert(chart.measure_events, m_measure_evs); break;
    }
    m_bpm.clear();
    m_stop.clear();
    m_measure_evs.clear();
    m_index = 0;
}

std::string DeleteTimingCommand::describe() const {
    std::string name(timing_kind_name(m_kind));
    return "删除 " + name + " (m" + std::to_string(m_measure) + " @" +
           std::to_string(m_pos.num) + "/" + std::to_string(m_pos.den) + ")";
}

// ---------- QuantizeNoteCommand ----------

QuantizeNoteCommand::QuantizeNoteCommand(std::uint32_t measure, Rational pos, Lane lane,
                                         std::uint32_t sample, std::int64_t snap_num,
                                         std::int64_t snap_den)
    : m_measure(measure), m_pos(pos), m_lane(lane), m_sample(sample), m_snap_num(snap_num),
      m_snap_den(snap_den) {
    if (m_snap_num <= 0) m_snap_num = 1;
    if (m_snap_den <= 0) m_snap_den = 1;
}

void QuantizeNoteCommand::apply(Chart& chart) {
    const auto idx = find_note(chart.notes, m_measure, m_pos, m_lane, m_sample);
    if (!idx) return;  // 找不到 → 无操作
    // 量化：k = round(pos * snapDen / snapNum)，目标 pos = k*snapNum/snapDen
    // （Rational 构造自动约分，如 2/4 → 1/2；网格上 pos 本身不变）
    const double k = std::round(static_cast<double>(m_pos.num) * m_snap_den /
                                (static_cast<double>(m_pos.den) * m_snap_num));
    const Rational target(static_cast<std::int64_t>(k) * m_snap_num, m_snap_den);
    m_changed = (target != m_pos);
    m_new_pos = target;
    if (m_changed) chart.notes[*idx].pos = target;
}

void QuantizeNoteCommand::invert(Chart& chart) {
    if (!m_changed) return;  // 未改变 → 无操作
    const auto idx = find_note(chart.notes, m_measure, m_new_pos, m_lane, m_sample);
    if (idx) chart.notes[*idx].pos = m_pos;  // 恢复原 pos
    m_changed = false;
}

std::string QuantizeNoteCommand::describe() const {
    return "量化 note (m" + std::to_string(m_measure) + " @" +
           std::to_string(m_pos.num) + "/" + std::to_string(m_pos.den) + ")";
}

// ---------- TransformNoteCommand ----------

namespace {

// 变换 lane：mirror = key i ↔ key (max-i+1)；rotate = key 循环移位。返回变换后 lane。
// 非 Key lane（scratch/pedal/bgm）不参与（LR2 惯例：镜像/旋转只动按键轨）。
Lane transform_lane(const Lane& lane, bool mirror, int rotate) {
    if (lane.kind != LaneKind::Key) return lane;
    // 键号上限：按模式？——命令不感知模式（模式是 Chart.mode_id 的呈现层概念）。
    // 这里用保守的 1..7（sp7k/dp 的键号区间；pms9k 键 1..9 超出部分不参与）。
    // 镜像：i → 8-i（1↔7, 2↔6, 3↔5, 4↔4）
    constexpr int kMaxKey = 7;
    int idx = lane.index;
    if (mirror) idx = kMaxKey + 1 - idx;
    if (rotate != 0) {
        // 循环移位：1→2→3→…→7→1（rotate 步）
        idx = ((idx - 1 + rotate) % kMaxKey + kMaxKey) % kMaxKey + 1;
    }
    // 超出 1..7 的键号（pms9k 的 8/9）镜像后回落到 0/负 → 夹回
    if (idx < 1) idx = 1;
    if (idx > kMaxKey) idx = kMaxKey;
    Lane out = lane;
    out.index = static_cast<std::uint8_t>(idx);
    return out;
}

}  // namespace

TransformNoteCommand::TransformNoteCommand(std::uint32_t measure, Rational pos, Lane lane,
                                           std::uint32_t sample, bool mirror, int rotate)
    : m_measure(measure), m_pos(pos), m_lane(lane), m_sample(sample), m_mirror(mirror),
      m_rotate(rotate) {}

void TransformNoteCommand::apply(Chart& chart) {
    const auto idx = find_note(chart.notes, m_measure, m_pos, m_lane, m_sample);
    if (!idx) return;
    m_new_lane = transform_lane(m_lane, m_mirror, m_rotate);
    if (*m_new_lane == m_lane) {
        m_new_lane.reset();  // 无变化 → invert 无操作
        return;
    }
    chart.notes[*idx].value.lane = *m_new_lane;
}

void TransformNoteCommand::invert(Chart& chart) {
    if (!m_new_lane) return;
    // 恢复原 lane（用新 lane 定位——apply 后 note 在 m_new_lane）
    const auto idx = find_note(chart.notes, m_measure, m_pos, *m_new_lane, m_sample);
    if (idx) chart.notes[*idx].value.lane = m_lane;
    m_new_lane.reset();
}

std::string TransformNoteCommand::describe() const {
    std::string s = "变换 note (m" + std::to_string(m_measure) + " @" +
                    std::to_string(m_pos.num) + "/" + std::to_string(m_pos.den) + ")";
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

MetaEditCommand::MetaEditCommand(std::string key, std::string value)
    : m_key(upper_ascii(key)), m_value(std::move(value)) {}

void MetaEditCommand::apply(Chart& chart) {
    const auto it = chart.meta.find(m_key);
    m_existed = it != chart.meta.end();
    if (m_existed) m_old_value = it->second;
    m_changed = m_existed ? (it->second != m_value) : !m_value.empty();
    if (!m_changed) return;
    if (m_value.empty()) {
        chart.meta.erase(m_key);  // 空值 = 删除
    } else {
        chart.meta[m_key] = m_value;
    }
}

void MetaEditCommand::invert(Chart& chart) {
    if (!m_changed) return;
    if (m_existed) {
        chart.meta[m_key] = m_old_value;  // 恢复旧值
    } else {
        chart.meta.erase(m_key);  // 原不存在 → 移除
    }
    m_changed = false;
}

std::string MetaEditCommand::describe() const {
    std::string s = "编辑头部 #" + m_key;
    if (m_value.empty()) s += "（删除）";
    return s;
}

}  // namespace beatbench::edit
