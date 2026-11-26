// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include "beatbench/core/command/Command.hpp"

namespace beatbench::cmd {

/// 注册项目相关命令（M7：project.scan——文件夹 → 谱面/资源索引）。
/// 由 register_builtin_commands 统一调用；亦可单独调用（测试注入）。
void register_project_commands(Registry& registry);

}  // namespace beatbench::cmd
