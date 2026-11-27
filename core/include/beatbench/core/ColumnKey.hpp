// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

namespace beatbench {

/// 编辑器列 key 的单一格式源：视图列头（ChartViewItem）、协议 hidden_lanes、
/// 换轨计划预检（LanePlan）三处共用的稳定标识。五类格式：
///   lane:<player>:<kind>:<index>   游玩轨（见 Lane.hpp 的 lane_key_string）
///   bgm:all | bgm:sub:<N>          BGM 聚合列 / 子行（N = sub_line）
///   bga:<layer>                    BGA 图层（0=base 1=poor 2=layer 3=layer2）
///   meta:bpm | meta:stop           元事件轨
/// 新增列类别时在此扩展生成函数，禁止调用方手拼字符串。

/// BGM 列 key：sub_line 有值 = 子行列（bgm:sub:N），nullopt = 聚合列（bgm:all）。
inline std::string bgm_column_key(std::optional<std::uint32_t> sub_line) {
    return sub_line ? "bgm:sub:" + std::to_string(*sub_line) : "bgm:all";
}

/// BGA 图层列 key。
inline std::string bga_column_key(int layer) { return "bga:" + std::to_string(layer); }

/// 元事件轨列 key：kind 取 "bpm" / "stop"。
inline std::string meta_column_key(std::string_view kind) {
    return std::string("meta:").append(kind);
}

}  // namespace beatbench
