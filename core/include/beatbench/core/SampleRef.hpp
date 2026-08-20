// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include <cstdint>

namespace beatbench {

/// 采样引用：模型层的不透明 id。
/// - BMS 的 36 进制编号 "01".."ZZ" 与 1296 上限是 bms codec 层的约束（定义表负责映射）；
/// - bmson 的 sound_channels 数组下标直接映射，无此上限；
/// - id == 0 表示无效/未绑定。
struct SampleRef {
    std::uint32_t id = 0;

    friend bool operator==(const SampleRef&, const SampleRef&) = default;
};

}  // namespace beatbench
