// SPDX-License-Identifier: GPL-3.0-only
// 键盘快速摆音（M3 体验项，2026-10 用户）：选中已有 note 后按轨键，每次把下一个
// 选中 note 移到目标游玩轨。只改 lane，不改 measure/pos/sample；不新建、不复制。
//
// 设计边界（doc/core-model.md 四、/ 项目 issue #46）：
// - 选中集按（拍位，原轨）快照排序；移动后不重排剩余队列。
// - LN 头尾视为一个逻辑对象：选中任一端时一次按键整体移动，保持配对。
// - 目标位置已有其它 note → 阻止且不消耗；目标轨在当前视口隐藏 → 同样阻止。
// - 每次按键 = 一个 MoveNoteCommand = 一个 undo 步。
// - 键位预设（单手换边 / 双手固定）在 C++ 表内；QSettings 只持久化预设选择。
//   单手默认 `B S X D C F V` + `A` 皿：`B` 避开「工具键预留位 Z」（doc/ui-design.md），
//   其余沿用 LR2 演奏位；PMS9K 变体首键同步为 `B`。
// - 事件过滤器处理轨键；文本焦点、Ctrl/Alt/Meta、自动重复一律让行。
#pragma once

#include <QObject>
#include <QStringList>
#include <QVariantList>
#include <optional>
#include <vector>

#include "beatbench/core/Chart.hpp"
#include "beatbench/core/edit/LanePlan.hpp"
#include "beatbench/core/edit/Selection.hpp"

class QEvent;

namespace beatbench::app {

class ChartSession;

class QuickPlaceController : public QObject {
    Q_OBJECT
    Q_PROPERTY(int preset READ preset WRITE setPreset NOTIFY presetChanged)
    Q_PROPERTY(int side READ side NOTIFY sideChanged)
    Q_PROPERTY(int pendingCount READ pendingCount NOTIFY pendingCountChanged)
    Q_PROPERTY(bool hasPending READ hasPending NOTIFY pendingCountChanged)

   public:
    explicit QuickPlaceController(QObject* parent = nullptr);

    /// 数据源（不拥有）；main.cpp 在 ChartSession 创建后注入。
    void setChartSession(ChartSession* session);

    /// QML 门控：编辑页 + 已打开谱面 / 文本输入焦点。
    Q_INVOKABLE void setEnabled(bool enabled);
    Q_INVOKABLE void setInputBlocked(bool blocked);
    /// 测试隔离：关闭 QSettings 写入，避免单测污染用户偏好。
    void setPersistEnabled(bool enabled) { m_persist = enabled; }

    /// 当前活动视口的隐藏列 key（#48 接入；单视口时为空）。
    /// 格式：lane:<player>:<kind>:<index>（如 lane:0:key:1、lane:1:scratch:0）。
    Q_INVOKABLE void setHiddenLanes(const QStringList& keys);

    /// QML 在 selectionRefs 变化时调用。内部移动回写的 selection 通过签名识别，
    /// 不会重置队列指针。
    Q_INVOKABLE void setSelection(const QVariantList& refs);
    Q_INVOKABLE void cancel();

    int preset() const { return m_preset; }
    Q_INVOKABLE void setPreset(int preset);
    int side() const { return m_side; }
    int pendingCount() const { return static_cast<int>(m_queue.size()) - m_index; }
    bool hasPending() const { return m_index < m_queue.size(); }

    bool eventFilter(QObject* watched, QEvent* event) override;

   signals:
    void presetChanged();
    void sideChanged();
    void pendingCountChanged();
    /// 内部移动后回写完整选中集（含 LN 伙伴）；QML 直接赋给 window.selectionRefs。
    void selectionUpdated(const QVariantList& refs);
    void statusMessage(const QString& text);
    /// 文档已改（控制器已调用 chartSession.refresh；QML 只需刷新 lint 等派生视图）。
    void edited();

   private:
    // 队列元素直接复用 edit::NoteGroup（LanePlan 地基的同构类型），
    // 不再手搓第二份 {a, b, ln}。
    void rebuildFromSelection(const std::vector<edit::NoteRef>& refs);
    void emitSelection();
    void resetQueue();
    void placeLane(const beatbench::Lane& lane);
    std::optional<beatbench::Lane> laneForKey(int key, int effectiveSide) const;
    QString mode() const;
    bool isPms() const;
    bool isDoublePlay() const;
    std::vector<edit::NoteRef> readSelection(const QVariantList& refs) const;
    QVariantList toVariant(const std::vector<edit::NoteRef>& refs) const;

    ChartSession* m_session = nullptr;
    bool m_enabled = false;
    bool m_inputBlocked = false;
    int m_preset = 0;  ///< 0 = 单手换边；1 = 双手固定
    int m_side = 0;    ///< 0 = 1P；1 = 2P（仅单手 DP/Battle）
    QStringList m_hiddenLanes;
    std::vector<edit::NoteRef> m_selection;
    std::vector<edit::NoteGroup> m_queue;
    std::size_t m_index = 0;
    /// 自身回写保护：selectionUpdated 同步环内的 setSelection 直接忽略。
    bool m_muting = false;
    bool m_persist = true;
};

}  // namespace beatbench::app
