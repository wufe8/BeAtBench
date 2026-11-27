// SPDX-License-Identifier: GPL-3.0-only
// 选中集 → 换轨计划：快速摆音与配置复制共用的纯规划层。
// 只读 Chart，产出 LaneMove；执行由调用方组装 EditCommand，避免两套管线分叉。
#include <algorithm>
#include <optional>
#include <set>
#include <string>
#include <tuple>
#include <vector>

#include "beatbench/core/ChartMode.hpp"
#include "beatbench/core/ColumnKey.hpp"
#include "beatbench/core/edit/EditorSession.hpp"
#include "beatbench/core/edit/LanePlan.hpp"

namespace beatbench::edit {
namespace {

std::optional<std::size_t> find_note_index(const Chart& chart, const NoteRef& ref) {
    const auto begin =
        std::lower_bound(chart.notes.begin(), chart.notes.end(), ref.measure,
                         [](const Event<Note>& e, std::uint32_t m) { return e.measure < m; });
    for (auto it = begin; it != chart.notes.end() && it->measure == ref.measure; ++it) {
        if (it->pos != ref.pos || it->value.lane != ref.lane || it->value.sample.id != ref.sample ||
            it->value.sub_line != ref.sub_line)
            continue;
        return static_cast<std::size_t>(it - chart.notes.begin());
    }
    return std::nullopt;
}

NoteRef ref_of(const Event<Note>& e) {
    return NoteRef{e.measure, e.pos, e.value.lane, e.value.sample.id, e.value.sub_line};
}

/// 同值 note 数量（别名/重复原始行会让一个 ref 匹配多条）。
std::size_t count_note_matches(const Chart& chart, const NoteRef& ref) {
    const auto begin =
        std::lower_bound(chart.notes.begin(), chart.notes.end(), ref.measure,
                         [](const Event<Note>& e, std::uint32_t m) { return e.measure < m; });
    std::size_t count = 0;
    for (auto it = begin; it != chart.notes.end() && it->measure == ref.measure; ++it) {
        if (it->pos == ref.pos && it->value.lane == ref.lane && it->value.sample.id == ref.sample &&
            it->value.sub_line == ref.sub_line)
            ++count;
    }
    return count;
}

bool target_hidden(const Lane& lane, std::uint32_t sub_line, const std::set<std::string>& hidden) {
    if (lane.kind == LaneKind::Bgm) {
        if (hidden.count(bgm_column_key(std::nullopt)) != 0) return true;
        if (hidden.count(bgm_column_key(sub_line)) != 0) return true;
    }
    return hidden.count(lane_key_string(lane)) != 0;
}

std::uint32_t first_free_bgm_sub(const Chart& chart, std::uint32_t measure, const Rational& pos,
                                 const std::set<std::size_t>& moving,
                                 const std::set<std::string>& claimed) {
    for (std::uint32_t sub = 0;; ++sub) {
        bool used = false;
        for (std::size_t i = 0; i < chart.notes.size(); ++i) {
            if (moving.count(i) != 0) continue;
            const auto& e = chart.notes[i];
            if (e.measure == measure && e.pos == pos && e.value.lane.kind == LaneKind::Bgm &&
                e.value.sub_line == sub) {
                used = true;
                break;
            }
        }
        if (!used &&
            claimed.count(lane_target_key(measure, pos, Lane{0, LaneKind::Bgm, 0}, sub)) == 0)
            return sub;
    }
}

bool collides(const Chart& chart, const NoteRef& at, const Lane& lane, std::uint32_t sub_line,
              const std::set<std::size_t>& moving) {
    for (std::size_t i = 0; i < chart.notes.size(); ++i) {
        if (moving.count(i) != 0) continue;
        const auto& e = chart.notes[i];
        if (e.measure != at.measure || e.pos != at.pos || e.value.lane != lane) continue;
        if (lane.kind == LaneKind::Bgm && e.value.sub_line != sub_line) continue;
        return true;
    }
    return false;
}

}  // namespace

std::string lane_target_key(std::uint32_t measure, const Rational& pos, const Lane& lane,
                            std::uint32_t sub_line) {
    return std::to_string(measure) + ":" + std::to_string(pos.num) + "/" + std::to_string(pos.den) +
           ":" + lane_key_string(lane) + ":" + std::to_string(sub_line);
}

bool lane_target_allowed(std::string_view mode, const Lane& lane, const LanePlanOptions& options) {
    if (lane.kind == LaneKind::Bgm) return options.allow_bgm && lane.player == 0 && lane.index == 0;
    if (lane.kind == LaneKind::Pedal) return options.allow_pedal && chart_mode_has_lane(mode, lane);
    return is_playable_lane(mode, lane);
}

std::vector<NoteGroup> group_selection(const Chart& chart, const std::vector<NoteRef>& refs) {
    std::set<NoteRef> normalized;
    for (const auto& ref : refs) {
        const auto idx = find_note_index(chart, ref);
        if (!idx) continue;
        normalized.insert(ref);
        const auto pair = chart.notes[*idx].value.ln_pair;
        if (pair && *pair < chart.notes.size() && *pair != *idx)
            normalized.insert(ref_of(chart.notes[*pair]));
    }

    std::set<NoteRef> used;
    std::vector<NoteGroup> groups;
    for (const auto& ref : normalized) {
        if (used.count(ref) != 0) continue;
        const auto idx = find_note_index(chart, ref);
        if (!idx) continue;
        NoteGroup group;
        group.a = ref;
        group.ambiguous = count_note_matches(chart, ref) > 1;
        used.insert(ref);
        const auto pair = chart.notes[*idx].value.ln_pair;
        if (pair && *pair < chart.notes.size() && *pair != *idx) {
            const NoteRef partner = ref_of(chart.notes[*pair]);
            if (used.count(partner) == 0) {
                group.b = partner;
                group.ln = true;
                group.ambiguous = group.ambiguous || count_note_matches(chart, partner) > 1;
                used.insert(partner);
            }
        }
        groups.push_back(group);
    }
    return groups;
}

// NOLINTNEXTLINE(readability-function-cognitive-complexity)——规划分支表：模式/隐藏/占用/plan 冲突
LanePlan plan_lane_moves(const Chart& chart, const std::vector<NoteGroup>& groups,
                         const std::vector<LaneTarget>& targets, const LanePlanOptions& options) {
    LanePlan plan;
    const std::size_t n = std::min(groups.size(), targets.size());
    plan.unplanned = groups.size() - n;
    if (n == 0) return plan;

    const std::string mode = chart.mode_id.value_or("sp7k");
    std::vector<std::vector<std::size_t>> indices(n);
    std::vector<bool> candidate(n, false);
    plan.moves.resize(n);

    std::set<std::size_t> moving;
    for (std::size_t i = 0; i < n; ++i) {
        auto& move = plan.moves[i];
        move.group = groups[i];
        move.ln = targets[i].ln;
        move.lane = targets[i].lane;

        if (move.group.ambiguous) {
            move.reason = LaneMoveReason::AmbiguousSource;
            continue;
        }
        const auto a = find_note_index(chart, move.group.a);
        std::optional<std::size_t> b;
        if (move.group.b) b = find_note_index(chart, *move.group.b);
        if (!a) {
            move.reason = LaneMoveReason::NotFound;
            continue;
        }
        if (!lane_target_allowed(mode, move.lane, options)) {
            move.reason = LaneMoveReason::InvalidLane;
            continue;
        }
        if (options.require_ln_match && move.group.ln != move.ln) {
            move.reason = LaneMoveReason::LnMismatch;
            continue;
        }
        if (move.group.ln && move.lane.kind == LaneKind::Bgm) {
            move.reason = LaneMoveReason::InvalidLane;
            continue;
        }
        indices[i].push_back(*a);
        if (b) indices[i].push_back(*b);
        candidate[i] = true;
        moving.insert(*a);
        if (b) moving.insert(*b);
    }

    std::set<std::string> claimed;
    for (std::size_t i = 0; i < n; ++i) {
        if (!candidate[i]) continue;
        auto& move = plan.moves[i];
        if (move.lane.kind == LaneKind::Bgm) {
            if (move.group.a.lane.kind == LaneKind::Bgm) {
                move.sub_line = move.group.a.sub_line;
            } else {
                move.sub_line = first_free_bgm_sub(chart, move.group.a.measure, move.group.a.pos,
                                                   moving, claimed);
            }
        } else {
            move.sub_line = 0;
        }
        if (target_hidden(move.lane, move.sub_line, options.hidden_lanes)) {
            move.reason = LaneMoveReason::Hidden;
            continue;
        }
        const std::string key =
            lane_target_key(move.group.a.measure, move.group.a.pos, move.lane, move.sub_line);
        if (claimed.count(key) != 0) {
            move.reason = LaneMoveReason::DuplicateTarget;
            continue;
        }
        claimed.insert(key);
        move.move = true;
    }

    // 动态冲突：先假设所有候选移动，失败者回到静止集后重查，直到稳定。
    bool changed = true;
    while (changed) {
        changed = false;
        std::set<std::size_t> active;
        for (std::size_t i = 0; i < n; ++i)
            if (plan.moves[i].move)
                for (const auto idx : indices[i]) active.insert(idx);
        for (std::size_t i = 0; i < n; ++i) {
            auto& move = plan.moves[i];
            if (!move.move) continue;
            if (collides(chart, move.group.a, move.lane, move.sub_line, active) ||
                (move.group.b &&
                 collides(chart, *move.group.b, move.lane, move.sub_line, active))) {
                move.move = false;
                move.reason = LaneMoveReason::Occupied;
                changed = true;
            }
        }
    }

    for (const auto& move : plan.moves)
        if (!move.move) ++plan.skipped;
    return plan;
}

std::unique_ptr<EditCommand> make_lane_move_command(const LaneMove& move) {
    if (!move.move) return nullptr;
    if (move.group.ln) return std::make_unique<MoveLnPairCommand>(move.group.a, move.lane);
    const std::optional<std::uint32_t> to_sub = move.lane.kind == LaneKind::Bgm
                                                    ? std::optional<std::uint32_t>(move.sub_line)
                                                    : std::nullopt;
    return std::make_unique<MoveNoteCommand>(move.group.a, move.group.a.measure, move.group.a.pos,
                                             move.lane, to_sub);
}

}  // namespace beatbench::edit
