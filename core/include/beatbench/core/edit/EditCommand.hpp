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
    PutTimingCommand(TimingKind kind, std::uint32_t measure, Rational pos, double value);
    std::string name() const override { return "timing.put"; }
    void apply(Chart& chart) override;
    void invert(Chart& chart) override;
    std::string describe() const override;

private:
    TimingKind m_kind;
    std::uint32_t m_measure;
    Rational m_pos;
    double m_value;
    /// apply 前是否存在同位事件（invert 分支：恢复旧值 / 移除）
    bool m_existed = false;
    /// m_existed 时的旧值（invert 恢复；按类型取 value/duration_us/beats）
    double m_old_value = 0;
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

}  // namespace beatbench::edit
