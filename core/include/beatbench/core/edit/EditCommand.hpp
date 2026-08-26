// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "beatbench/core/Chart.hpp"
#include "beatbench/core/Payloads.hpp"
#include "beatbench/core/Rational.hpp"

namespace beatbench::edit {

/// 时间轴事件种类（BPM / STOP / 节拍），命令协议 timing.* 的 kind 参数。
/// 三种事件模型值不同（Bpm/Stop/MeasureLen），编辑语义统一：按 (measure,pos) 定位。
enum class TimingKind : std::uint8_t {
    Bpm,   ///< BPM 事件（ch03/ch08；值 = BPM 数值）
    Stop,  ///< STOP 事件（ch09；值 = 时长微秒）
    Measure,  ///< 节拍事件（ch02；值 = 每小节拍数，4/4 = 4）
};

/// note → 跨命名空间事件的目标种类（BGA 层 / BPM / STOP）。
/// 与 TimingKind 平行但语义不同：BGA 用 image.id、BPM/STOP 用 ref_id + 定义值。
enum class ConvertTarget : std::uint8_t {
    BgaBase = 0,  ///< BGA base 层（ch04）
    BgaPoor = 1,  ///< BGA poor 层（ch06）
    BgaLayer = 2, ///< BGA layer 层（ch07）
    BgaLayer2 = 3,///< BGA layer2 层（ch0A）
    Bpm = 4,      ///< BPM 事件（ch03；id → #BPMxx 引用）
    Stop = 5,     ///< STOP 事件（ch09；id → #STOPxx 引用）
};

/// 把 TimingKind 转协议字符串（"bpm"/"stop"/"measure"）。未知 → 空串。
std::string_view timing_kind_name(TimingKind kind);

/// 协议字符串 → TimingKind。未知 → nullopt。
std::optional<TimingKind> timing_kind_from_name(std::string_view name);

/// 编辑命令（02 §6.1 / 01 §5.6）：文档变更的唯一操作面，可逆、可合并。
/// 与协议命令（cmd::Command，一次性 run→Json）平行：编辑命令有状态
/// （作用在 Chart& 上，apply/invert 配对保证可逆）；协议命令无状态。
/// 编辑命令内部可被协议命令包装（如 note.put 的 dispatch 入口），
/// GUI/CLI/脚本共用同一批编辑原语。
class EditCommand {
public:
    virtual ~EditCommand() = default;

    /// 命令名（诊断/日志/协议映射用，如 "note.put"）。
    virtual std::string name() const = 0;

    /// 应用到文档。执行后文档处于新状态；invert() 必须能精确还原。
    virtual void apply(Chart& chart) = 0;

    /// 逆操作：把 chart 还原到 apply 之前的状态。
    /// 必须与 apply 精确互逆（可逆性测试守卫）。
    virtual void invert(Chart& chart) = 0;

    /// 尝试与下一个命令合并（连续拖动/滚轮微调 → 一个 undo 步）。
    /// next 尚未应用；若可合并，把 next 并入自身并返回 true（调用方不再单独入栈），
    /// 否则返回 false（调用方把 next 作为新栈项）。
    /// 默认不合并。
    virtual bool merge_with(const EditCommand& next) { (void)next; return false; }

    /// 人类可读描述（undo 菜单/诊断显示）。
    virtual std::string describe() const { return name(); }
};

/// 复合命令：一组子命令按序 apply/invert（逆序反转），整体 = 一个 undo 步。
/// 批量编辑（框选删除/粘贴/变换）用它组合单 note 命令，不重复实现批量逻辑。
class CompositeCommand : public EditCommand {
public:
    /// 追加子命令（所有权转移；追加后命令不可再被外部修改）。
    void add(std::unique_ptr<EditCommand> cmd);

    /// 子命令数（调用方判断是否产生了任何有效子命令；如平移全落负小节则可能为 0）。
    std::size_t size() const { return m_commands.size(); }

    std::string name() const override;
    void apply(Chart& chart) override;
    void invert(Chart& chart) override;
    std::string describe() const override;

private:
    std::vector<std::unique_ptr<EditCommand>> m_commands;
};

/// 放置/更改时间轴事件（BPM/STOP/节拍 → timing.put，doc/05 §9「工具栏值放置」）。
/// 语义：在 (measure, pos) 处设置 kind 事件的值。**同位替换**——已存在同
/// (measure, pos) 的同类事件则改值，否则插入（与 timing 引擎「同 pos 后者覆盖」一致）。
/// 逆操作：还原到操作前状态（替换 → 恢复旧值；插入 → 移除）。
/// BMS 写出时事件值由 codec 自动派生 #BPMxx/#STOPxx 定义（bms_writer §2），
/// 因此「值」是唯一编辑维度——ch03 内联/引用的文本差异对编辑透明。
///
/// 节拍事件（Measure）约定 pos = 0（BMS ch02 作用于整小节，parser 恒 pos 0）；
/// 传入非 0 pos 时归一为 0（命令自身保证，调用方无需处理）。
class PutTimingCommand : public EditCommand {
public:
    PutTimingCommand(TimingKind kind, std::uint32_t measure, Rational pos, double value,
                     std::optional<std::uint32_t> ref = std::nullopt);
    std::string name() const override { return "timing.put"; }
    void apply(Chart& chart) override;
    void invert(Chart& chart) override;
    std::string describe() const override;

private:
    TimingKind m_kind;
    std::uint32_t m_measure;
    Rational m_pos;
    double m_value;
    std::optional<std::uint32_t> m_ref;  ///< 手动绑定 #BPMxx/#STOPxx id（缺省=由 codec 派生）
    /// apply 前是否存在同位事件（invert 分支：恢复旧值 / 移除）
    bool m_existed = false;
    /// m_existed 时的旧值（invert 恢复；按类型取 value/count/beats）
    double m_old_value = 0;
    /// apply 前存在的旧 ref_id（invert 恢复；event 无 ref_id 时 = nullopt）
    std::optional<std::uint32_t> m_old_ref;
    /// apply 实际插入位置（invert 移除时定位；nullopt = 未执行/替换）
    std::optional<std::size_t> m_applied_index;
};

/// 删除时间轴事件（BPM/STOP/节拍 → timing.delete）。
/// 语义：删除 (measure, pos) 处匹配 kind 的事件（同 pos 多值删除全部——解析器
/// 同 pos 后者覆盖，编辑语义只认位置）。不存在 → 无操作（命令成功）。
/// 逆操作：按快照精确恢复（含原容器位置近似）。
class DeleteTimingCommand : public EditCommand {
public:
    DeleteTimingCommand(TimingKind kind, std::uint32_t measure, Rational pos);
    std::string name() const override { return "timing.delete"; }
    void apply(Chart& chart) override;
    void invert(Chart& chart) override;
    std::string describe() const override;

private:
    TimingKind m_kind;
    std::uint32_t m_measure;
    Rational m_pos;
    /// apply 删除事件的完整快照（invert 恢复；同 pos 多值 = 多条）
    std::vector<Event<Bpm>> m_bpm;
    std::vector<Event<Stop>> m_stop;
    std::vector<Event<MeasureLen>> m_measure_evs;
    /// 删除位置（invert 尽量原位恢复）
    std::size_t m_index = 0;
};

/// note → 时间轴/BGA 事件转换（跨「id 命名空间」移动，2026-09 用户确认：
/// 只要 BMS 格式上 id 可表示就允许移动——游玩轨 note → BGA/BPM/STOP 轨，
/// 其 id 不变，只是查找文件的引用命名空间变了）。
/// 语义：把 (measure, pos, lane, sample) 的 note 移除，在目标位置
/// (to_measure, to_pos) 的目标语义容器（bga_events / bpm_events / stop_events）
/// 插入事件（支持同时时间位移——自由 2D 拖动到 BGA/BPM 轨）：
/// - BGA：sample.id 直接作为 #BMPxx id（WAV↔BMP id 文本相同；#WAV22 ↔ #BMP22）；
/// - BPM：sample.id 作为 #BPMxx 引用 id（ref_id），value 由该 #BPMxx 定义解析；
/// - STOP：sample.id 作为 #STOPxx 引用 id（ref_id），value 由该 #STOPxx 定义解析。
/// 逆操作：反向转换（事件 → note；id 回填 note.sample.id，位置回到 (measure,pos)）。
/// 单个 undo 步；批量 = CompositeCommand（与 note.move 同族）。
class ConvertNoteCommand : public EditCommand {
public:
    ConvertNoteCommand(std::uint32_t measure, Rational pos, Lane lane, std::uint32_t sample,
                       std::uint32_t bgm_line, ConvertTarget target,
                       std::uint32_t to_measure = 0, Rational to_pos = Rational(0, 1));
    std::string name() const override { return "note.convert"; }
    void apply(Chart& chart) override;
    void invert(Chart& chart) override;
    std::string describe() const override;

private:
    std::uint32_t m_measure;      ///< 源 (measure, pos)
    Rational m_pos;
    Lane m_lane;
    std::uint32_t m_sample;
    std::uint32_t m_bgm_line;
    ConvertTarget m_target;
    std::uint32_t m_to_measure;   ///< 目标位置（转换后事件落点；可同一 position）
    Rational m_to_pos;
    /// apply 删除的 note 快照（invert 恢复）
    std::optional<Event<Note>> m_removed;
    /// apply 断开配对的伙伴快照（invert 恢复互指）
    std::optional<Event<Note>> m_partner;
    /// BGA 层号（target==Bga）或 BPM/STOP 的 ref_id；invert 恢复用
    std::int32_t m_bga_layer = -1;
    std::optional<std::uint32_t> m_ref_id;
    /// apply 插入的事件下标（invert 精确移除）
    std::optional<std::size_t> m_insert_index;
    /// 写入 BPM/STOP 的 value 快照（invert 恢复；值 = 由定义表解析）
    double m_value = 0.0;
};

/// 单点 ↔ LN 转换（2026-09 用户：工具栏按钮一键转换；对选中的 note）。
/// 语义（apply）：
/// - note 有 ln_pair（是 LN 一段）→ **断开**：本 note 与伙伴的 ln_pair 都清除
///   （两端变两个独立单点；LN 中段线消失）；
/// - note 无 ln_pair（单点）→ **配 LN**：向前找**最近同 lane 同 sample 未配对**
///   单点配成 LN（互为 ln_pair；忽略中间其它通道）；找不到 → 无操作（命令成功，
///   调用方按「转换数」提示）。
/// invert：精确恢复（断开 → 恢复互指；配对 → 清除互指）。
/// 批量 = CompositeCommand（一个 undo 步）；单选 = 本命令。
class ToggleLnCommand : public EditCommand {
public:
    ToggleLnCommand(std::uint32_t measure, Rational pos, Lane lane, std::uint32_t sample,
                    std::uint32_t bgm_line = 0);
    std::string name() const override { return "note.toggleLn"; }
    void apply(Chart& chart) override;
    void invert(Chart& chart) override;
    std::string describe() const override;

private:
    std::uint32_t m_measure;
    Rational m_pos;
    Lane m_lane;
    std::uint32_t m_sample;
    std::uint32_t m_bgm_line;
    /// apply 前本 note 是否在 LN 通道（invert 恢复原值）
    bool m_was_ln = false;
    /// 单点→LN 时被标记的伙伴下标（invert 清除标记）
    std::optional<std::uint32_t> m_applied_partner;
    bool m_did_change = false;  ///< apply 实际执行（找到 note）
};

/// 重命名「定义表」条目 id（2026-09 用户：双击手动编辑采样 id，为 BGA 编辑打基础）。
/// 语义：定义表键 (kind, from_id) → (kind, to_id)，并把所有引用该 id 的对象一并改到 to_id：
/// - Wav → notes[].sample.id；Bmp → bga_events[].image.id；
/// - Bpm/Stop → bpm_events/stop_events 的 ref_id。
/// 这是**定义表重映射**：文本表示的 #WAVxx 编号变化，实际音频文件不变。
/// 逆操作：反向重映射（to → from），精确恢复（含碰撞时原 to_id 的定义与引用）。
/// 单个 undo 步。
class RenameSampleCommand : public EditCommand {
public:
    RenameSampleCommand(SampleKind kind, std::uint32_t from_id, std::uint32_t to_id);
    std::string name() const override { return "sample.rename"; }
    void apply(Chart& chart) override;
    void invert(Chart& chart) override;
    std::string describe() const override;

private:
    SampleKind m_kind;
    std::uint32_t m_from_id;
    std::uint32_t m_to_id;
    /// apply 前 (kind, from_id) 是否存在（invert：存在→还回，不存在→维持移除）
    bool m_had_from = false;
    SampleDef m_old_def;               ///< apply 前 (kind, from_id) 的定义
    bool m_had_to = false;             ///< apply 前 (kind, to_id) 已有不同定义（碰撞）
    SampleDef m_old_to_def;            ///< 碰撞时原目标定义（invert 恢复）
    /// 引用 from_id 的对象下标（invert 精确还原；按 kind 只用对应容器）
    std::vector<std::size_t> m_note_idx;
    std::vector<std::size_t> m_bga_idx;
    std::vector<std::size_t> m_bpm_idx;
    std::vector<std::size_t> m_stop_idx;
    bool m_changed = false;            ///< apply 实际执行了重映射
};

/// 设置「定义表」条目的文件路径（切音工作区手工版：双击采样行改绑定的文件，sample.setFile）。
/// 语义：samples[(kind, id)].file = file；键不存在则创建（含空文件）。
/// 逆操作：恢复到 apply 前（存在→还回旧值；不存在→移除新键）。单个 undo 步。
class SetSampleFileCommand : public EditCommand {
public:
    SetSampleFileCommand(SampleKind kind, std::uint32_t id, std::string file);
    std::string name() const override { return "sample.setFile"; }
    void apply(Chart& chart) override;
    void invert(Chart& chart) override;
    std::string describe() const override;

private:
    SampleKind m_kind;
    std::uint32_t m_id;
    std::string m_file;
    bool m_existed = false;
    std::string m_old_file;
    bool m_changed = false;
};

/// 设置「定义表」条目的数值原文本（BPM/STOP 定义表手工编辑：右 Dock 时间轴「#BPM/#STOP 定义」，
/// sample.setValue）。语义：samples[(kind, id)].value = value；键不存在则创建。
/// 逆操作：恢复 apply 前（存在→还回旧值；不存在→移除新键）。单个 undo 步。
/// ⚠️ 这是定义表层面的「id → 值」绑定——与事件层面的 timing.put（在 (measure,pos) 放事件，
/// 值可自动派生 #BPMxx）正交；两者共同把「创建 id+绑定值」与「在时间轴使用 id」分离。
class SetSampleValueCommand : public EditCommand {
public:
    SetSampleValueCommand(SampleKind kind, std::uint32_t id, std::string value);
    std::string name() const override { return "sample.setValue"; }
    void apply(Chart& chart) override;
    void invert(Chart& chart) override;
    std::string describe() const override;

private:
    SampleKind m_kind;
    std::uint32_t m_id;
    std::string m_value;
    bool m_existed = false;
    std::string m_old_value;
    bool m_changed = false;
};

/// 修改某个 note 引用的采样 id（编辑区双击 note 改 #WAV id，note.setSample）。
/// 语义：按 (measure, pos, lane, sample, bgm_line) 定位 note，把其 sample 引用改为 to。
/// 仅改这一条 note 的引用（不重命名定义表；音频文件不变）。找不到 → 无操作。
/// 逆操作：恢复原 sample 引用。单个 undo 步。
class SetNoteSampleCommand : public EditCommand {
public:
    SetNoteSampleCommand(std::uint32_t measure, Rational pos, Lane lane, std::uint32_t sample,
                         std::uint32_t bgm_line, std::uint32_t to);
    std::string name() const override { return "note.setSample"; }
    void apply(Chart& chart) override;
    void invert(Chart& chart) override;
    std::string describe() const override;

private:
    std::uint32_t m_measure;
    Rational m_pos;
    Lane m_lane;
    std::uint32_t m_sample;
    std::uint32_t m_bgm_line;
    std::uint32_t m_to;
    std::optional<std::size_t> m_applied_index;  ///< 命中的 note 下标（invert 定位）
    bool m_did_change = false;
};

/// 删除「定义表」条目（#WAV/#BMP/#BPM/#STOP 的 id 定义，sample.delete）。
/// 语义：从 samples 移除 (kind, id)。若仍有对象引用该 id，则**只删定义、引用保持原 id**——
/// 与文件管理器「解绑文件」语义一致（引用保留 id，但无文件/值定义 → 之后写回派生）。
/// 逆操作：恢复被移除的定义（存在时还回）。单个 undo 步。
class DeleteSampleCommand : public EditCommand {
public:
    DeleteSampleCommand(SampleKind kind, std::uint32_t id);
    std::string name() const override { return "sample.delete"; }
    void apply(Chart& chart) override;
    void invert(Chart& chart) override;
    std::string describe() const override;

private:
    SampleKind m_kind;
    std::uint32_t m_id;
    bool m_existed = false;      ///< apply 前存在（invert 还回）
    SampleDef m_old_def;         ///< apply 前的定义
    bool m_changed = false;      ///< apply 实际删除了定义
};

}  // namespace beatbench::edit
