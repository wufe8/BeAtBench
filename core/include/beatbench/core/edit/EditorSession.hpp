// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "beatbench/core/Chart.hpp"
#include "beatbench/core/Lane.hpp"
#include "beatbench/core/Rational.hpp"
#include "beatbench/core/edit/EditCommand.hpp"
#include "beatbench/core/edit/Selection.hpp"

namespace beatbench::edit {

/// 编辑会话：持有可编辑文档（Chart）+ undo/redo 栈 + 执行编辑命令。
/// 纯 core 零 Qt；GUI/CLI/测试三方复用（02 §6.1「命令对象 = 唯一操作面」的文档侧承载）。
/// 会话状态（Selection/Viewport/Playhead）不入 undo 栈（01 §5.6）——由调用方自持。
class EditorSession {
public:
    EditorSession();

    /// 用初始 Chart 装载（编辑前调用；后续编辑通过 exec 修改同一文档）。
    /// 单参版本不记录路径（保存时需显式传 path）。
    void load(Chart chart);

    /// 带路径装载：记录文档路径（session.save 缺省写回目标）。
    void load(Chart chart, std::string path);

    const Chart& chart() const { return *m_chart; }
    Chart& chart_mut() { return *m_chart; }  ///< 仅供非编辑直接操作（如导入）；正常走 exec

    bool has_chart() const { return m_chart != nullptr; }

    /// 文档路径（load 时设置；另存为后由调用方 set_path 更新；未设置 = 空串）。
    const std::string& path() const { return m_path; }
    void set_path(std::string path) { m_path = std::move(path); }

    /// 执行编辑命令：apply → 入 undo 栈（清空 redo）；返回是否成功。
    /// 失败（命令抛出）→ 文档回滚到执行前（undo 栈不变）。
    bool exec(std::unique_ptr<EditCommand> cmd);

    /// 撤销：undo 栈顶 invert → 移到 redo 栈。无命令可撤 → false。
    bool undo();

    /// 重做：redo 栈顶 apply → 移到 undo 栈。无命令可重做 → false。
    bool redo();

    bool can_undo() const { return !m_undo.empty(); }
    bool can_redo() const { return !m_redo.empty(); }
    std::size_t undo_depth() const { return m_undo.size(); }
    std::size_t redo_depth() const { return m_redo.size(); }

    /// 撤销/重做栈顶命令描述（菜单显示；空 → 空串）。
    std::string undo_label() const;
    std::string redo_label() const;

    /// 选择集（会话状态，不入 undo 栈；doc/01 §5.6）。
    /// load() 时清空；编辑命令不改 selection（note 值标识天然稳定）。
    const Selection& selection() const { return m_selection; }
    Selection& selection_mut() { return m_selection; }
    void set_selection(Selection sel) { m_selection = std::move(sel); }

    // —— 崩溃备份 / 自动保存（2026-09，用户决策：默认关自动保存，以手动保存 + 崩溃备份为主） ——
    // 机制：每次 exec/undo/redo 修改文档后调用 maybe_persist()：
    //   - backup（默认开）：把「修改后」chart 写到 path + ".bak"（崩溃时 .bak = 内存最新，
    //     只丢「备份写后、崩溃前」的极小窗口编辑）；
    //   - autosave（默认关）：把「修改后」chart 直接写回 path（覆盖）。
    // 持久化由 persist_hook 注入（core/edit 不依赖 codec；GUI/CLI 注入 codec 写出）。

    /// 设置持久化钩子：写入 (chart, path)；返回是否成功。默认无钩子（不自动落盘）。
    /// 由调用方（builtins/GUI）注入：用 codec->write + 文件写。
    using PersistHook = std::function<bool(const Chart&, const std::string& path)>;
    void set_persist_hook(PersistHook hook) { m_persist_hook = std::move(hook); }

    /// 崩溃备份开关（默认 true；需 persist_hook + path 才生效）。
    bool backup_enabled() const { return m_backup; }
    void set_backup_enabled(bool on) { m_backup = on; }

    /// 自动保存开关（默认 false；需 persist_hook + path 才生效）。
    bool autosave_enabled() const { return m_autosave; }
    void set_autosave_enabled(bool on) { m_autosave = on; }

private:
    void maybe_persist();  ///< exec/undo/redo 后调用（backup/autosave 落盘）

    std::unique_ptr<Chart> m_chart;
    std::vector<std::unique_ptr<EditCommand>> m_undo;
    std::vector<std::unique_ptr<EditCommand>> m_redo;
    Selection m_selection;
    std::string m_path;
    PersistHook m_persist_hook;
    bool m_backup = true;
    bool m_autosave = false;
};

// —— 具体编辑命令（edit_commands.cpp 实现） ——

/// 放置 note（doc/05 §9「视口点放」→ note.put）。
/// 语义：在 (measure, pos) 的 lane 上放 sample 引用的 note。
/// 逆操作：精确删除该 (measure, pos, lane) 处、匹配 sample 的 note。
///
/// LN/地雷（2026-09，用户确认：BMS 中 LN 与单点文件层无本质区别，识别是前端职责；
/// core 提供 kind 语义扩展性 + 配对辅助）：
/// - kind = Normal（默认）：普通 note；
/// - kind = Landmine：地雷（写出走 D1-D9/E1-E9 通道）；
/// - kind = Ln（`ln_kind=true`）：LN 放置——若该 lane 已有**同 sample 且未配对**的 note
///   （时间上往前最近），自动配对为新尾（互为 ln_pair）；否则作为未配对候选头
///   （后续再放同 lane 同 sample 自动配成尾）。配对关系在模型层，写出由 codec 按
///   LNTYPE 决定通道（LNTYPE 1 → 5x/6x；LNTYPE 2 → 普通通道 + #LNOBJ 尾值）。
class PutNoteCommand : public EditCommand {
public:
    PutNoteCommand(std::uint32_t measure, Rational pos, Lane lane, std::uint32_t sample,
                   bool ln_kind = false, NoteKind kind = NoteKind::Normal,
                   std::uint32_t bgm_line = 0);
    std::string name() const override { return "note.put"; }
    void apply(Chart& chart) override;
    void invert(Chart& chart) override;
    std::string describe() const override;

private:
    std::uint32_t m_measure;
    Rational m_pos;
    Lane m_lane;
    std::uint32_t m_sample;
    bool m_ln_kind;             ///< LN 放置（自动配对）
    NoteKind m_kind;            ///< Normal / Landmine
    std::uint32_t m_bgm_line;   ///< BGM 行序号（放置到指定 ch01 行；非 BGM = 0）
    /// apply 实际插入的 note 下标（invert 用它精确删除；同 pos 冲突分裂等场景）
    std::optional<std::size_t> m_applied_index;
    /// apply 自动配对到的头下标（invert 恢复：解除该头配对）
    std::optional<std::size_t> m_paired_head;
};

/// 移动 note（doc/05 §9「视口拖动」→ note.move，merge_with 合并拖动）。
/// 语义：把 (from_measure, from_pos, lane) 处、匹配 sample 的 note 移到目标位置。
/// 逆操作：移回原位。连续移动同一 note → merge 为一个 undo 步。
///
/// 跨通道（2026-09，M2 自由 2D 拖动）：`to_lane` 可选——传了 = 移动同时改到目标轨道
/// （时间 + 通道一起动）；缺省 nullopt = 纯时间移动（向后兼容，行为与旧版完全一致）。
///
/// LN 处理（2026-08 对齐 BMS 编辑器惯例，用户确认）：
/// - move_ln_pair = false（默认）：LN 当作单个 note 处理——只移动该 note，
///   配对解除（伙伴 ln_pair 清空）。配对断了由解析器自然延续/ lint 告警，
///   编辑器不主动修复（BMS 编辑器常见行为）。
/// - move_ln_pair = true：识别到 LN 则整体头尾一起移动（保持相对位置），配对保留；
///   跨通道时伙伴 lane 同步改为 to_lane（LN 头尾恒同轨道）。
class MoveNoteCommand : public EditCommand {
public:
    MoveNoteCommand(std::uint32_t from_measure, Rational from_pos, Lane lane,
                    std::uint32_t sample, std::uint32_t to_measure, Rational to_pos,
                    bool move_ln_pair = false,
                    std::optional<Lane> to_lane = std::nullopt,
                    std::uint32_t from_bgm_line = 0,
                    std::optional<std::uint32_t> to_bgm_line = std::nullopt);
    std::string name() const override { return "note.move"; }
    void apply(Chart& chart) override;
    void invert(Chart& chart) override;
    bool merge_with(const EditCommand& next) override;
    std::string describe() const override;

private:
    std::uint32_t m_from_measure;
    Rational m_from_pos;
    Lane m_lane;
    std::uint32_t m_sample;
    std::uint32_t m_to_measure;
    Rational m_to_pos;
    bool m_move_ln_pair;
    /// 目标轨道（可选；nullopt = 纯时间移动，不换轨）
    std::optional<Lane> m_to_lane;
    /// 源 BGM 行序号（消歧：同 (measure,pos,lane,sample) 的 Bgm note 靠它定位）
    std::uint32_t m_from_bgm_line = 0;
    /// 目标 BGM 行序号（nullopt = 不指定：按目标小节 ch01 行数自动分配；非 BGM 目标忽略）
    std::optional<std::uint32_t> m_to_bgm_line;
    /// apply 快照（invert 恢复；找不到 note 时为空）
    std::optional<Event<Note>> m_moved;
    std::optional<Event<Note>> m_partner;
    /// 单 note 模式 apply 后移动端重连的新伙伴（m_paired_at = 移动端新下标，m_paired_with = 伙伴）；
    /// invert 时用于清理该临时配对（否则新伙伴会悬挂指向已移回主 note 的旧下标）。
    std::optional<std::size_t> m_paired_at;
    std::optional<std::size_t> m_paired_with;
    Rational m_last_delta{0, 1};
};

/// 删除 note（doc/05 §9「选择集删除」→ note.delete）。
/// 语义：删除 (measure, pos, lane) 处匹配 sample 的 note。
/// 逆操作：精确恢复该 note（含原容器位置近似——重插同 (measure,pos,lane,sample)）。
class DeleteNoteCommand : public EditCommand {
public:
    DeleteNoteCommand(std::uint32_t measure, Rational pos, Lane lane, std::uint32_t sample,
                      std::uint32_t bgm_line = 0);
    std::string name() const override { return "note.delete"; }
    void apply(Chart& chart) override;
    void invert(Chart& chart) override;
    std::string describe() const override;

private:
    std::uint32_t m_measure;
    Rational m_pos;
    Lane m_lane;
    std::uint32_t m_sample;
    std::uint32_t m_bgm_line;  ///< BGM 行序号（消歧；非 Bgm = 0）
    /// apply 删除的 note 的完整拷贝（invert 恢复；含 ln_pair 等）
    std::optional<Event<Note>> m_removed;
    /// 原容器位置（invert 尽量原位恢复；同 (measure,pos,lane) 冲突时退化为追加）
    std::size_t m_removed_index = 0;
    /// 原配对伙伴快照（apply 清空伙伴 ln_pair 前保存；invert 按值重定位恢复互指）
    std::optional<Event<Note>> m_partner;
};

/// 量化 note 位置（doc/01 §D「量化(quantize)」→ note.quantize）。
/// 语义：把 (measure, pos, lane, sample) 处匹配 note 的 pos 吸附到 snapNum/snapDen 网格
/// （目标 pos = k*snapNum/snapDen，k = round(pos*snapDen/snapNum)）。
/// 只改 pos，**不动 lane/sample/ln_pair**（量化不该拆 LN——头尾各自吸附到网格）。
/// 逆操作：精确恢复原 pos。批量 = CompositeCommand 包装多个（一个 undo 步）。
class QuantizeNoteCommand : public EditCommand {
public:
    QuantizeNoteCommand(std::uint32_t measure, Rational pos, Lane lane, std::uint32_t sample,
                        std::int64_t snap_num, std::int64_t snap_den);
    std::string name() const override { return "note.quantize"; }
    void apply(Chart& chart) override;
    void invert(Chart& chart) override;
    std::string describe() const override;

private:
    std::uint32_t m_measure;
    Rational m_pos;    ///< 原始 pos（命令目标 = 量化后；invert 恢复此值）
    Lane m_lane;
    std::uint32_t m_sample;
    std::int64_t m_snap_num;
    std::int64_t m_snap_den;
    /// apply 实际执行时是否发生了改变（pos 已在网格上 → false，invert 无操作）
    bool m_changed = false;
    /// apply 后的新 pos（invert 恢复用）
    Rational m_new_pos;
};

/// 变换选中 note（doc/01 §D「镜像/旋转（LR2 式 mirror/random）」→ note.transform）。
/// 语义：对选中 note 做轨道变换——mirror = 左右镜像（key i ↔ key max-i+1，scratch/pedal
/// 不变）；rotate = key 轨循环移位（rotate=1 → key1→key2…key7→key1；负数反向）。
/// 只改 lane，**不动 pos/sample/ln_pair**（LN 头尾同轨，变换后仍同轨）。
/// 逆操作：反向变换恢复原轨道。批量 = CompositeCommand 包装多个（一个 undo 步）。
class TransformNoteCommand : public EditCommand {
public:
    TransformNoteCommand(std::uint32_t measure, Rational pos, Lane lane, std::uint32_t sample,
                         bool mirror, int rotate);
    std::string name() const override { return "note.transform"; }
    void apply(Chart& chart) override;
    void invert(Chart& chart) override;
    std::string describe() const override;

private:
    std::uint32_t m_measure;
    Rational m_pos;
    Lane m_lane;
    std::uint32_t m_sample;
    bool m_mirror;
    int m_rotate;
    /// apply 后的新 lane（invert 恢复）
    std::optional<Lane> m_new_lane;
};

/// 编辑头部字段（doc/05 §107「元信息表单」→ meta.edit）。
/// 语义：设置 Chart.meta[key] = value。**value 为空串 = 删除该字段**（元信息表单清空输入）。
/// 逆操作：恢复原值（原不存在 → 移除；原存在 → 恢复旧值）。
/// 批量 = CompositeCommand 包多个（一个 undo 步）。键名统一大写（meta 键规范）。
class MetaEditCommand : public EditCommand {
public:
    MetaEditCommand(std::string key, std::string value);
    std::string name() const override { return "meta.edit"; }
    void apply(Chart& chart) override;
    void invert(Chart& chart) override;
    std::string describe() const override;

private:
    std::string m_key;    ///< 大写键名
    std::string m_value;  ///< 新值（空 = 删除）
    /// apply 前是否存在（invert 分支：恢复旧值 / 移除）
    bool m_existed = false;
    /// 原值（m_existed 时有效）
    std::string m_old_value;
    /// apply 实际是否改变（无变化 → invert 无操作）
    bool m_changed = false;
};

}  // namespace beatbench::edit
