// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "beatbench/core/Chart.hpp"
#include "beatbench/core/Lane.hpp"
#include "beatbench/core/Rational.hpp"
#include "beatbench/core/edit/EditCommand.hpp"
#include "beatbench/core/edit/Selection.hpp"

namespace beatbench::edit {

/// 编辑会话：持有可编辑文档（Chart）+ undo/redo 栈 + 执行编辑命令。
/// 纯 core 零 Qt；GUI/CLI/测试三方复用（02 §6.1「命令对象 = 唯一操作面」的文档侧承载）。
/// 会话状态（Selection/Viewport/Playhead）不入 undo 栈（01 §5.6）——由调用方自持。
class EditorSession {
public:
    EditorSession();

    /// 用初始 Chart 装载（编辑前调用；后续编辑通过 exec 修改同一文档）。
    /// 单参版本不记录路径（保存时需显式传 path）。
    void load(Chart chart);

    /// 带路径装载：记录文档路径（session.save 缺省写回目标）。
    void load(Chart chart, std::string path);

    const Chart& chart() const { return *m_chart; }
    Chart& chart_mut() { return *m_chart; }  ///< 仅供非编辑直接操作（如导入）；正常走 exec

    bool has_chart() const { return m_chart != nullptr; }

    /// 文档路径（load 时设置；另存为后由调用方 set_path 更新；未设置 = 空串）。
    const std::string& path() const { return m_path; }
    void set_path(std::string path) { m_path = std::move(path); }

    /// 执行编辑命令：apply → 入 undo 栈（清空 redo）；返回是否成功。
    /// 失败（命令抛出）→ 文档回滚到执行前（undo 栈不变）。
    bool exec(std::unique_ptr<EditCommand> cmd);

    /// 撤销：undo 栈顶 invert → 移到 redo 栈。无命令可撤 → false。
    bool undo();

    /// 重做：redo 栈顶 apply → 移到 undo 栈。无命令可重做 → false。
    bool redo();

    bool can_undo() const { return !m_undo.empty(); }
    bool can_redo() const { return !m_redo.empty(); }
    std::size_t undo_depth() const { return m_undo.size(); }
    std::size_t redo_depth() const { return m_redo.size(); }

    /// 撤销/重做栈顶命令描述（菜单显示；空 → 空串）。
    std::string undo_label() const;
    std::string redo_label() const;

    /// 选择集（会话状态，不入 undo 栈；doc/01 §5.6）。
    /// load() 时清空；编辑命令不改 selection（note 值标识天然稳定）。
    const Selection& selection() const { return m_selection; }
    Selection& selection_mut() { return m_selection; }
    void set_selection(Selection sel) { m_selection = std::move(sel); }

private:
    std::unique_ptr<Chart> m_chart;
    std::vector<std::unique_ptr<EditCommand>> m_undo;
    std::vector<std::unique_ptr<EditCommand>> m_redo;
    Selection m_selection;
    std::string m_path;
};

// —— 具体编辑命令（edit_commands.cpp 实现） ——

/// 放置 note（doc/05 §9「视口点放」→ note.put）。
/// 语义：在 (measure, pos) 的 lane 上放 sample 引用的 note。
/// 逆操作：精确删除该 (measure, pos, lane) 处、匹配 sample 的 note。
class PutNoteCommand : public EditCommand {
public:
    PutNoteCommand(std::uint32_t measure, Rational pos, Lane lane, std::uint32_t sample);
    std::string name() const override { return "note.put"; }
    void apply(Chart& chart) override;
    void invert(Chart& chart) override;
    std::string describe() const override;

private:
    std::uint32_t m_measure;
    Rational m_pos;
    Lane m_lane;
    std::uint32_t m_sample;
    /// apply 实际插入的 note 下标（invert 用它精确删除；同 pos 冲突分裂等场景）
    std::optional<std::size_t> m_applied_index;
};

/// 移动 note（doc/05 §9「视口拖动」→ note.move，merge_with 合并拖动）。
/// 语义：把 (from_measure, from_pos, lane) 处、匹配 sample 的 note 移到目标位置。
/// 逆操作：移回原位。连续移动同一 note → merge 为一个 undo 步。
///
/// LN 处理（2026-08 对齐 BMS 编辑器惯例，用户确认）：
/// - move_ln_pair = false（默认）：LN 当作单个 note 处理——只移动该 note，
///   配对解除（伙伴 ln_pair 清空）。配对断了由解析器自然延续/ lint 告警，
///   编辑器不主动修复（BMS 编辑器常见行为）。
/// - move_ln_pair = true：识别到 LN 则整体头尾一起移动（保持相对位置），配对保留。
class MoveNoteCommand : public EditCommand {
public:
    MoveNoteCommand(std::uint32_t from_measure, Rational from_pos, Lane lane,
                    std::uint32_t sample, std::uint32_t to_measure, Rational to_pos,
                    bool move_ln_pair = false);
    std::string name() const override { return "note.move"; }
    void apply(Chart& chart) override;
    void invert(Chart& chart) override;
    bool merge_with(const EditCommand& next) override;
    std::string describe() const override;

private:
    std::uint32_t m_from_measure;
    Rational m_from_pos;
    Lane m_lane;
    std::uint32_t m_sample;
    std::uint32_t m_to_measure;
    Rational m_to_pos;
    bool m_move_ln_pair;
    /// apply 快照（invert 恢复；找不到 note 时为空）
    std::optional<Event<Note>> m_moved;
    std::optional<Event<Note>> m_partner;
    Rational m_last_delta{0, 1};
};

/// 删除 note（doc/05 §9「选择集删除」→ note.delete）。
/// 语义：删除 (measure, pos, lane) 处匹配 sample 的 note。
/// 逆操作：精确恢复该 note（含原容器位置近似——重插同 (measure,pos,lane,sample)）。
class DeleteNoteCommand : public EditCommand {
public:
    DeleteNoteCommand(std::uint32_t measure, Rational pos, Lane lane, std::uint32_t sample);
    std::string name() const override { return "note.delete"; }
    void apply(Chart& chart) override;
    void invert(Chart& chart) override;
    std::string describe() const override;

private:
    std::uint32_t m_measure;
    Rational m_pos;
    Lane m_lane;
    std::uint32_t m_sample;
    /// apply 删除的 note 的完整拷贝（invert 恢复；含 ln_pair 等）
    std::optional<Event<Note>> m_removed;
    /// 原容器位置（invert 尽量原位恢复；同 (measure,pos,lane) 冲突时退化为追加）
    std::size_t m_removed_index = 0;
    /// 原配对伙伴快照（apply 清空伙伴 ln_pair 前保存；invert 按值重定位恢复互指）
    std::optional<Event<Note>> m_partner;
};

}  // namespace beatbench::edit
