// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include <cstdint>
#include <map>
#include <string>
#include <unordered_map>
#include <vector>

#include "beatbench/core/Event.hpp"
#include "beatbench/core/Payloads.hpp"
#include "beatbench/core/SampleRef.hpp"

namespace beatbench {

/// 采样/图片/值定义（#WAVxx/#BMPxx/#BPMxx/#STOPxx 的模型形态）。
/// 键 = SampleRef.id；BMS 的 36 进制编号由 bms codec 映射。
struct SampleDef {
    std::string file;  ///< 相对谱面目录的路径
};

/// 权威谱面模型（格式无关）。任何 codec 都能填充；
/// 未知头部字段与格式扩展 → extensions 透传，保证往返保真（对齐稿 02 §4）。
struct Chart {
    std::unordered_map<std::string, std::string> meta;  ///< 头部字段，键名大写、值原样
    std::unordered_map<std::uint32_t, SampleDef> samples;

    std::vector<Event<Note>> notes;
    std::vector<Event<Bpm>> bpm_events;
    std::vector<Event<Stop>> stop_events;
    std::vector<Event<MeasureLen>> measure_events;
    std::vector<Event<Bga>> bga_events;

    std::map<std::string, std::string> extensions;  ///< 未知头部字段与格式扩展
};

}  // namespace beatbench
