// SPDX-License-Identifier: GPL-3.0-only
// 播放头叠层（M5.2 性能拆层，2026-09）：红线（当前时间点）+ A/B 循环标记线的
// **独立绘制层**——叠在 ChartViewItem 之上，只画线（<1ms）。
//
// 背景：播放头线原先画在 ChartViewItem 里 → 每次 playheadSec 更新（20Hz 播放时钟）
// 触发**全量重绘**大画布（波形 + 数百 note + 网格 + 标尺 ≈ 13ms/帧）→ 30fps 天花板
// （--perf-log 实测 2026-09）。拆分后：播放时钟变化只重绘本层，ChartViewItem 仅在
// 内容/滚动/缩放时重绘（低频）→ 帧率回 60fps+。
//
// 实现（零 Qt 依赖面）：QQuickItem（非 PaintedItem——用 render 到 QSGNode 或
// QQuickPaintedItem 均可；选 QQuickPaintedItem 与 ChartViewItem/WaveformOverviewItem
// 同构，paint() 画线）。属性 = 秒 → 屏幕 y 所需全部换算状态（session/measureHeight/
// scrollY/contentHeight/topHigh），QML 绑定 ChartView 的 view 状态。
#pragma once

#include <QQuickPaintedItem>
#include <QtQml/qqmlregistration.h>

namespace beatbench::app {

class ChartSession;

class PlayheadOverlayItem : public QQuickPaintedItem {
    Q_OBJECT
    QML_ELEMENT

    Q_PROPERTY(QObject* session READ session WRITE setSession NOTIFY sessionChanged)
    Q_PROPERTY(qreal measureHeight READ measureHeight WRITE setMeasureHeight NOTIFY measureHeightChanged)
    Q_PROPERTY(qreal scrollY READ scrollY WRITE setScrollY NOTIFY scrollYChanged)
    Q_PROPERTY(qreal contentHeight READ contentHeight WRITE setContentHeight NOTIFY contentHeightChanged)
    Q_PROPERTY(bool topHigh READ topHigh WRITE setTopHigh NOTIFY topHighChanged)
    /// A/B 循环标记（秒；-1 = 未设）。绿 A / 橙 B 虚线 + 左侧标签（**内容锚定**，随滚动）。
    Q_PROPERTY(double loopASec READ loopASec WRITE setLoopASec NOTIFY loopASecChanged)
    Q_PROPERTY(double loopBSec READ loopBSec WRITE setLoopBSec NOTIFY loopBSecChanged)
    Q_PROPERTY(qreal rulerWidth READ rulerWidth WRITE setRulerWidth NOTIFY rulerWidthChanged)
    /// 开头纯留白小节数（与 ChartView 数据同源）：A/B 标记换算 y 时须扣留白，
    /// 否则标记比实际值早 1-2 小节（2026-09 用户）。
    Q_PROPERTY(qreal leadMeasures READ leadMeasures WRITE setLeadMeasures NOTIFY leadMeasuresChanged)

public:
    explicit PlayheadOverlayItem(QQuickItem* parent = nullptr);

    void paint(QPainter* painter) override;

    QObject* session() const { return m_session; }
    void setSession(QObject* session);
    qreal measureHeight() const { return m_measureHeight; }
    void setMeasureHeight(qreal v);
    qreal scrollY() const { return m_scrollY; }
    void setScrollY(qreal v);
    qreal contentHeight() const { return m_contentHeight; }
    void setContentHeight(qreal v);
    bool topHigh() const { return m_topHigh; }
    void setTopHigh(bool v);
    double loopASec() const { return m_loopASec; }
    void setLoopASec(double v);
    double loopBSec() const { return m_loopBSec; }
    void setLoopBSec(double v);
    qreal rulerWidth() const { return m_rulerWidth; }
    void setRulerWidth(qreal v);
    qreal leadMeasures() const { return m_leadMeasures; }
    void setLeadMeasures(qreal v);

signals:
    void sessionChanged();
    void measureHeightChanged();
    void scrollYChanged();
    void contentHeightChanged();
    void topHighChanged();
    void loopASecChanged();
    void loopBSecChanged();
    void rulerWidthChanged();
    void leadMeasuresChanged();

private:
    ChartSession* sessionObj() const;
    /// 秒 → 屏幕 y（timing position_at + yOf 语义；A/B 标记用；无 timing/无效 → -1e9）。
    qreal yForSec(double sec) const;

    QObject* m_session = nullptr;
    qreal m_measureHeight = 96.0;
    qreal m_scrollY = 0.0;
    qreal m_contentHeight = 0.0;
    bool m_topHigh = true;
    double m_loopASec = -1.0;
    double m_loopBSec = -1.0;
    qreal m_rulerWidth = 56.0;
    qreal m_leadMeasures = 0.0;
};

}  // namespace beatbench::app
