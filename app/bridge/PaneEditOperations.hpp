// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include <QObject>
#include <QPointer>
#include <QVariantList>
#include <QVariantMap>
#include <cstdint>

#include "PaneEditContext.hpp"

namespace beatbench::app {

// 仅操作构造时指定的窗格；不访问全局活动主谱、Dock、系统剪贴板或音频。
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
    // 仅返回原协议文本；系统剪贴板和弹窗归 UI。默认不允许定义行整段粘贴。
    // settings 的 allowDefinitions / legacyBase36CaseFold 独立默认 false；
    // 仅旧 A raw 入口显式开启兼容。普通 paste 不能跳过 Base62 确认。
    Q_INVOKABLE QVariantMap copySelection();
    Q_INVOKABLE QVariantMap paste(const QString& text, int measure,
                                  const QVariantMap& settings = {});
    Q_INVOKABLE QVariantMap pasteCaptured(const QVariantMap& token, const QString& text,
                                          int measure, const QVariantMap& settings = {});
    // UI 冻结 obj/settings 与 context.capture()，显示/接受共用目标事件拍位的 BPM。
    Q_INVOKABLE QVariantMap metaEditInfo(const QVariantMap& object, const QVariantMap& settings);
    Q_INVOKABLE QVariantMap editMetaCaptured(const QVariantMap& token, const QVariantMap& object,
                                             const QString& text, const QVariantMap& settings);
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
    // Captured gesture conversion shares all policies with the local move path.
    QVariantMap moveSelectionCaptured(const QVariantMap& token, double deltaF,
                                      const QVariantMap& targetLane, const QVariantMap& sourceLane,
                                      int sourceBgmLine, const QVariantList& columns,
                                      const QVariantMap& settings);
    Q_INVOKABLE QVariantMap moveMeta(const QString& kind, const QVariantMap& object, double deltaF,
                                     const QVariantMap& targetLane, const QVariantMap& settings);
   signals:
    void message(const QString& text);

   private:
    QVariantMap fail(const QString& code, const QString& text);
    QVariantMap run(const QString& command, const QVariantMap& args, const QString& text);
    QVariantMap placeLnObj(const QVariantMap& hit, const QVariantMap& settings);
    QVariantMap pasteImpl(const QVariantMap* token, const QString& text, int measure,
                          const QVariantMap& settings);
    QVariantMap moveSelectionImpl(const QVariantMap* token, double deltaF,
                                  const QVariantMap& targetLane, const QVariantMap& sourceLane,
                                  int sourceBgmLine, const QVariantList& columns,
                                  const QVariantMap& settings);
    bool pendingIsCurrent() const;
    void rememberPending(const QVariantMap& ref);
    QPointer<PaneEditContext> m_context;
    QString m_lnSession;
    std::uint64_t m_lnRevision = 0;
};
}  // namespace beatbench::app
