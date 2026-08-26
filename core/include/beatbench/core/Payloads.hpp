// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include <cstdint>
#include <optional>

#include "beatbench/core/Lane.hpp"
#include "beatbench/core/SampleRef.hpp"

namespace beatbench {

/// note 类型。特殊属性放这里而非 Lane（Lane 只管轨道身份）。
enum class NoteKind : std::uint8_t {
    Normal,    ///< 普通 note（含 LN 头尾）
    Landmine,  ///< 地雷（BMS D1-D9 / E1-E9）
    // 预留：Hidden（隐形/WW）等按需加入
};

/// 音符。LN：头尾是两条独立 Event，通过 ln_pair 互指（存 notes 容器下标）。
/// lane = 轨道身份（由 codec 的通道映射表解析得出，格式无关）；
/// 语义与 LNTYPE 1/2 的文本表示解耦——转换由 bms codec 负责（对齐稿 02 §4.1）。
struct Note {
    Lane lane;
    SampleRef sample;
    NoteKind kind = NoteKind::Normal;
    std::optional<std::uint32_t> ln_pair;  ///< 配对 note 的下标（LN 头<->尾）
    /// 是否来自 BMS LN 通道（51-69 / 61-69，LNTYPE 1）。parser 填初始值；
    /// 编辑后由 rebuild_ln_pairs 统一推导（编辑命令**不维护** ln_pair——用户原则：
    /// ln_pair 是「自下而上：文件数据 → 解析 → 效果呈现」的派生，不是编辑真相源）。
    /// 前端用它：① LN note 深色 ② 未配对时 lint 提示。
    bool ln_channel = false;
    /// BGM 行序号（仅 LaneKind::Bgm 有意义）：该小节内第几次读到 ch01（2026-09 用户确认：
    /// BGM 展开 = 按行序分列，非按 #WAV id；空行也占位）。展示辅助字段——不参与语义相等
    /// （多个同 (pos,lane,sample) 的 Bgm note 仅因行号不同应视为同对象），parser 填初始值，
    /// 移动/编辑后由命令层按新位置所在行更新；writer 按它分组写回保持原多行结构。
    std::uint32_t bgm_line = 0;

    friend bool operator==(const Note& a, const Note& b) {
        return a.lane == b.lane && a.sample == b.sample && a.kind == b.kind &&
               a.ln_pair == b.ln_pair;  // 排除 bgm_line / ln_channel（辅助、派生）
    }
};

/// BPM 事件（BMS ch03 内联值或 ch08/#BPMxx 引用解析后的落值）。
struct Bpm {
    double value = 130.0;  ///< 可为小数；负数按规范宽容处理（lint 告警）
    /// 原始 #BPMxx 引用 id（可选）：解析自 ch03 定宽槽位 / ch08；
    /// 写回优先用它输出槽位文本（保持「id 不变」，跨命名空间移动/往返保真基础）。
    /// 内联数值（ch03 奇数长）无引用 → nullopt。
    /// ⚠️ 辅助字段：不参与语义相等（roundtrip/命令比较按值；ref_id 只是文本表示辅助）。
    std::optional<std::uint32_t> ref_id;
    /// 源通道是 ch08（#BPMxx/#EXBPMxx 引用通道）（2026-09 用户修复）：
    /// ch03 是 `00-FF` 十六进制 BPM 值（最大 255），ch08 才是 #BPMxx 引用通道；
    /// 写回必须按源通道输出——否则 `#00108:00000002`（引用 #BPM02=280）会被写成
    /// `#00103:00000002`，标准播放器按十六进制读成 2 BPM（格式破坏）。
    /// ⚠️ 辅助字段：不参与语义相等。
    bool ch08 = false;

    friend bool operator==(const Bpm& a, const Bpm& b) {
        return a.value == b.value;  // 排除 ref_id/ch08（与 Note.bgm_line 同理）
    }
    friend bool operator<(const Bpm& a, const Bpm& b) { return a.value < b.value; }
};

/// STOP 事件。模型存原始计数 n（BMS #STOPxx n = n/192 个「当前 BPM 的全音符」）。
/// 全音符 = 4 拍 = 240/BPM 秒（**不受拍子设置影响**，只看触发时的 BPM），
/// 故 STOP 秒数 = n × 240/(192×BPM) = n × 1.25/BPM。⚠️ 不能把 n 烘焙成固定秒——
/// 同一 #STOPxx 在不同 BPM 下时长不同；秒数换算交给 TimingEngine（按该拍位生效 BPM）。
/// 注：hitkey 通道表把 STOP 记为「音符单位、随当下 BPM 变化」（1/192 拍）；
/// 早期实现误存 n/192 秒（固定秒），2026-09 修正为存原始计数。
struct Stop {
    std::int64_t count = 0;  ///< 原始计数 n（1/192 全音符单位；与 BMS #STOPxx 文本一致）
    /// 原始 #STOPxx 引用 id（可选）：解析自 ch09 槽位；写回优先用它（与 Bpm.ref_id 同理）。
    /// ⚠️ 辅助字段：不参与语义相等。
    std::optional<std::uint32_t> ref_id;

    friend bool operator==(const Stop& a, const Stop& b) {
        return a.count == b.count;  // 排除 ref_id
    }
    friend bool operator<(const Stop& a, const Stop& b) {
        return a.count < b.count;
    }
};

/// 节拍事件（BMS ch02：每小节拍数，4/4 = 4；0.5 = 半拍小节）。
/// 小节时长 = beats × 60 / BPM（由 timing 引擎按 BPM 分段积分）。
struct MeasureLen {
    double beats = 4.0;

    friend bool operator==(const MeasureLen&, const MeasureLen&) = default;
    friend bool operator<(const MeasureLen& a, const MeasureLen& b) {
        return a.beats < b.beats;
    }
};

/// BGA 对象（#BMPxx 引用 + 图层 + 不透明度，最小骨架；ARGB 滤镜后续加）。
struct Bga {
    SampleRef image;
    std::int32_t layer = 0;      ///< 0=base 1=poor 2=layer 3=layer2
    std::uint8_t opacity = 255;  ///< 0-255，255=不透明

    friend bool operator==(const Bga&, const Bga&) = default;
    friend bool operator<(const Bga& a, const Bga& b) {
        if (a.image.id != b.image.id) return a.image.id < b.image.id;
        if (a.layer != b.layer) return a.layer < b.layer;
        return a.opacity < b.opacity;
    }
};

}  // namespace beatbench
