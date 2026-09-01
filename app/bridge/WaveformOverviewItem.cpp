// SPDX-License-Identifier: GPL-3.0-only
// WaveformOverviewItem 实现：总览条绘制 + 点击/拖动 seek（见头文件注释）。
// 方向：0 = 水平（底部条）；1 = 垂直（右侧条，当前布局——时间轴与视口同向）。
#include "bridge/WaveformOverviewItem.hpp"

#include <QMouseEvent>
#include <QPainter>
#include <QCursor>

#include <algorithm>
#include <cmath>

#include "bridge/ChartSession.hpp"
#include "bridge/ThemeManager.hpp"
#include "beatbench/audio/WaveformPyramid.hpp"

namespace beatbench::app {

namespace {
constexpr qreal kSeekThresholdPx = 2.0;  // 点击 vs 拖动阈值（拖动起点也先 seek 到按下处）
}  // namespace

WaveformOverviewItem::WaveformOverviewItem(QQuickItem* parent) : QQuickPaintedItem(parent) {
    setAntialiasing(true);
    setAcceptedMouseButtons(Qt::LeftButton);
    setCursor(QCursor(Qt::PointingHandCursor));
    setImplicitHeight(36.0);
}

ChartSession* WaveformOverviewItem::sessionObj() const {
    return qobject_cast<ChartSession*>(m_session);
}

ThemeManager* WaveformOverviewItem::themeObj() const {
    return qobject_cast<ThemeManager*>(m_theme);
}

void WaveformOverviewItem::setSession(QObject* session) {
    if (m_session == session) return;
    if (auto* old = sessionObj()) disconnect(old, nullptr, this, nullptr);
    m_session = session;
    if (auto* cs = sessionObj()) {
        // 渲染完成 → 波形数据就绪 → 重绘（renderFinished 信号在 UI 线程发）
        connect(cs, &ChartSession::renderFinished, this, [this] { update(); });
        // 文档/内容变化 → 波形过期/换谱 → 重绘（显示占位提示）
        connect(cs, &ChartSession::documentChanged, this, [this] { update(); });
        connect(cs, &ChartSession::contentChanged, this, [this] { update(); });
    }
    emit sessionChanged();
    update();
}

void WaveformOverviewItem::setTheme(QObject* theme) {
    if (m_theme == theme) return;
    m_theme = theme;
    emit themeChanged();
    update();
}

void WaveformOverviewItem::setOrientation(int v) {
    if (m_orientation == v) return;
    m_orientation = v;
    emit orientationChanged();
    update();
}

void WaveformOverviewItem::setMeasureHeight(qreal v) {
    if (qFuzzyCompare(m_measureHeight, v)) return;
    m_measureHeight = v;
    emit measureHeightChanged();
    update();
}

void WaveformOverviewItem::setScrollY(qreal v) {
    if (qFuzzyCompare(m_scrollY, v)) return;
    m_scrollY = v;
    emit scrollYChanged();
    update();
}

void WaveformOverviewItem::setContentHeight(qreal v) {
    if (qFuzzyCompare(m_contentHeight, v)) return;
    m_contentHeight = v;
    emit contentHeightChanged();
    update();
}

void WaveformOverviewItem::setTopHigh(bool v) {
    if (m_topHigh == v) return;
    m_topHigh = v;
    emit topHighChanged();
    update();
}

void WaveformOverviewItem::setViewportHeight(qreal v) {
    if (qFuzzyCompare(m_viewportHeight, v)) return;
    m_viewportHeight = v;
    emit viewportHeightChanged();
    update();
}

void WaveformOverviewItem::setLeadMeasures(qreal v) {
    if (qFuzzyCompare(m_leadMeasures, v)) return;
    m_leadMeasures = v;
    emit leadMeasuresChanged();
    update();
}

QString WaveformOverviewItem::formatTime(double sec) {
    if (sec < 0.0) sec = 0.0;
    const int total = static_cast<int>(std::lround(sec));
    const int m = total / 60;
    const int s = total % 60;
    return QString::asprintf("%d:%02d", m, s);
}

WaveformOverviewItem::ViewWindow WaveformOverviewItem::viewWindow() const {
    ViewWindow w{0.0, 0.0};
    const ChartSession* cs = sessionObj();
    if (!cs || !cs->timing() || !cs->chart()) return w;
    if (m_contentHeight <= 0.0 || m_measureHeight <= 0.0) return w;
    // 内容坐标（拍位）：topHigh 下内容自下而上 → 视口上下缘对应内容坐标区间。
    // ✂️ M5.2 开头留白：contentCoord → 拍位须扣 lead（contentHeight 已含 lead；
    //    与 ChartViewItem::measureAt 同构）。未扣则视口窗整体偏往未来（用顶/底基准都不准）。
    const qreal cLo = m_topHigh ? (m_contentHeight - (m_scrollY + m_viewportHeight))
                                : m_scrollY;
    const qreal cHi = m_topHigh ? (m_contentHeight - m_scrollY) : (m_scrollY + m_viewportHeight);
    const qreal mfLo = cLo / m_measureHeight - m_leadMeasures;
    const qreal mfHi = cHi / m_measureHeight - m_leadMeasures;
    // 拍位 → 秒：TimingEngine::time_us(Position{m, pos})（pos = 小节内分数，微小节精度）
    const auto toSec = [&](qreal mf) -> double {
        if (mf < 0.0) mf = 0.0;
        const std::uint32_t msec = static_cast<std::uint32_t>(mf);
        const double frac = mf - msec;
        // Rational(num, den)：frac 乘 1e6 取整（近似足够；BPM 变化在小节内线性段）
        const beatbench::Rational pos(static_cast<std::int64_t>(std::lround(frac * 1e6)),
                                      1000000);
        const beatbench::Position p{msec, pos};
        return static_cast<double>(cs->timing()->time_us(p)) / 1e6;
    };
    double t0 = toSec(mfLo);
    double t1 = toSec(mfHi);
    if (t0 > t1) std::swap(t0, t1);
    w.t0 = std::min(t0, t1);
    w.t1 = std::max(t0, t1);
    return w;
}

void WaveformOverviewItem::requestSeek(qreal x) {
    const ChartSession* cs = sessionObj();
    if (!cs) return;
    const auto info = cs->waveformInfo();
    if (!info.value(QStringLiteral("valid")).toBool()) return;
    const double dur = info.value(QStringLiteral("durationSec")).toDouble();
    const qreal extent = (m_orientation == 1) ? height() : width();
    double frac = std::clamp(x / std::max<qreal>(1.0, extent), 0.0, 1.0);
    // 垂直条时间方向与视口一致（topHigh）：topHigh=true → 0s 在底部 → 反比
    if (m_orientation == 1 && m_topHigh) frac = 1.0 - frac;
    emit seekRequested(frac * dur);
}

void WaveformOverviewItem::mousePressEvent(QMouseEvent* event) {
    if (event->button() == Qt::LeftButton) {
        // ⚠️ 必须 accept 以拿到鼠标捕获：否则按下后 move/release 不会派发给本 item
        // （编辑器 MouseArea 在下方 z:0，会在拖动过程中接走事件）→ 点击能跳但拖动不生效。
        event->accept();
        const QPointF p = event->position();
        requestSeek(m_orientation == 1 ? p.y() : p.x());
    }
    QQuickPaintedItem::mousePressEvent(event);
}

void WaveformOverviewItem::mouseMoveEvent(QMouseEvent* event) {
    if (event->buttons() & Qt::LeftButton) {
        event->accept();
        const QPointF p = event->position();
        requestSeek(m_orientation == 1 ? p.y() : p.x());
    }
    QQuickPaintedItem::mouseMoveEvent(event);
}

void WaveformOverviewItem::mouseReleaseEvent(QMouseEvent* event) {
    QQuickPaintedItem::mouseReleaseEvent(event);
}

void WaveformOverviewItem::paint(QPainter* p) {
    const qreal w = width();
    const qreal h = height();
    if (w <= 0 || h <= 0) return;
    const ThemeManager* th = themeObj();
    p->fillRect(QRectF(0, 0, w, h), th ? th->surface() : QColor(QStringLiteral("#12151a")));

    const ChartSession* cs = sessionObj();
    const auto info = cs ? cs->waveformInfo() : QVariantMap();
    const bool valid = info.value(QStringLiteral("valid")).toBool();
    const qreal extent = (m_orientation == 1) ? h : w;   // 波形轴向长度
    const qreal extentC = (m_orientation == 1) ? w : h;  // 垂直轴长度（宽度）
    if (!valid) {
        // 未渲染：提示（旋转 90° 的文本在垂直条上太挤 → 用短文本 + 方向感知）
        p->setPen(th ? th->textFaint() : QColor(QStringLiteral("#6b7484")));
        p->drawText(QRectF(0, 0, w, h), Qt::AlignCenter,
                    m_orientation == 1 ? QStringLiteral("波形") : QStringLiteral("波形（Space 渲染后显示）"));
        return;
    }

    const double dur = info.value(QStringLiteral("durationSec")).toDouble();
    const qreal barTop = 3.0;
    const qreal barH = extentC - 2.0 * barTop;
    const qreal center = extentC / 2.0;

    // ---- 逐列 min/max（每像素一列；列数 = width，防大宽下重查询——宽度受限） ----
    const qreal framesTotal = info.value(QStringLiteral("frames")).toDouble();
    const double sr = info.value(QStringLiteral("sampleRate")).toDouble();
    const qreal pxPerSec = extent / std::max(1.0, dur);
    // 波形幅度最值（归一化：非全 0 时除以最大值；静音/低音仍可见）
    // ⚠️ 不能逐列查询再归一（会闪）——先扫全曲顶层桶求全局 min/max 幅度缩放。
    qreal gMin = 0.0, gMax = 0.0;
    {
        const auto r = cs->waveformRange(0, static_cast<qlonglong>(framesTotal));
        gMin = r.value(QStringLiteral("min")).toDouble();
        gMax = r.value(QStringLiteral("max")).toDouble();
    }
    const qreal amp = std::max(std::abs(gMin), std::abs(gMax));
    const qreal scale = amp > 1e-6 ? (barH / 2.0 - 1.0) / amp : 0.0;

    QColor waveCol = th ? th->wave() : QColor(QStringLiteral("#8b9cf8"));
    waveCol.setAlpha(220);
    p->setPen(Qt::NoPen);
    // C++ 侧直接查金字塔（每列一次 range；比 QML 逐列 invoke 快一个量级）
    auto wf = cs->waveformPyramid();
    for (int i = 0; i < static_cast<int>(extent); ++i) {
        // i → 时间比例：垂直条 topHigh 下 0s 在底部 → 从底部往上画
        const qreal frac = (m_orientation == 1 && m_topHigh)
                               ? static_cast<qreal>(extent - 1 - i) / extent
                               : static_cast<qreal>(i) / extent;
        const double t0s = frac * dur;
        const double t1s = (frac + 1.0 / extent) * dur;
        const qlonglong f0 = static_cast<qlonglong>(t0s * sr);
        const qlonglong f1 = static_cast<qlonglong>(t1s * sr);
        if (f1 <= f0) continue;
        const auto r = wf ? wf->range(static_cast<std::size_t>(f0),
                                      static_cast<std::size_t>(f1))
                          : beatbench::audio::WaveformPyramid::Range{};
        const double mn = r.min;
        const double mx = r.max;
        // 静音（min==max==0）：跳过（保持背景干净）
        if (mn == 0.0 && mx == 0.0) continue;
        if (m_orientation == 1) {
            // 垂直：幅度向左/右铺（center ± scale）；时间轴 = i（y 方向）
            const qreal xLeft = center - mx * scale;
            const qreal xRight = center - mn * scale;
            p->fillRect(QRectF(std::min(xLeft, xRight), i, std::max<qreal>(1.0, std::abs(xRight - xLeft)), 1.0),
                        waveCol);
        } else {
            // 水平：幅度向上/下铺（center ± scale）；时间轴 = i（x 方向）
            const qreal yTop = center - mx * scale;
            const qreal yBot = center - mn * scale;
            p->fillRect(QRectF(i, yTop, 1.0, std::max<qreal>(1.0, yBot - yTop)), waveCol);
        }
    }

    // ---- 视口窗指示（半透明覆盖 + 可拖拽边缘感） ----
    const auto win = viewWindow();
    if (win.t1 > win.t0 && dur > 0.0) {
        qreal a0 = static_cast<qreal>(win.t0 / dur);
        qreal a1 = static_cast<qreal>(win.t1 / dur);
        if (m_orientation == 1 && m_topHigh) {  // 垂直条：0s 在底部 → 窗翻转
            std::swap(a0, a1);
            a0 = 1.0 - a0;
            a1 = 1.0 - a1;
        }
        QColor ov = th ? th->accent() : QColor(QStringLiteral("#22d3ee"));
        ov.setAlpha(36);
        if (m_orientation == 1) {
            const qreal y0 = a0 * h, y1 = a1 * h;
            p->fillRect(QRectF(0, std::min(y0, y1), w, std::abs(y1 - y0)), ov);
            p->setPen(QPen(th ? th->accent() : QColor(QStringLiteral("#22d3ee")), 1.0));
            p->drawLine(QPointF(0, y0), QPointF(w, y0));
            p->drawLine(QPointF(0, y1), QPointF(w, y1));
        } else {
            const qreal x0 = a0 * w, x1 = a1 * w;
            p->fillRect(QRectF(std::min(x0, x1), 0, std::abs(x1 - x0), h), ov);
            p->setPen(QPen(th ? th->accent() : QColor(QStringLiteral("#22d3ee")), 1.0));
            p->drawLine(QPointF(x0, 0), QPointF(x0, h));
            p->drawLine(QPointF(x1, 0), QPointF(x1, h));
        }
    }

    // ---- 时间标注（横条右下 / 竖条右下角；垂直条画在顶部竖排太挤 → 底部小字） ----
    QFont f = p->font();
    f.setPixelSize(th ? th->fsTiny() : 11.0);
    p->setFont(f);
    p->setPen(th ? th->textFaint() : QColor(QStringLiteral("#6b7484")));
    if (m_orientation == 1) {
        p->drawText(QRectF(0, h - 14, w, 14), Qt::AlignCenter,
                    QStringLiteral("%1").arg(formatTime(win.t0)));
    } else {
        p->drawText(QRectF(4, 0, w - 8, h), Qt::AlignRight | Qt::AlignVCenter,
                    QStringLiteral("%1 / %2").arg(formatTime(win.t0)).arg(formatTime(dur)));
    }
}

}  // namespace beatbench::app
