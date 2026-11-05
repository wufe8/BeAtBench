// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include <QObject>
#include <QPointer>
#include <QVariantList>
#include <QVariantMap>
#include <cstdint>

#include "PaneEditContext.hpp"

namespace beatbench::app {

// 仅操作构造时指定的窗格；不访问全局活动主谱、Dock、剪贴板或音频。
// settings: {tool,sampleText,bmpText,bpmValue,stopValue,snapNum:1,snapDen:16}。
// hit/targetLane 沿用 ChartViewItem::hitTest/laneAtX；sourceLane 为 {player,kind,index}。
// columns 必须是完整显示顺序（包含 meta/BGA 占位），每项为 laneAtX 同形元数据；
// BGM 必须保留 sub_line。不能用只认演出列的 laneAtColumn 输出代替。
class PaneEditOperations : public QObject {
    Q_OBJECT
    Q_PROPERTY(PaneEditContext* context READ context CONSTANT)
   public:
    explicit PaneEditOperations(PaneEditContext* context, QObject* parent = nullptr);
    PaneEditContext* context() const { return m_context.data(); }
    Q_INVOKABLE QVariantMap place(const QVariantMap& hit, const QVariantMap& settings);
    Q_INVOKABLE QVariantMap selectNote(const QVariantMap& ref, bool ctrl,
                                       bool lnSelectMode = false);
    Q_INVOKABLE QVariantMap selectNotes(const QVariantList& refs);
    Q_INVOKABLE QVariantMap selectMeta(const QVariantMap& object, bool ctrl);
    Q_INVOKABLE QVariantMap clearSelection();
    Q_INVOKABLE QVariantMap deleteNote(const QVariantMap& ref);
    Q_INVOKABLE QVariantMap deleteMeta(const QVariantMap& object);
    Q_INVOKABLE QVariantMap deleteSelection();
    Q_INVOKABLE QVariantMap cancelPendingLn();
    Q_INVOKABLE QVariantMap undo();
    Q_INVOKABLE QVariantMap redo();
    Q_INVOKABLE QVariantMap toggleLn();
    Q_INVOKABLE QVariantMap quantize(const QVariantMap& settings);
    Q_INVOKABLE QVariantMap transform(bool mirror, int rotate);
    Q_INVOKABLE QVariantMap moveSelection(double deltaF, const QVariantMap& targetLane,
                                          const QVariantMap& sourceLane, int sourceBgmLine,
                                          const QVariantList& columns, const QVariantMap& settings);
    Q_INVOKABLE QVariantMap moveMeta(const QString& kind, const QVariantMap& object, double deltaF,
                                     const QVariantMap& targetLane, const QVariantMap& settings);
   signals:
    void message(const QString& text);

   private:
    QVariantMap fail(const QString& code, const QString& text);
    QVariantMap run(const QString& command, const QVariantMap& args, const QString& text);
    QVariantMap placeLnObj(const QVariantMap& hit, const QVariantMap& settings);
    bool pendingIsCurrent() const;
    void rememberPending(const QVariantMap& ref);
    QPointer<PaneEditContext> m_context;
    QString m_lnSession;
    std::uint64_t m_lnRevision = 0;
};
}  // namespace beatbench::app
