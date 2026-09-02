// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include <string_view>

namespace beatbench {

/// 引擎版本（CLI 显示、JSON version 命令、GUI 关于页共用同一来源）。
inline constexpr std::string_view kVersion = "0.2.0";

/// 命令协议 API 版本：请求/响应结构或语义不兼容变更时 +1。
/// 客户端应读取 version 命令返回的 api 并按其适配。
inline constexpr int kApiVersion = 1;

}  // namespace beatbench
