// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include <string>
#include <string_view>

#include "beatbench/core/Chart.hpp"

namespace beatbench::commands {

struct CommandResult {
    bool ok = true;
    std::string error;  ///< 失败原因（可展示给用户）
};

/// 命令对象 = 唯一操作面（对齐稿 02 §6.1）：
/// - GUI   = 交互式执行器（执行 → 入 undo 栈 → 视图刷新）；
/// - CLI   = 批处理执行器（同一命令 headless 运行）；
/// - Lua   = 第三种执行器（Phase D）。
/// 设计约束：
/// - apply 失败不得留下半成品状态（先校验后落盘）；
/// - invert 必须恢复 apply 前状态（undo 无上限）；
/// - merge_with 用于合并连续操作（拖动/滚轮微调 = 一个 undo 步）。
/// JSON 序列化（参数/结果）随 M1 引入（nlohmann/json 或自写最小 JSON）。
class Command {
public:
    virtual ~Command() = default;

    /// 稳定命令名（CLI 子命令与 JSON 字段共用），如 "note.put"、"bpm.change"。
    virtual std::string_view name() const = 0;

    /// 正向执行。
    virtual CommandResult apply(Chart& chart) = 0;

    /// 反向执行（undo）。
    virtual CommandResult invert(Chart& chart) = 0;

    /// 尝试与下一条命令合并为一步 undo；返回 false 表示不可合并。
    virtual bool merge_with(const Command& next) {
        (void)next;
        return false;
    }

    // virtual std::string serialize_params() const = 0;                    // M1
    // static std::unique_ptr<Command> deserialize(std::string_view json);  // M1
};

}  // namespace beatbench::commands
