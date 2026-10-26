// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <vector>

#include "beatbench/core/Chart.hpp"
#include "beatbench/core/Rational.hpp"
#include "beatbench/core/edit/Selection.hpp"

namespace beatbench::edit {

class EditCommand;

/// 逻辑 note 组：普通 note 或一条 LN 的头尾两端。
/// a 是较早的一端（按 (measure,pos,lane,sub_line) 排序），b 是 LN 另一端。
struct NoteGroup {
    NoteRef a;
    std::optional<NoteRef> b;
    bool ln = false;
    bool ambiguous = false;  ///< 源 ref 在谱面中匹配到多个同值 note（别名/重复行）
};

/// 目标轨：lane + 是否要求目标也是 LN（配置复制的 pattern 槽）。
struct LaneTarget {
    Lane lane;
    bool ln = false;
};

/// 规划失败原因（enum 而非字符串：调用方按类别 switch，拼错编译期报错）。
enum class LaneMoveReason : std::uint8_t {
    None,             ///< 成功
    NotFound,         ///< 源 note 已不存在
    InvalidLane,      ///< 模式表/开关不允许的目标轨（含 LN 不可进 BGM）
    Hidden,           ///< 目标轨在活动视口隐藏
    LnMismatch,       ///< pattern 槽 LN 标记与源组不一致
    Occupied,         ///< 目标位置已有其它 note（含动态冲突收敛）
    DuplicateTarget,  ///< 多个组套用同一目标槽
    AmbiguousSource,  ///< 源 ref 匹配到多个同值 note，无法确定移动哪一个
};

struct LanePlanOptions {
    /// 允许 BGM 目标（配置复制为 true；快速摆音为 false）。
    bool allow_bgm = false;
    /// 允许踏板目标（默认 false；快速摆音/配置复制只支持 key/scratch/BGM）。
    bool allow_pedal = false;
    /// 目标 ln 标记必须与源组一致（配置复制为 true；快速摆音为 false）。
    bool require_ln_match = false;
    /// 当前活动视口隐藏列 key；BGM 同时识别 bgm:all / bgm:sub:N。
    std::set<std::string> hidden_lanes;
};

/// 一个目标槽的规划结果。move=false 时 reason 说明原因，调用方不执行。
struct LaneMove {
    NoteGroup group;
    Lane lane;
    std::uint32_t sub_line = 0;
    bool ln = false;
    bool move = false;
    LaneMoveReason reason = LaneMoveReason::None;
};

struct LanePlan {
    std::vector<LaneMove> moves;  ///< 与 min(groups, targets) 对齐
    std::size_t skipped = 0;      ///< moves 中 move=false 的数量
    std::size_t unplanned = 0;    ///< 目标槽用完后剩余的组数
};

/// 选中集归一：去重、补 LN 伙伴、按 (measure,pos,lane,sub_line) 排序、组 LN 头尾。
std::vector<NoteGroup> group_selection(const Chart& chart, const std::vector<NoteRef>& refs);

/// 目标 lane 是否允许（模式表 + BGM/踏板开关）。
bool lane_target_allowed(std::string_view mode, const Lane& lane, const LanePlanOptions& options);

/// 规划“把组按目标槽换轨”：校验 lane/LN/隐藏/占用/plan 间冲突；不执行。
/// 目标槽不足时，剩余组计入 unplanned，不移动。
LanePlan plan_lane_moves(const Chart& chart, const std::vector<NoteGroup>& groups,
                         const std::vector<LaneTarget>& targets, const LanePlanOptions& options);

/// 按规划结果生成编辑命令；move=false 时返回 nullptr。
std::unique_ptr<EditCommand> make_lane_move_command(const LaneMove& move);

/// 目标位置 key（BGM 带 sub_line；非 BGM 忽略 sub_line）。
std::string lane_target_key(std::uint32_t measure, const Rational& pos, const Lane& lane,
                            std::uint32_t sub_line);

}  // namespace beatbench::edit
