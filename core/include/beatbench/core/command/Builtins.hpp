// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include "beatbench/core/command/Command.hpp"

namespace beatbench::cmd {

/// 注册全部内建命令（version / capabilities / info / check / convert）。
/// 可重复调用：同名字会覆盖已注册的同名命令（便于测试注入替身）。
void register_builtin_commands(Registry& registry);

}  // namespace beatbench::cmd
