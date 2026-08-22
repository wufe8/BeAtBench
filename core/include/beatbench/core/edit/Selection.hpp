// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include <cstdint>
#include <optional>
#include <set>
#include <vector>

#include "beatbench/core/Lane.hpp"
#include "beatbench/core/Rational.hpp"

namespace beatbench::edit {

/// note 值标识（选择集/剪贴板元素）：(measure, pos, lane, sample)。
/// 用值而非容器下标：编辑（插入/删除/移动）会改变下标，值标识天然鲁棒——
/// undo 后 note 回来了，selection 仍指向它（01 §5.6：选择是会话状态，不入 undo）。
struct NoteRef {
    std::uint32_t measure = 0;
    Rational pos;
    Lane lane;
    std::uint32_t sample = 0;

    friend bool operator<(const NoteRef& a, const NoteRef& b) {
        if (a.measure != b.measure) return a.measure < b.measure;
        if (a.pos != b.pos) return a.pos < b.pos;
        if (a.lane != b.lane) return a.lane < b.lane;
        return a.sample < b.sample;
    }
    friend bool operator==(const NoteRef& a, const NoteRef& b) {
        return a.measure == b.measure && a.pos == b.pos && a.lane == b.lane &&
               a.sample == b.sample;
    }
};

/// 选择集：无序 note 集合（有序容器便于确定性输出/对比）。
/// 会话状态，不入 undo 栈（doc/01 §5.6）；由 EditorSession 持有。
class Selection {
public:
    void add(NoteRef ref) { m_refs.insert(std::move(ref)); }
    void remove(const NoteRef& ref) { m_refs.erase(ref); }
    void clear() { m_refs.clear(); }
    bool contains(const NoteRef& ref) const { return m_refs.count(ref) != 0; }
    bool empty() const { return m_refs.empty(); }
    std::size_t size() const { return m_refs.size(); }

    const std::set<NoteRef>& refs() const { return m_refs; }

    /// 框选：矩形范围内（measure 区间 + lane 集合 + pos 区间）的 note 集合。
    /// 由调用方（GUI）把可视区域映射为这些参数；core 只做集合运算。
    /// lanes 为空 = 全部 lane；pos 区间 [pos_lo, pos_hi]（含端点）。
    void add_rect(std::uint32_t measure_lo, std::uint32_t measure_hi,
                  const std::vector<Lane>& lanes, Rational pos_lo, Rational pos_hi,
                  const std::vector<NoteRef>& candidates);

private:
    std::set<NoteRef> m_refs;
};

}  // namespace beatbench::edit
