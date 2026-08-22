// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include <memory>
#include <string>
#include <vector>

#include "beatbench/core/Chart.hpp"

namespace beatbench::edit {

/// 编辑命令（02 §6.1 / 01 §5.6）：文档变更的唯一操作面，可逆、可合并。
/// 与协议命令（cmd::Command，一次性 run→Json）平行：编辑命令有状态
/// （作用在 Chart& 上，apply/invert 配对保证可逆）；协议命令无状态。
/// 编辑命令内部可被协议命令包装（如 note.put 的 dispatch 入口），
/// GUI/CLI/脚本共用同一批编辑原语。
class EditCommand {
public:
    virtual ~EditCommand() = default;

    /// 命令名（诊断/日志/协议映射用，如 "note.put"）。
    virtual std::string name() const = 0;

    /// 应用到文档。执行后文档处于新状态；invert() 必须能精确还原。
    virtual void apply(Chart& chart) = 0;

    /// 逆操作：把 chart 还原到 apply 之前的状态。
    /// 必须与 apply 精确互逆（可逆性测试守卫）。
    virtual void invert(Chart& chart) = 0;

    /// 尝试与下一个命令合并（连续拖动/滚轮微调 → 一个 undo 步）。
    /// next 尚未应用；若可合并，把 next 并入自身并返回 true（调用方不再单独入栈），
    /// 否则返回 false（调用方把 next 作为新栈项）。
    /// 默认不合并。
    virtual bool merge_with(const EditCommand& next) { (void)next; return false; }

    /// 人类可读描述（undo 菜单/诊断显示）。
    virtual std::string describe() const { return name(); }
};

/// 复合命令：一组子命令按序 apply/invert（逆序反转），整体 = 一个 undo 步。
/// 批量编辑（框选删除/粘贴/变换）用它组合单 note 命令，不重复实现批量逻辑。
class CompositeCommand : public EditCommand {
public:
    /// 追加子命令（所有权转移；追加后命令不可再被外部修改）。
    void add(std::unique_ptr<EditCommand> cmd);

    std::string name() const override;
    void apply(Chart& chart) override;
    void invert(Chart& chart) override;
    std::string describe() const override;

private:
    std::vector<std::unique_ptr<EditCommand>> m_commands;
};

}  // namespace beatbench::edit
