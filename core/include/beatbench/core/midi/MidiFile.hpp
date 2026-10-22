// SPDX-License-Identifier: GPL-3.0-only
// MIDI 文件解析器（M6.1）：Standard MIDI File（.mid）→ note/tempo/拍号模型。
//
// 定位（doc/core-model.md 三、）：M6 切音工作台的「MIDI 驱动」数据源——MIDI note 天然给出
// 切片边界（note on/off）与铺放拍位。零 Qt、零依赖、可单测；CLI/GUI/测试三方复用。
//
// 解析范围（刻意保守）：
// - 读：MThd（format 0/1/2、ntrks、division PPQ；SMPTE division 亦支持但降级提示）、
//   MTrk 事件（delta VLQ + 通道消息 + meta）。通道消息中仅 note on/off 有用，
//   其余（CC/弯音/触后/程序切换）消费后跳过；meta 仅 0x51 tempo / 0x58 拍号 / 0x2F EOT。
// - 正确性要点：running status（状态字节复用）、Note On 力度=0 视作 Note Off、
//   tempo 分段换算 tick→秒（默认 120 BPM）、悬空 on/off 计入 warnings 不中断。
// - 不做：写 MIDI、SysEx 内容解析、通道过滤/力度阈值（M6.2 按需加）。
//
// 错误策略：结构性损坏（头/块长度、未知 format、运行状态缺失）抛 MidiError；
// 可恢复的不一致（悬空 Note、SMPTE 下 tempo、format 2）进 warnings。
#pragma once

#include <cstdint>
#include <stdexcept>
#include <string>
#include <vector>

namespace beatbench::midi {

/// 解析失败（结构性损坏）；命令层转 CommandError("bad_midi")。
class MidiError : public std::runtime_error {
public:
    explicit MidiError(const std::string& message) : std::runtime_error(message) {}
};

/// 一个配对完成的音符（同轨/同通道/同音高 on→off）。
struct MidiNote {
    int track = 0;        ///< 轨道序号（0 起）
    int channel = 0;      ///< MIDI 通道 0-15
    int pitch = 0;        ///< MIDI 音高 0-127（60 = C4）
    int velocity = 0;     ///< Note On 力度（1-127）
    int startTick = 0;    ///< 起止 tick（绝对，PPQ 基准）
    int endTick = 0;
    double startSec = 0.0;  ///< 经 tempo 表换算的秒
    double endSec = 0.0;
};

/// tempo 事件（0x51）：tick 起生效 usPerQuarter 微秒/四分音符。
/// sec = 该 tempo 生效点的累计秒（由 tempo 表换算；SMPTE 下无意义）。
struct MidiTempo {
    int tick = 0;
    int usPerQuarter = 500000;  ///< 500000 = 120 BPM（规范默认）
    double sec = 0.0;
};

/// 拍号事件（0x58）：tick 起生效；denominator 存真值（如 4/4 → 4，非 2 的幂指数）。
struct MidiTimeSig {
    int tick = 0;
    int numerator = 4;
    int denominator = 4;
    double sec = 0.0;
};

/// 解析产物（全部值，无文件路径引用）。
struct MidiFile {
    int format = 0;            ///< 0 单轨 / 1 多轨同步 / 2 顺序
    int ntrks = 0;
    int division = 0;          ///< raw 16 位（PPQ 或 SMPTE 编码）
    bool isSMPTE = false;      ///< true = division 高位为 1（帧率时间基）
    int ppq = 0;               ///< ticks per quarter（!isSMPTE 时有意义）
    int framesPerSecond = 0;   ///< SMPTE 帧率（isSMPTE 时有意义）
    int ticksPerFrame = 0;     ///< SMPTE 每帧 tick（isSMPTE 时有意义）
    double durationSec = 0.0;  ///< 整曲时长（最后一个事件/音符的秒）

    std::vector<MidiNote> notes;        ///< 全部配对音符（按 startTick/track/channel/pitch 排序）
    std::vector<MidiTempo> tempos;      ///< 按 tick 升序；sec 已换算
    std::vector<MidiTimeSig> timeSigs;  ///< 按 tick 升序；sec 已换算
    std::vector<std::string> warnings;  ///< 可恢复的不一致（人类可读中文）
};

/// 解析字节流。结构性损坏抛 MidiError。
MidiFile parse_midi_bytes(const std::vector<std::uint8_t>& bytes);

}  // namespace beatbench::midi
