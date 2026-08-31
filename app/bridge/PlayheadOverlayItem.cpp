// SPDX-License-Identifier: GPL-3.0-only
// PlayheadOverlayItem 实现（见 hpp 注释）：只画红线 + A/B 虚线（<1ms），
// 播放时钟更新不触发 ChartViewItem 全量重绘（30fps → 60fps+ 性能修复）。
#include "bridge/PlayheadOverlayItem.hpp"

#include <QPainter>
#include <QPainterPath>
#include <QPolygonF>

#include "bridge/ChartSession.hpp"
#include "beatbench/core/timing/TimingEngine.hpp"

namespace beatbench::app {

namespace {
const QColor kPlayheadColor(QStringLiteral("#e53935"));  // 红（用户拍板）
const QColor kLoopAColor(QStringLiteral("#4caf50"));     // 绿 A
const QColor kLoopBColor(QStringLiteral("#ff9800"));     // 橙 B
}  // namespace

PlayheadOverlayItem::PlayheadOverlayItem(QQuickItem* parent) : QQuickPaintedItem(parent) {
    // 叠层：透明、不拦截鼠标（ChartViewItem 接收输入；本层只显示）
    setFlag(ItemHasContents, true);
    setAcceptedMouseButtons(Qt::NoButton);
}

void PlayheadOverlayItem::setSession(QObject* session) {
    if (m_session == session) return;
    m_session = session;
    emit sessionChanged();
    update();
}

void PlayheadOverlayItem::setMeasureHeight(qreal v) {
    if (qFuzzyCompare(m_measureHeight, v)) return;
    m_measureHeight = v;
    emit measureHeightChanged();
    update();
}

void PlayheadOverlayItem::setScrollY(qreal v) {
    if (qFuzzyCompare(m_scrollY, v)) return;
    m_scrollY = v;
    emit scrollYChanged();
    update();
}

void PlayheadOverlayItem::setContentHeight(qreal v) {
    if (qFuzzyCompare(m_contentHeight, v)) return;
    m_contentHeight = v;
    emit contentHeightChanged();
    update();
}

void PlayheadOverlayItem::setTopHigh(bool v) {
    if (m_topHigh == v) return;
    m_topHigh = v;
    emit topHighChanged();
    update();
}

void PlayheadOverlayItem::setPlayheadSec(double v) {
    if (qFuzzyCompare(m_playheadSec, v)) return;
    m_playheadSec = v;
    emit playheadSecChanged();
    update();
}

void PlayheadOverlayItem::setLoopASec(double v) {
    if (qFuzzyCompare(m_loopASec, v)) return;
    m_loopASec = v;
    emit loopASecChanged();
    update();
}

void PlayheadOverlayItem::setLoopBSec(double v) {
    if (qFuzzyCompare(m_loopBSec, v)) return;
    m_loopBSec = v;
    emit loopBSecChanged();
    update();
}

void PlayheadOverlayItem::setRulerWidth(qreal v) {
    if (qFuzzyCompare(m_rulerWidth, v)) return;
    m_rulerWidth = v;
    emit rulerWidthChanged();
    update();
}

ChartSession* PlayheadOverlayItem::sessionObj() const {
    return qobject_cast<ChartSession*>(m_session);
}

qreal PlayheadOverlayItem::yForSec(double sec) const {
    const ChartSession* cs = sessionObj();
    if (!cs || !cs->timing() || sec < 0.0) return -1e9;
    const auto pos = cs->timing()->position_at(static_cast<std::int64_t>(sec * 1e6));
    if (!pos) return -1e9;
    const double mf = static_cast<double>(pos->measure) +
                      static_cast<double>(pos->pos.num) / static_cast<double>(pos->pos.den);
    const qreal c = static_cast<qreal>(mf) * m_measureHeight;
    return (m_topHigh ? (m_contentHeight - c) : c) - m_scrollY;
}

void PlayheadOverlayItem::paint(QPainter* p) {
    const qreal w = width();
    const qreal h = height();
    if (w <= 0 || h <= 0) return;

    // A/B 循环标记（虚线 + 左侧标签；先画（在红线下层））
    {
        p->setFont(QFont(QStringLiteral("Consolas"), 10));
        const auto drawMark = [&](double sec, const QColor& color, const QString& label) {
            const qreal py = yForSec(sec);
            if (py < -1.0 || py > h + 1.0) return;
            QPen pen(color, 1.0, Qt::DashLine);
            p->setPen(pen);
            p->drawLine(QPointF(0.0, py), QPointF(w, py));
            const QRectF lb(0.0, py - 9.0, m_rulerWidth, 18.0);
            p->fillRect(lb, QColor(0, 0, 0, 160));
            p->setPen(color);
            p->drawText(lb, Qt::AlignCenter, label);
        };
        drawMark(m_loopASec, kLoopAColor, QStringLiteral("A"));
        drawMark(m_loopBSec, kLoopBColor, QStringLiteral("B"));
    }

    // 播放头红线（当前时间点；贯穿编辑区）
    if (m_playheadSec >= 0.0) {
        const qreal py = yForSec(m_playheadSec);
        if (py >= -1.0 && py <= h + 1.0) {
            p->setPen(QPen(kPlayheadColor, 2.0));
            p->drawLine(QPointF(0.0, py), QPointF(w, py));
            // 左侧三角（方向指示）
            QPolygonF tri;
            tri << QPointF(0.0, py - 8.0) << QPointF(8.0, py) << QPointF(0.0, py + 8.0);
            p->setBrush(kPlayheadColor);
            p->setPen(Qt::NoPen);
            p->drawPolygon(tri);
        }
    }
}

}  // namespace beatbench::app
