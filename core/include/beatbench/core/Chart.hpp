// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include <cstdint>
#include <map>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include "beatbench/core/Event.hpp"
#include "beatbench/core/Payloads.hpp"
#include "beatbench/core/SampleRef.hpp"

namespace beatbench {

/// 定义表条目类型（BMS #WAVxx/#BMPxx/#BPMxx/#STOPxx 的模型形态，格式无关）。
/// 键 = (kind, id)：**BMS 中四类定义表是独立命名空间**（#BPM01 与 #BMP01 可共存），
/// 其他格式（bmson sound_channels 等）映射到同构概念。
/// SampleRef.id 单独出现时由使用方语境决定 kind（note 引用 WAV、BGA 引用 BMP）。
enum class SampleKind : std::uint8_t {
    Wav,  ///< 音频采样（#WAVxx → file）
    Bmp,  ///< 图像/BGA（#BMPxx → file）
    Bpm,  ///< BPM 定义（#BPMxx → value 原文本，保留精度）
    Stop, ///< STOP 定义（#STOPxx → value 原文本）
};

struct SampleDef {
    std::string file;   ///< Wav/Bmp：相对谱面目录的路径
    std::string value;  ///< Bpm/Stop：数值原文本（往返保留精度，codec 展开事件时再解析）

    friend bool operator==(const SampleDef&, const SampleDef&) = default;
};

/// 定义表 id 进制（BMS 文本层特性，模型层记录以便 codec 解释/输出 id 文本）。
/// 默认 36（0-9A-Z，大小写折叠）；`#BASE 62` 扩展 → 大小写敏感 base62（62×62=3844，
/// LR2 扩展 DLL / beatoraja 支持，见 BMS文件分析笔记）。其他格式可用自己的约定。
enum class IdBase : std::uint8_t {
    Base36 = 36,
    Base62 = 62,
};

/// 权威谱面模型（格式无关）。任何 codec 都能填充；
/// 未知头部字段与格式扩展 → extensions 透传，保证往返保真（对齐稿 02 §4）。
struct Chart {
    std::unordered_map<std::string, std::string> meta;  ///< 头部字段，键名大写、值原样
    /// 定义表：键 = (kind, id)。BMS 的 36 进制编号由 bms codec 映射；
    /// id 上限（1296 / base62 3844）是 bms codec 层的约束与校验，模型不设限。
    std::map<std::pair<SampleKind, std::uint32_t>, SampleDef> samples;

    IdBase id_base = IdBase::Base36;  ///< 定义表 id 进制（见上）；#BASE 指令由 parser 设置

    /// 游玩模式 id（格式无关配置表，见 ChartMode.hpp）。
    /// 读取时由 codec 推断并写入（bms：#PLAYER/扩展名 → sp7k/dp/battle/pms9k；
    /// 5k 不区分，一律 sp7k 呈现）；写回时决定通道反向映射表。
    /// 缺省 = 调用方按默认模式处理（bms 为 sp7k）。
    std::optional<std::string> mode_id;

    /// 未结构化消费的原始行（保序）：注释块、控制指令（#RANDOM/#IF…）、
    /// 数据行（#mmmcc:…，note 解析前的承载）、未知行。写回时原样输出。
    /// 这是格式无关的通用保留机制：任何 codec 都能把暂不理解的文本行存到这里。
    std::vector<std::string> raw_lines;

    std::vector<Event<Note>> notes;
    std::vector<Event<Bpm>> bpm_events;
    std::vector<Event<Stop>> stop_events;
    std::vector<Event<MeasureLen>> measure_events;
    std::vector<Event<Bga>> bga_events;

    std::map<std::string, std::string> extensions;  ///< 未知头部字段与格式扩展
};

}  // namespace beatbench
