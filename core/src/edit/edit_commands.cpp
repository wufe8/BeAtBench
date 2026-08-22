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
std::optional<std::size_t> find_note(const std::vector<Event<Note>>& notes,
                                     std::uint32_t measure, const Rational& pos,
                                     const Lane& lane, std::uint32_t sample) {
    auto it = std::lower_bound(notes.begin(), notes.end(), measure,
                               [](const Event<Note>& e, std::uint32_t m) {
                                   return e.measure < m;
                               });
    for (; it != notes.end() && it->measure == measure; ++it) {
        const auto& n = it->value;
        if (it->pos == pos && n.lane == lane && n.sample.id == sample)
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
        return true;
    }
    m_undo.push_back(std::move(cmd));
    m_redo.clear();
    return true;
}

bool EditorSession::undo() {
    if (m_undo.empty() || !m_chart) return false;
    auto cmd = std::move(m_undo.back());
    m_undo.pop_back();
    cmd->invert(*m_chart);
    m_redo.push_back(std::move(cmd));
    return true;
}

bool EditorSession::redo() {
    if (m_redo.empty() || !m_chart) return false;
    auto cmd = std::move(m_redo.back());
    m_redo.pop_back();
    cmd->apply(*m_chart);
    m_undo.push_back(std::move(cmd));
    return true;
}

std::string EditorSession::undo_label() const {
    return m_undo.empty() ? std::string() : m_undo.back()->describe();
}

std::string EditorSession::redo_label() const {
    return m_redo.empty() ? std::string() : m_redo.back()->describe();
}

// ---------- PutNoteCommand ----------

PutNoteCommand::PutNoteCommand(std::uint32_t measure, Rational pos, Lane lane,
                               std::uint32_t sample)
    : m_measure(measure), m_pos(pos), m_lane(lane), m_sample(sample) {}

void PutNoteCommand::apply(Chart& chart) {
    const std::size_t at = lower_bound_pos(chart.notes, m_measure, m_pos);
    Event<Note> ev;
    ev.measure = m_measure;
    ev.pos = m_pos;
    ev.value.lane = m_lane;
    ev.value.sample.id = m_sample;
    chart.notes.insert(chart.notes.begin() + static_cast<std::ptrdiff_t>(at), ev);
    m_applied_index = at;
    shift_pairs_after(chart.notes, at, +1);
}

void PutNoteCommand::invert(Chart& chart) {
    if (!m_applied_index || *m_applied_index >= chart.notes.size()) return;
    const std::size_t at = *m_applied_index;
    shift_pairs_after(chart.notes, at, -1);
    chart.notes.erase(chart.notes.begin() + static_cast<std::ptrdiff_t>(at));
    m_applied_index.reset();
}

std::string PutNoteCommand::describe() const {
    return "放置 note (m" + std::to_string(m_measure) + " @" + std::to_string(m_pos.num) +
           "/" + std::to_string(m_pos.den) + ")";
}

// ---------- MoveNoteCommand ----------

MoveNoteCommand::MoveNoteCommand(std::uint32_t from_measure, Rational from_pos, Lane lane,
                                 std::uint32_t sample, std::uint32_t to_measure,
                                 Rational to_pos, bool move_ln_pair)
    : m_from_measure(from_measure), m_from_pos(from_pos), m_lane(lane), m_sample(sample),
      m_to_measure(to_measure), m_to_pos(to_pos), m_move_ln_pair(move_ln_pair) {}

void MoveNoteCommand::apply(Chart& chart) {
    const auto idx = find_note(chart.notes, m_from_measure, m_from_pos, m_lane, m_sample);
    if (!idx) return;  // 找不到 → 无操作
    m_moved = chart.notes[*idx];  // 快照（invert 恢复）
    m_partner.reset();

    // 配对端：成对模式 → 随动；单 note 模式 → 只记录快照（invert 恢复），但实际不动伙伴
    std::optional<std::size_t> partner_idx;
    if (const auto p = chart.notes[*idx].value.ln_pair) {
        if (*p < chart.notes.size() && chart.notes[*p].value.ln_pair &&
            *chart.notes[*p].value.ln_pair == *idx) {
            m_partner = chart.notes[*p];  // 快照（两种模式都记，invert 恢复用）
            if (m_move_ln_pair) {
                partner_idx = *p;
            } else {
                // 单 note 模式：解除配对（伙伴 ln_pair 清空，防悬挂）
                chart.notes[*p].value.ln_pair.reset();
            }
        }
    }

    // 位移 delta = 目标 - 源
    const Rational delta = m_to_pos - m_from_pos;

    // 快照式重排：先取走主 note（与配对端，若成对），从容器删除
    std::vector<Event<Note>> moved;
    moved.push_back(std::move(chart.notes[*idx]));
    if (partner_idx) moved.push_back(std::move(chart.notes[*partner_idx]));

    std::vector<std::size_t> del = {*idx};
    if (partner_idx) del.push_back(*partner_idx);
    std::sort(del.begin(), del.end(), std::greater<std::size_t>());
    for (const auto d : del) {
        chart.notes.erase(chart.notes.begin() + static_cast<std::ptrdiff_t>(d));
    }
    // 其余配对端下标修正（删除点之前的数量）
    for (std::size_t i = 0; i < chart.notes.size(); ++i) {
        auto& p = chart.notes[i].value.ln_pair;
        if (!p) continue;
        std::size_t shift = 0;
        for (const auto d : del) {
            if (*p > d) ++shift;
        }
        *p = static_cast<std::uint32_t>(static_cast<std::size_t>(*p) - shift);
    }

    // 重新插入：主 note 到目标；配对端（若成对）到 (目标 + delta)
    Event<Note> main_ev = std::move(moved[0]);
    main_ev.measure = m_to_measure;
    main_ev.pos = m_to_pos;
    main_ev.value.ln_pair.reset();  // 单 note：无配对；成对：稍后重设
    const std::size_t main_at = lower_bound_pos(chart.notes, m_to_measure, m_to_pos);
    chart.notes.insert(chart.notes.begin() + static_cast<std::ptrdiff_t>(main_at),
                       std::move(main_ev));

    std::optional<std::size_t> part_at;
    if (moved.size() > 1) {
        Event<Note> part_ev = std::move(moved[1]);
        // 伙伴随主 note 移动：目标 = 主目标 + 伙伴相对主 note 的原始偏移
        part_ev.measure = m_to_measure;
        part_ev.pos = m_to_pos + (m_partner ? m_partner->pos - m_from_pos : Rational(0, 1));
        part_ev.value.ln_pair.reset();
        part_at = lower_bound_pos(chart.notes, part_ev.measure, part_ev.pos);
        chart.notes.insert(chart.notes.begin() + static_cast<std::ptrdiff_t>(*part_at),
                           std::move(part_ev));
    }

    // 其余配对端下标修正（插入点后 +1）
    for (std::size_t i = 0; i < chart.notes.size(); ++i) {
        auto& p = chart.notes[i].value.ln_pair;
        if (!p) continue;
        if (main_at < i && *p >= main_at) ++*p;
        if (part_at && *part_at < i && *p >= *part_at) ++*p;
    }

    // 重设互指（成对模式：主 ↔ 配对端）
    if (part_at) {
        std::size_t a = main_at, b = *part_at;
        if (a == b) b = a + 1;  // 同 pos 冲突（理论不可达）
        if (a < chart.notes.size() && b < chart.notes.size()) {
            chart.notes[a].value.ln_pair = b;
            chart.notes[b].value.ln_pair = a;
        }
    }
    m_last_delta = delta;
}

void MoveNoteCommand::invert(Chart& chart) {
    if (!m_moved) return;  // apply 未执行（找不到 note）→ 无操作
    // 反向：把 (m_to_measure, m_to_pos, lane, sample) 移回 (m_from_measure, m_from_pos)
    const auto idx = find_note(chart.notes, m_to_measure, m_to_pos, m_lane, m_sample);
    if (!idx) return;
    // 配对端：成对模式 → 当前容器中随动伙伴；单 note 模式 → 无随动（伙伴留原位）
    std::optional<std::size_t> partner_idx;
    if (m_move_ln_pair && m_partner) {
        partner_idx = find_partner(chart.notes, chart.notes[*idx], *idx);
    }
    // 快照式重排（对称反向）
    std::vector<Event<Note>> moved;
    moved.push_back(std::move(chart.notes[*idx]));
    if (partner_idx) moved.push_back(std::move(chart.notes[*partner_idx]));
    std::vector<std::size_t> del = {*idx};
    if (partner_idx) del.push_back(*partner_idx);
    std::sort(del.begin(), del.end(), std::greater<std::size_t>());
    for (const auto d : del) {
        chart.notes.erase(chart.notes.begin() + static_cast<std::ptrdiff_t>(d));
    }
    for (std::size_t i = 0; i < chart.notes.size(); ++i) {
        auto& p = chart.notes[i].value.ln_pair;
        if (!p) continue;
        std::size_t shift = 0;
        for (const auto d : del) {
            if (*p > d) ++shift;
        }
        *p = static_cast<std::uint32_t>(static_cast<std::size_t>(*p) - shift);
    }
    Event<Note> main_ev = std::move(moved[0]);
    main_ev.measure = m_from_measure;
    main_ev.pos = m_from_pos;
    main_ev.value.ln_pair.reset();
    const std::size_t main_at = lower_bound_pos(chart.notes, m_from_measure, m_from_pos);
    chart.notes.insert(chart.notes.begin() + static_cast<std::ptrdiff_t>(main_at),
                       std::move(main_ev));
    std::optional<std::size_t> part_at;
    if (moved.size() > 1) {
        Event<Note> part_ev = std::move(moved[1]);
        // 伙伴恢复回原始位置（快照；伙伴相对主 note 的偏移保持）
        part_ev.measure = m_partner ? m_partner->measure : m_from_measure;
        part_ev.pos = m_partner ? m_partner->pos : m_from_pos;
        part_ev.value.ln_pair.reset();
        part_at = lower_bound_pos(chart.notes, part_ev.measure, part_ev.pos);
        chart.notes.insert(chart.notes.begin() + static_cast<std::ptrdiff_t>(*part_at),
                           std::move(part_ev));
    }
    for (std::size_t i = 0; i < chart.notes.size(); ++i) {
        auto& p = chart.notes[i].value.ln_pair;
        if (!p) continue;
        if (main_at < i && *p >= main_at) ++*p;
        if (part_at && *part_at < i && *p >= *part_at) ++*p;
    }
    if (part_at) {
        std::size_t a = main_at, b = *part_at;
        if (a == b) b = a + 1;
        if (a < chart.notes.size() && b < chart.notes.size()) {
            chart.notes[a].value.ln_pair = b;
            chart.notes[b].value.ln_pair = a;
        }
    } else if (m_partner) {
        // 单 note 模式：恢复配对——按伙伴快照 (measure,pos,lane,sample) 找当前下标，
        // 与主 note（已移回 m_from）互指。伙伴从未移动，值快照可直接定位。
        const auto pp = find_note(chart.notes, m_partner->measure, m_partner->pos,
                                  m_partner->value.lane, m_partner->value.sample.id);
        if (pp && *pp != main_at && main_at < chart.notes.size()) {
            chart.notes[main_at].value.ln_pair = *pp;
            chart.notes[*pp].value.ln_pair = main_at;
        }
    }
    m_moved.reset();
    m_partner.reset();
}

bool MoveNoteCommand::merge_with(const EditCommand& next) {
    // 连续移动同一 note：next 的 from == 本命令的 to 且 lane/sample 一致 → 并入
    const auto* mv = dynamic_cast<const MoveNoteCommand*>(&next);
    if (!mv) return false;
    if (mv->m_from_measure == m_to_measure && mv->m_from_pos == m_to_pos &&
        mv->m_lane == m_lane && mv->m_sample == m_sample && mv->m_move_ln_pair == m_move_ln_pair) {
        m_to_measure = mv->m_to_measure;
        m_to_pos = mv->m_to_pos;
        return true;
    }
    return false;
}

std::string MoveNoteCommand::describe() const {
    return "移动 note (m" + std::to_string(m_from_measure) + " → m" +
           std::to_string(m_to_measure) + ")";
}

// ---------- DeleteNoteCommand ----------

DeleteNoteCommand::DeleteNoteCommand(std::uint32_t measure, Rational pos, Lane lane,
                                     std::uint32_t sample)
    : m_measure(measure), m_pos(pos), m_lane(lane), m_sample(sample) {}

void DeleteNoteCommand::apply(Chart& chart) {
    const auto idx = find_note(chart.notes, m_measure, m_pos, m_lane, m_sample);
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

}  // namespace beatbench::edit
