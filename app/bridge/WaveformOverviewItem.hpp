// SPDX-License-Identifier: GPL-3.0-only
// 波形总览条（M4.3c）：全曲 min/max 波形 + 视口窗指示 + 点击/拖动跳转。
//
// ⚠️ 2026-09 用户：波形图放**右侧**（垂直条，最右缘 vbar 左侧）——底部已属
// 水平滚动条（横向看不同通道），垂直条不与之争空间。支持两方向：
//   orientation 0 = 水平（底部条，历史默认）；1 = 垂直（右侧条，当前布局）。
// 垂直条时间轴方向与视口一致（topHigh 感知：topHigh=true 时 0s 在底部，
// 与 ChartView 的「顶部=高小节」同向——窗指示与机手滚动直觉一致）。
// 数据源 = ChartSession 最近渲染的 WaveformPyramid（Space 渲染后自动出现；
// 未渲染 → 占位提示）。点击/拖动 → seekRequested(seconds) → ChartView.scrollToTime。
// 皮肤边界：QPainter 自绘 + Theme token（wave/border/textFaint…），默认皮肤组件库成员。
#pragma once

#include <QQuickPaintedItem>
#include <QtQml/qqmlregistration.h>

namespace beatbench::app {

class ChartSession;
class ThemeManager;

class WaveformOverviewItem : public QQuickPaintedItem {
    Q_OBJECT
    QML_ELEMENT

    Q_PROPERTY(QObject* session READ session WRITE setSession NOTIFY sessionChanged)
    Q_PROPERTY(QObject* theme READ theme WRITE setTheme NOTIFY themeChanged)
    /// 方向：0 = 水平（底部条）；1 = 垂直（右侧条，2026-09 用户布局）。
    Q_PROPERTY(int orientation READ orientation WRITE setOrientation NOTIFY orientationChanged)
    // —— 视口状态（ChartView.qml 绑定传入；换算可见时间窗） ——
    Q_PROPERTY(qreal measureHeight READ measureHeight WRITE setMeasureHeight NOTIFY measureHeightChanged)
    Q_PROPERTY(qreal scrollY READ scrollY WRITE setScrollY NOTIFY scrollYChanged)
    Q_PROPERTY(qreal contentHeight READ contentHeight WRITE setContentHeight NOTIFY contentHeightChanged)
    Q_PROPERTY(bool topHigh READ topHigh WRITE setTopHigh NOTIFY topHighChanged)
    Q_PROPERTY(qreal viewportHeight READ viewportHeight WRITE setViewportHeight NOTIFY viewportHeightChanged)
    /// 开头留白小节数（M5.2；数值方向与 ChartView 同源）。内容坐标换算拍位须扣它，
    /// 否则视口窗整体往未来偏移（用视口顶/底基准都不准；2026-09 用户）。
    Q_PROPERTY(qreal leadMeasures READ leadMeasures WRITE setLeadMeasures NOTIFY leadMeasuresChanged)

public:
    explicit WaveformOverviewItem(QQuickItem* parent = nullptr);

    void paint(QPainter* painter) override;

    QObject* session() const { return m_session; }
    void setSession(QObject* session);
    QObject* theme() const { return m_theme; }
    void setTheme(QObject* theme);
    int orientation() const { return m_orientation; }
    void setOrientation(int v);
    qreal measureHeight() const { return m_measureHeight; }
    void setMeasureHeight(qreal v);
    qreal scrollY() const { return m_scrollY; }
    void setScrollY(qreal v);
    qreal contentHeight() const { return m_contentHeight; }
    void setContentHeight(qreal v);
    bool topHigh() const { return m_topHigh; }
    void setTopHigh(bool v);
    qreal viewportHeight() const { return m_viewportHeight; }
    void setViewportHeight(qreal v);
    qreal leadMeasures() const { return m_leadMeasures; }
    void setLeadMeasures(qreal v);

    /// 视口窗（秒，可能 topHigh 逆序 → 归一 [t0, t1]）。
    struct ViewWindow { double t0 = 0.0, t1 = 0.0; };

signals:
    void sessionChanged();
    void themeChanged();
    void orientationChanged();
    void measureHeightChanged();
    void scrollYChanged();
    void contentHeightChanged();
    void topHighChanged();
    void viewportHeightChanged();
    void leadMeasuresChanged();
    /// 点击/拖动总览条 → 目标时间（秒）→ QML 滚动视口（ChartView.scrollToTime）。
    void seekRequested(double seconds);

protected:
    void mousePressEvent(QMouseEvent* event) override;
    void mouseMoveEvent(QMouseEvent* event) override;
    void mouseReleaseEvent(QMouseEvent* event) override;

private:
    ChartSession* sessionObj() const;
    ThemeManager* themeObj() const;
    void requestSeek(qreal pos);
    /// 视口窗（秒；含小数小节 → timeAtMeasure 换算；无 timing → {0,0}）。
    ViewWindow viewWindow() const;
    static QString formatTime(double sec);

    QObject* m_session = nullptr;
    QObject* m_theme = nullptr;
    int m_orientation = 1;  // 默认垂直（当前布局=右侧条）
    qreal m_measureHeight = 96.0;
    qreal m_scrollY = 0.0;
    qreal m_contentHeight = 0.0;
    bool m_topHigh = true;
    qreal m_viewportHeight = 0.0;
    qreal m_leadMeasures = 0.0;
};

}  // namespace beatbench::app
