// SPDX-License-Identifier: GPL-3.0-only
// SliceWaveformItem 实现（见 hpp 注释）。M6.4 起为「换行视口」：整曲按 rowSec 换行成
// 多行，行 k 覆盖 [k·rowSec, (k+1)·rowSec)（行界 = rowSec 整数倍）；滚轮/键盘整行滚动，
// Ctrl+滚轮缩放。绘制顺序（每行）：波形列 → 中央轴 → 网格参考 → MIDI 刻度 → 切片线 →
// 播放头 → 行分隔线 + 左侧标签（小节号+秒）。
#include "bridge/SliceWaveformItem.hpp"

#include <QMouseEvent>
#include <QPainter>
#include <QWheelEvent>
#include <algorithm>
#include <cmath>
#include <limits>

#include "bridge/SliceWorkspace.hpp"
#include "bridge/ThemeManager.hpp"

namespace beatbench::app {

// 行绘制共用常量：左侧标签 gutter 宽（跨 paint/交互函数使用；先于其定义）
static constexpr qreal kGutterW = 64.0;

SliceWaveformItem::SliceWaveformItem(QQuickItem* parent) : QQuickPaintedItem(parent) {
    setAntialiasing(false);  // 波形逐列 1px：抗锯齿反而糊
    setAcceptedMouseButtons(Qt::LeftButton | Qt::RightButton);
}

void SliceWaveformItem::setWorkspace(QObject* v) {
    if (m_workspace == v) return;
    if (auto* old = workspaceObj()) disconnect(old, nullptr, this, nullptr);
    m_workspace = v;
    if (auto* ws = workspaceObj()) {
        // 数据变更（解码完成/MIDI 导入/offset/切片变化）→ 重绘（信号均在 UI 线程发）
        connect(ws, &SliceWorkspace::audioChanged, this, [this] { update(); });
        connect(ws, &SliceWorkspace::midiChanged, this, [this] { update(); });
        connect(ws, &SliceWorkspace::offsetChanged, this, [this] { update(); });
        connect(ws, &SliceWorkspace::slicesChanged, this, [this] { update(); });
    }
    emit workspaceChanged();
    update();
}

void SliceWaveformItem::setTheme(QObject* v) {
    if (m_theme == v) return;
    m_theme = v;
    emit themeChanged();
    update();
}

void SliceWaveformItem::setPlayheadSec(qreal v) {
    if (qFuzzyCompare(m_playheadSec, v)) return;
    m_playheadSec = v;
    emit playheadSecChanged();
    update();
}

void SliceWaveformItem::setGridVisible(bool v) {
    if (m_gridVisible == v) return;
    m_gridVisible = v;
    emit gridVisibleChanged();
    update();
}

void SliceWaveformItem::setGridBpm(qreal v) {
    if (qFuzzyCompare(m_gridBpm, v)) return;
    m_gridBpm = v;
    emit gridBpmChanged();
    update();
}

void SliceWaveformItem::setGridSubdivision(int v) {
    if (m_gridSubdivision == v) return;
    m_gridSubdivision = v;
    emit gridSubdivisionChanged();
    update();
}

void SliceWaveformItem::setGridBeatsPerMeasure(int v) {
    if (m_gridBeatsPerMeasure == v) return;
    m_gridBeatsPerMeasure = v;
    emit gridBeatsPerMeasureChanged();
    update();
}

void SliceWaveformItem::setMidiVisible(bool v) {
    if (m_midiVisible == v) return;
    m_midiVisible = v;
    emit midiVisibleChanged();
    update();
}

void SliceWaveformItem::setRowSec(qreal v) {
    if (qFuzzyCompare(m_rowSec, v)) return;
    m_rowSec = v;
    emit rowSecChanged();
    update();
}

void SliceWaveformItem::setScrollRow(int v) {
    if (m_scrollRow == v) return;
    m_scrollRow = v;
    emit scrollRowChanged();
    update();
}

void SliceWaveformItem::setVisibleRows(int v) {
    const int c = std::clamp(v, 1, 8);
    if (m_visibleRows == c) return;
    m_visibleRows = c;
    emit visibleRowsChanged();
    update();
}

void SliceWaveformItem::setSelectedBeatSec(qreal v) {
    if (qFuzzyCompare(m_selectedBeatSec, v)) return;
    m_selectedBeatSec = v;
    emit selectedBeatSecChanged();
    update();
}

int SliceWaveformItem::totalRows() const {
    const SliceWorkspace* ws = workspaceObj();
    if (!ws || !ws->hasAudio()) return 1;
    const double dur = static_cast<double>(ws->audioDurationSec());
    const double rs = (m_rowSec > 0.0) ? m_rowSec : dur;
    if (!(rs > 0.0) || dur <= 0.0) return 1;
    return std::max(1, static_cast<int>(std::ceil(dur / rs - 1e-9)));
}

SliceWorkspace* SliceWaveformItem::workspaceObj() const {
    return qobject_cast<SliceWorkspace*>(m_workspace);
}

ThemeManager* SliceWaveformItem::themeObj() const { return qobject_cast<ThemeManager*>(m_theme); }

void SliceWaveformItem::requestSeek(qreal x, qreal y) {
    const SliceWorkspace* ws = workspaceObj();
    if (!ws || !ws->hasAudio()) return;
    const double dur = static_cast<double>(ws->audioDurationSec());
    const double rs = (m_rowSec > 0.0) ? m_rowSec : dur;
    const qreal gutterW = 64.0;
    const qreal plotW = std::max<qreal>(1.0, width() - gutterW);
    const int vis = std::max(1, m_visibleRows);
    const qreal rowH = height() / vis;
    const int rowIdx = static_cast<int>(std::floor(y / rowH));
    double t = (static_cast<double>(m_scrollRow) + rowIdx) * rs +
               static_cast<double>(x - gutterW) / (plotW / rs);
    if (t < 0.0 || t > dur) return;
    // M6.4b：网格模式下点击 seek 吸附最近的拍子线（为方向键/手动切片铺路）
    if (m_gridVisible) t = snapToGrid(t);
    emit seekRequested(t);
}

double SliceWaveformItem::snapToGrid(double t) const {
    const SliceWorkspace* ws = workspaceObj();
    if (!ws || !ws->hasAudio() || m_gridBpm <= 0.0) return t;
    const int sub = std::max(1, m_gridSubdivision);
    const double cell = 60.0 / static_cast<double>(m_gridBpm) / static_cast<double>(sub);
    if (!std::isfinite(cell) || cell <= 0.0) return t;
    const double offset = ws->offsetSecD();
    const double k = std::round((t - offset) / cell);
    double out = offset + k * cell;
    const double dur = static_cast<double>(ws->audioDurationSec());
    return std::clamp(out, 0.0, dur);
}

void SliceWaveformItem::mousePressEvent(QMouseEvent* event) {
    if (event->button() == Qt::LeftButton) {
        event->accept();
        forceActiveFocus();  // 键盘滚动（方向键）入口
        // M6.4d 手动切分（不限时间，无双击阈值）：首次点击 = 选中拍子；
        // 再点击同一拍子 = 切分点切换（添加/删除）；落点 gridVisible 时吸附拍线
        const double t = manualPointAt(event->position().x(), event->position().y());
        if (t >= 0.0) {
            if (m_selectedBeatSec >= 0.0 && std::abs(t - m_selectedBeatSec) <= 1e-4) {
                emit manualToggleRequested(t);  // 同一拍子再击 → toggle
            } else {
                setSelectedBeatSec(t);  // 选中（或换选）
            }
        }
        requestSeek(event->position().x(), event->position().y());
    } else if (event->button() == Qt::RightButton) {
        event->accept();
        forceActiveFocus();
        // M6.4c 手动切分：右键删除切分点（落点须命中边界；吸附后发）
        const double t = manualPointAt(event->position().x(), event->position().y());
        if (t >= 0.0) emit manualDeleteRequested(t);
    }
    QQuickPaintedItem::mousePressEvent(event);
}

double SliceWaveformItem::manualPointAt(qreal x, qreal y) const {
    const SliceWorkspace* ws = workspaceObj();
    if (!ws || !ws->hasAudio()) return -1.0;
    const double dur = static_cast<double>(ws->audioDurationSec());
    const double rs = (m_rowSec > 0.0) ? m_rowSec : dur;
    const qreal plotW = std::max<qreal>(1.0, width() - kGutterW);
    const int vis = std::max(1, m_visibleRows);
    const qreal rowH = height() / vis;
    const int rowIdx = static_cast<int>(std::floor(y / rowH));
    double t = (static_cast<double>(m_scrollRow) + rowIdx) * rs +
               static_cast<double>(x - kGutterW) / (plotW / rs);
    if (t < 0.0 || t > dur) return -1.0;
    if (m_gridVisible) t = snapToGrid(t);  // 网格模式：落点吸附拍子线
    return t;
}

void SliceWaveformItem::mouseMoveEvent(QMouseEvent* event) {
    if (event->buttons() & Qt::LeftButton) {
        event->accept();
        requestSeek(event->position().x(), event->position().y());
    }
    QQuickPaintedItem::mouseMoveEvent(event);
}

void SliceWaveformItem::mouseReleaseEvent(QMouseEvent* event) {
    // 拖动结束：落点再 seek 一次（QML 端 refSeek 幂等；保证最终位置精确）
    if (event->button() == Qt::LeftButton)
        requestSeek(event->position().x(), event->position().y());
    QQuickPaintedItem::mouseReleaseEvent(event);
}

void SliceWaveformItem::wheelEvent(QWheelEvent* event) {
    const int dy = event->angleDelta().y();
    if (dy == 0) {
        event->ignore();
        return;
    }
    if (event->modifiers() & Qt::ControlModifier) {
        emit zoomRequested(dy > 0 ? 1 : -1);  // Ctrl+滚轮 = 缩放（QML 换档）
    } else {
        emit scrollRequested(dy > 0 ? -1 : 1);  // 滚轮 = 整行滚动（QML 改 scrollRow）
    }
    event->accept();
}

// 行绘制共用常量：左侧标签 gutter 宽
// （定义于文件上部 namespace 起始处）

void SliceWaveformItem::paint(QPainter* p) {
    const qreal w = width();
    const qreal h = height();
    if (w <= 0 || h <= 0) return;
    const ThemeManager* th = themeObj();
    p->fillRect(QRectF(0, 0, w, h), th ? th->surface() : QColor(QStringLiteral("#12151a")));

    const SliceWorkspace* ws = workspaceObj();
    const auto pyr = ws ? ws->waveformPyramid() : nullptr;
    if (!ws || !pyr || !pyr->valid()) {
        p->setPen(th ? th->textFaint() : QColor(QStringLiteral("#6b7484")));
        p->drawText(QRectF(0, 0, w, h), Qt::AlignCenter, QStringLiteral("导入参考音频后显示波形"));
        return;
    }

    const double dur = static_cast<double>(ws->audioDurationSec());
    if (dur <= 0.0) return;
    const double rs = (m_rowSec > 0.0) ? m_rowSec : dur;
    const int total = std::max(1, static_cast<int>(std::ceil(dur / rs - 1e-9)));
    const int vis = std::clamp(m_visibleRows, 1, 8);
    const int maxRow = std::max(0, total - vis);
    const int sr = std::clamp(m_scrollRow, 0, maxRow);
    const qreal rowH = h / vis;
    const qreal plotW = w - kGutterW;
    if (plotW <= 10.0) return;
    const qreal pxPerSec = plotW / rs;

    // ---- 全曲幅度归一化（先扫全局；行间尺度一致，防逐行归一化闪烁） ----
    qreal amp = 0.0;
    {
        const auto r = pyr->range(0, pyr->frameCount());
        amp = std::max(std::abs(r.min), std::abs(r.max));
    }
    const double srRate = pyr->sampleRate() > 0.0 ? pyr->sampleRate() : 44100.0;
    // 深档直读 PCM（跨距 <256 采样时金字塔会把高频波形糊成实心）——持有共享指针保证生命周期
    const auto pcmHolder = ws->track().pcm();
    const float* pcmData = pcmHolder ? pcmHolder->data() : nullptr;
    const std::size_t pcmFrames = pcmHolder ? pcmHolder->size() / 2 : 0;

    const QColor rowSep = th ? th->border() : QColor(QStringLiteral("#2a2f3a"));
    for (int i = 0; i < vis; ++i) {
        const int rowIdx = sr + i;
        const qreal top = i * rowH;
        const QRectF plot(kGutterW, top, plotW, rowH);
        if (rowIdx < total)
            drawRow(p, plot, rowIdx * rs, (rowIdx + 1) * rs, ws, pyr, pcmData, pcmFrames, pxPerSec,
                    amp, srRate);
        // 行分隔线
        p->fillRect(QRectF(0, top + rowH - 1.0, w, 1.0), rowSep);
        // 行标签（左侧 gutter；行内容在右侧）
        if (rowIdx < total)
            drawRowLabel(p, QRectF(0, top, kGutterW, rowH), rowIdx * rs, (rowIdx + 1) * rs, th);
    }
}

void SliceWaveformItem::drawRow(QPainter* p, const QRectF& plot, double t0, double t1,
                                const SliceWorkspace* ws,
                                const beatbench::audio::WaveformPyramid* pyr, const float* pcm,
                                std::size_t pcmFrames, qreal pxPerSec, qreal amp, double sr) const {
    const ThemeManager* th = themeObj();
    const qreal w = plot.width();
    const qreal h = plot.height();
    if (w <= 1.0 || h <= 1.0) return;
    p->save();
    p->setClipRect(plot);

    const qreal centerY = plot.y() + h / 2.0;
    // ---- 逐列 min/max（t = t0 + col/pxPerSec） ----
    // M6.4b：每像素跨距 < 256 采样 → 直接扫原始 PCM（金字塔 base 桶 256 会把
    // 高频波形（如 440Hz 正弦的周期）糊成实心方块）；否则走金字塔（浅档）。
    QColor waveCol = th ? th->wave() : QColor(QStringLiteral("#8b9cf8"));
    waveCol.setAlpha(220);
    QColor gridCol = th ? th->border() : QColor(QStringLiteral("#2a2f3a"));
    p->setPen(Qt::NoPen);
    const double spanPerPx = sr / pxPerSec;
    const bool direct = (pcm != nullptr) && pcmFrames > 0 && spanPerPx < 256.0;
    for (int i = 0; i < static_cast<int>(w); ++i) {
        const double t = t0 + static_cast<double>(i) / pxPerSec;
        const double tNext = t0 + static_cast<double>(i + 1) / pxPerSec;
        const std::size_t f0 = static_cast<std::size_t>(t * sr);
        const std::size_t f1 = static_cast<std::size_t>(tNext * sr);
        auto r = beatbench::audio::WaveformPyramid::Range{};
        if (direct) {
            // 直接扫：mono = 左右均值；夹逼帧数（行可能超出音频末端）
            const std::size_t e = std::min(f1, pcmFrames);
            float mn = std::numeric_limits<float>::max();
            float mx = std::numeric_limits<float>::lowest();
            for (std::size_t f = f0; f < e; ++f) {
                const float mono = (pcm[2 * f] + pcm[2 * f + 1]) * 0.5f;
                mn = std::min(mn, mono);
                mx = std::max(mx, mono);
            }
            if (e > f0) r = {mn, mx};
        } else {
            r = pyr ? pyr->range(f0, std::max(f0 + 1, f1)) : r;
        }
        if (amp > 1e-6 && (r.max != 0.0f || r.min != 0.0f)) {
            const qreal yTop = centerY - static_cast<qreal>(r.max) / amp * (h / 2.0 - 1.0);
            const qreal yBot = centerY - static_cast<qreal>(r.min) / amp * (h / 2.0 - 1.0);
            p->fillRect(QRectF(plot.x() + i, yTop, 1.0, std::max<qreal>(1.0, yBot - yTop)),
                        waveCol);
        }
    }
    // ---- 中央轴（行内） ----
    p->setPen(QPen(gridCol, 1));
    p->drawLine(QPointF(plot.x(), centerY), QPointF(plot.x() + w, centerY));

    // ---- 实时拍子网格参考线 ----
    drawGridLines(p, plot, t0, t1, ws, pxPerSec, th);

    // ---- MIDI note 刻度行 ----
    const double offset = ws->offsetSecD();
    QColor noteCol = th ? th->accent2() : QColor(QStringLiteral("#2dd8c8"));
    noteCol.setAlpha(170);
    if (m_midiVisible) {
        for (const auto& n : ws->notes()) {
            const double xs = n.startSec + offset;
            const double xe = n.endSec + offset;
            if (xe < t0 || xs > t1) continue;
            const qreal cx = plot.x() + static_cast<qreal>((xs - t0) * pxPerSec);
            p->fillRect(QRectF(cx, plot.y() + 2.0, 1.5, h - 4.0), noteCol);
            if (xe > xs + 1.0) {
                QColor tail = noteCol;
                tail.setAlpha(90);
                const qreal cxe = plot.x() + static_cast<qreal>((xe - t0) * pxPerSec);
                p->fillRect(QRectF(cxe, plot.y() + 2.0, 1.5, h - 4.0), tail);
            }
        }
    }

    // ---- 切片边界线（grid = 主色；midi = 强调；淡色 = 切片末端） ----
    if (ws->hasSlices()) {
        QColor gridLine = th ? th->primary() : QColor(QStringLiteral("#8b9cf8"));
        gridLine.setAlpha(210);
        QColor midiLine = th ? th->accent2() : QColor(QStringLiteral("#2dd8c8"));
        midiLine.setAlpha(230);
        for (const auto& s : ws->slicesC()) {
            const bool isMidi = s.kind == "midi";
            QColor col = isMidi ? midiLine : gridLine;
            const double xs = s.startSec;
            const double xe = s.endSec;
            if (xe < t0 || xs > t1) continue;
            const qreal cx = plot.x() + static_cast<qreal>((xs - t0) * pxPerSec);
            p->fillRect(QRectF(cx, plot.y(), 1.0, h), col);
            QColor tail = col;
            tail.setAlpha(80);
            const qreal cxe = plot.x() + static_cast<qreal>((xe - t0) * pxPerSec);
            p->fillRect(QRectF(cxe, plot.y(), 1.0, h), tail);
        }
    }

    // ---- 播放头（行内竖线） ----
    if (m_playheadSec >= 0.0 && m_playheadSec >= t0 && m_playheadSec <= t1) {
        const qreal x = plot.x() + static_cast<qreal>((m_playheadSec - t0) * pxPerSec);
        QColor ph = th ? th->primary() : QColor(QStringLiteral("#8b9cf8"));
        p->fillRect(QRectF(x - 0.75, plot.y(), 1.5, h), ph);
    }

    // ---- 选中拍子光标（M6.4d：teal 2px 竖带 + 顶部 tab；点击选中、再击同一拍子=切分点切换） ----
    if (m_selectedBeatSec >= 0.0 && m_selectedBeatSec >= t0 && m_selectedBeatSec <= t1) {
        const qreal x = plot.x() + static_cast<qreal>((m_selectedBeatSec - t0) * pxPerSec);
        QColor c = th ? th->accent2() : QColor(QStringLiteral("#2dd8c8"));
        c.setAlpha(200);
        p->fillRect(QRectF(x - 1.0, plot.y(), 2.0, h), c);
        p->fillRect(QRectF(x - 3.0, plot.y(), 6.0, 6.0),
                    th ? th->accent2() : QColor(QStringLiteral("#2dd8c8")));
    }

    p->restore();
}

void SliceWaveformItem::drawRowLabel(QPainter* p, const QRectF& row, double t0, double t1,
                                     const ThemeManager* th) const {
    // 小节号（按当前网格 BPM/拍数换算；round 到最近整数小节）+ 秒
    const int bpm = std::max(1, static_cast<int>(std::lround(m_gridBpm)));
    const int bpmCount = std::max(1, m_gridBeatsPerMeasure);
    const double beatSec = 60.0 / bpm;
    const int measure = static_cast<int>(std::floor(t0 / (beatSec * bpmCount) + 1e-9)) + 1;
    const int sec = static_cast<int>(std::floor(t0));
    const QString label = QStringLiteral("%1 | %2:%3")
                              .arg(measure, 3, 10, QChar('0'))
                              .arg(sec / 60)
                              .arg(sec % 60, 2, 10, QChar('0'));
    QFont f = p->font();
    f.setFamily(th ? th->fontMono() : QStringLiteral("Consolas"));
    f.setPixelSize(10);
    p->setFont(f);
    p->setPen(th ? th->textMuted() : QColor(QStringLiteral("#9aa3b2")));
    p->drawText(row.adjusted(4, 0, -4, 0), Qt::AlignRight | Qt::AlignVCenter, label);
    Q_UNUSED(t1);
}

void SliceWaveformItem::drawGridLines(QPainter* p, const QRectF& plot, double t0, double t1,
                                      const SliceWorkspace* ws, qreal pxPerSec,
                                      const ThemeManager* th) const {
    if (!m_gridVisible || !ws || !ws->hasAudio()) return;
    if (m_gridBpm <= 0.0) return;
    const int sub = std::max(1, m_gridSubdivision);
    const int bpmCount = std::max(1, m_gridBeatsPerMeasure);
    const double cell = 60.0 / static_cast<double>(m_gridBpm) / static_cast<double>(sub);
    if (!std::isfinite(cell) || cell <= 0.0) return;

    const double offset = ws->offsetSecD();
    // 层级配色：起点 = accent 青（最显眼）+ tab/标签；小节 = keyNote 2px；
    // 拍 = keyNote 1px（中亮）；细分 = textMuted 灰 1px。
    const QColor originCol = th ? th->accent() : QColor(QStringLiteral("#22d3ee"));
    QColor measureCol = th ? th->keyNote() : QColor(QStringLiteral("#8b9cf8"));
    QColor beatCol = th ? th->keyNote() : QColor(QStringLiteral("#8b9cf8"));
    QColor subCol = th ? th->textMuted() : QColor(QStringLiteral("#9aa3b2"));
    measureCol.setAlpha(250);
    beatCol.setAlpha(190);
    subCol.setAlpha(110);
    const qreal top = plot.y() + 2.0;
    const qreal bot = plot.y() + plot.height() - 2.0;
    const int cellsPerBeat = sub;
    const int cellsPerMeasure = cellsPerBeat * bpmCount;

    // 取整：从第一个 >= t0 的 cell 开始（t=offset+k*cell）
    int k0 = static_cast<int>(std::ceil((t0 - offset) / cell - 1e-9));
    if (k0 < 0) k0 = 0;
    constexpr int kMaxLines = 20000;
    int drawn = 0;
    for (int k = k0;; ++k, ++drawn) {
        if (drawn > kMaxLines) break;
        const double t = offset + static_cast<double>(k) * cell;
        if (t > t1) break;
        const qreal x = plot.x() + static_cast<qreal>((t - t0) * pxPerSec);
        if (x < plot.x() - 0.5 || x > plot.x() + plot.width() + 0.5) continue;
        const bool isOrigin = (k == 0);
        const bool isMeasure = (k % cellsPerMeasure) == 0;
        const bool isBeat = (k % cellsPerBeat) == 0;
        QColor col;
        double penW = 1.0;
        if (isOrigin) {
            col = originCol;
            penW = 2.0;
        } else if (isMeasure) {
            col = measureCol;
            penW = 2.0;
        } else if (isBeat) {
            col = beatCol;
            penW = 1.0;
        } else {
            col = subCol;
            penW = 1.0;
        }
        p->setPen(QPen(col, penW));
        p->drawLine(QPointF(x, top), QPointF(x, bot));
        if (isOrigin) drawOriginMarker(p, x, th, t, plot);
    }
}

void SliceWaveformItem::drawOriginMarker(QPainter* p, qreal x, const ThemeManager* th, double t,
                                         const QRectF& plot) const {
    // 顶部 tab（起点标记）：一条亮色短横 + 秒数标签（mono），clamp 在行内。
    const QColor col = th ? th->accent() : QColor(QStringLiteral("#22d3ee"));
    const qreal tabW = 10.0;
    const qreal tabH = 8.0;
    const qreal tabLeft =
        std::clamp(x - tabW / 2.0, plot.x(), plot.x() + std::max(0.0, plot.width() - tabW));
    p->fillRect(QRectF(tabLeft, plot.y(), tabW, tabH), col);
    const QString label = QStringLiteral("%1s").arg(t, 0, 'f', 3);
    QFont f = p->font();
    f.setFamily(th ? th->fontMono() : QStringLiteral("Consolas"));
    f.setPixelSize(10);
    p->setFont(f);
    p->setPen(col);
    const int tw = p->fontMetrics().horizontalAdvance(label);
    qreal lx = x + tabW / 2.0 + 3.0;
    lx = std::clamp(lx, plot.x() + 4.0, plot.x() + std::max(4.0, plot.width() - tw - 4.0));
    p->drawText(QPointF(lx, plot.y() + tabH - 1.0), label);
}

}  // namespace beatbench::app
