// SPDX-License-Identifier: GPL-3.0-only
#pragma once

#include <QObject>
#include <QPointF>
#include <QPointer>
#include <QQuickItem>
#include <QVariantMap>
#include <array>
#include <vector>

#include "ChartViewItem.hpp"
#include "NoteMovePlan.hpp"
#include "PaneEditOperations.hpp"

namespace beatbench::app {

// 一个 mouse grab 的同步业务主机；坐标入参均为 source viewport local（非 surface local）。
// surface 仅作手势身份和 editable/moveMode/snap 属性来源，不拥有文档。
// 无 system drag/drop、focus/active/Dock/audio 副作用；QML 必须将 managed latch 保留到 release，
// 即使 active 已因隐藏/失效变为 false，也不得回退旧 release commit。
class PaneDragController : public QObject {
    Q_OBJECT
    Q_PROPERTY(bool active READ active NOTIFY activeChanged)
   public:
    explicit PaneDragController(QObject* parent = nullptr);
    ~PaneDragController() override;
    Q_INVOKABLE bool registerPane(PaneEditContext* context, QQuickItem* surface,
                                  ChartViewItem* viewport, PaneEditOperations* operations);
    bool active() const { return m_active; }
    Q_INVOKABLE bool begin(QQuickItem* sourceSurface, const QPointF& pressPoint,
                           const QPointF& currentPoint, const QVariantMap& pressedRef,
                           const QVariantList& selection);
    Q_INVOKABLE QVariantMap update(QQuickItem* sourceSurface, const QPointF& point);
    Q_INVOKABLE QVariantMap finish(QQuickItem* sourceSurface, const QPointF& point);
    Q_INVOKABLE void cancel(QQuickItem* sourceSurface, const QString& reason);
    Q_INVOKABLE void cancelActive(const QString& reason);
   signals:
    void activeChanged();
    void message(const QString& text);
    void selectionCommitted(PaneEditContext* source, PaneEditContext* target,
                            const QVariantList& selection);

   private:
    struct Endpoint {
        QPointer<PaneEditContext> context;
        QPointer<QQuickItem> surface;
        QPointer<ChartViewItem> viewport;
        QPointer<PaneEditOperations> operations;
        std::vector<QMetaObject::Connection> connections;
    };
    bool endpointReady(int index) const;
    bool sourceCurrent() const;
    void checkCurrent();
    void endpointChanged(int index);
    void clearPreviews();
    QVariantMap sample(const QPointF& sourcePoint);
    QVariantMap error(const QString& code, const QString& text, bool handled = true) const;
    std::array<Endpoint, 2> m_endpoints;
    int m_source = -1;
    int m_target = -1;
    QPointer<QQuickItem> m_owner;
    QString m_sessionId;
    QVariantMap m_token;
    NoteMoveSource m_frozen;
    NoteMoveDrop m_drop;
    NoteMovePlan m_plan;
    QPointF m_pressScene;
    QPointF m_lastScene;
    double m_pressF = 0;
    bool m_moveMode = false;
    bool m_active = false;
    bool m_committing = false;
    bool m_finishing = false;
    bool m_sampling = false;
    bool m_dragged = false;
    bool m_conversion = false;
    QString m_terminalReason;
};
}  // namespace beatbench::app
