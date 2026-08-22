// SPDX-License-Identifier: GPL-3.0-only
// ChartViewItem 实现：QPainter 自绘竖向时间轴（标尺/网格/轨道列/note·LN·地雷）。
// 性能：只绘制可见小节范围（culling）；首屏 245 小节 ≈ 6 万 note 级谱面实测不足再迁 QSG
// （doc/08 §2 路线：QPainter 起步，瓶颈后迁）。
#include "bridge/ChartViewItem.hpp"

#include <QHoverEvent>
#include <QPainter>
#include <QPainterPath>

#include <algorithm>
#include <cstdlib>
#include <cmath>

#include "bridge/ChartSession.hpp"
#include "bridge/ThemeManager.hpp"
#include "beatbench/core/bms/ChannelMap.hpp"

namespace beatbench::app {

ChartViewItem::ChartViewItem(QQuickItem* parent) : QQuickPaintedItem(parent) {
    setAntialiasing(true);
    setAcceptHoverEvents(true);  // 状态栏鼠标位置 + note 信息（M2 跟进）
    m_rulerFont.setFamily(QStringLiteral("Consolas"));
    m_rulerFont.setPixelSize(11);
    m_noteLabelFont.setFamily(QStringLiteral("Consolas"));
    m_noteLabelFont.setPixelSize(9);
}

ChartSession* ChartViewItem::sessionObj() const {
    return qobject_cast<ChartSession*>(m_session);
}

ThemeManager* ChartViewItem::themeObj() const {
    return qobject_cast<ThemeManager*>(m_theme);
}

void ChartViewItem::setSession(QObject* session) {
    if (m_session == session) return;
    if (auto* old = sessionObj()) disconnect(old, nullptr, this, nullptr);
    m_session = session;
    if (auto* cs = sessionObj()) {
        connect(cs, &ChartSession::chartChanged, this, &ChartViewItem::onSessionChartChanged);
    }
    rebuildColumns();
    emit sessionChanged();
    emit contentHeightChanged();
    emit chartChanged();
    update();
}

void ChartViewItem::setTheme(QObject* theme) {
    if (m_theme == theme) return;
    m_theme = theme;
    emit themeChanged();
    update();
}

void ChartViewItem::onSessionChartChanged() {
    rebuildColumns();
    emit contentHeightChanged();
    emit chartChanged();
    update();
}

void ChartViewItem::setMeasureHeight(qreal v) {
    if (qFuzzyCompare(m_measureHeight, v)) return;
    // 缩放锚点：保持视口中心对应的拍位不动（修复滚动位置跳变）。
    // ⚠️ measureAt(screenY) 内部已加 scrollY（屏幕→内容坐标），这里只传视口中心的
    // **屏幕** y = height()/2；若再传 scrollY+height()/2 会双重加 scrollY（历史 bug：
    // 中心拍位恒为负 → setScrollY 钳到最底，Ctrl+滚轮必跳回 01 小节）。
    const qreal centerMf = measureAt(height() / 2.0);
    m_measureHeight = v;
    const qreal centerContent = m_topHigh
                                    ? (contentHeight() - centerMf * m_measureHeight)
                                    : (centerMf * m_measureHeight);
    setScrollY(centerContent - height() / 2.0);
    emit measureHeightChanged();
    emit contentHeightChanged();
    update();
}

void ChartViewItem::setRulerWidth(qreal v) {
    if (qFuzzyCompare(m_rulerWidth, v)) return;
    m_rulerWidth = v;
    emit rulerWidthChanged();
    update();
}

void ChartViewItem::setLaneWidth(qreal v) {
    if (qFuzzyCompare(m_laneWidth, v)) return;
    m_laneWidth = v;
    emit laneWidthChanged();
    update();
}

void ChartViewItem::setScrollY(qreal v) {
    const qreal clamped = std::clamp(v, 0.0, std::max<qreal>(0.0, contentHeight() - height()));
    if (qFuzzyCompare(m_scrollY, clamped)) return;
    m_scrollY = clamped;
    emit scrollYChanged();
    update();
}

qreal ChartViewItem::contentHeight() const {
    const ChartSession* cs = sessionObj();
    return (cs ? cs->measureCount() : 0) * m_measureHeight;
}

void ChartViewItem::setTopHigh(bool v) {
    if (m_topHigh == v) return;
    m_topHigh = v;
    emit topHighChanged();
    update();
}

void ChartViewItem::setGridDiv(int v) {
    if (m_gridDiv == v) return;
    m_gridDiv = std::max(1, v);
    emit gridDivChanged();
    update();
}

void ChartViewItem::setBgmExpanded(bool v) {
    if (m_bgmExpanded == v) return;
    m_bgmExpanded = v;
    rebuildColumns();
    emit bgmExpandedChanged();
    update();
}

void ChartViewItem::setShowChannelIds(bool v) {
    if (m_showChannelIds == v) return;
    m_showChannelIds = v;
    rebuildColumns();
    emit showChannelIdsChanged();
    update();
}

int ChartViewItem::bgmHeaderIndexAt(qreal x) const {
    for (std::size_t i = 0; i < m_columns.size(); ++i) {
        if (!m_columns[i].bgm) continue;
        if (i < m_colRects.size() && m_colRects[i].contains(QPointF(x, 9.0)))
            return static_cast<int>(i);
    }
    return -1;
}

void ChartViewItem::setBeatNum(int v) {
    if (m_beatNum == v) return;
    m_beatNum = std::max(1, v);
    emit beatNumChanged();
    update();
}

void ChartViewItem::setBeatDen(int v) {
    if (m_beatDen == v) return;
    m_beatDen = std::max(1, v);
    emit beatDenChanged();
    update();
}

void ChartViewItem::setNoteSampleMode(int v) {
    const int clamped = std::clamp(v, 0, 2);
    if (m_noteSampleMode == clamped) return;
    m_noteSampleMode = clamped;
    emit noteSampleModeChanged();
    update();
}

void ChartViewItem::setShowExtras(bool v) {
    if (m_showExtras == v) return;
    m_showExtras = v;
    rebuildColumns();
    emit showExtrasChanged();
    update();
}

qreal ChartViewItem::metaTrackWidth() const {
    qreal w = 0.0;
    for (const auto& c : m_columns)
        if (c.bpm || c.stop) w += m_metaTrackWidth;
    return w;
}

qreal ChartViewItem::columnWidth(std::size_t i) const {
    if (i >= m_columns.size()) return m_laneWidth;
    return (m_columns[i].bpm || m_columns[i].stop) ? m_metaTrackWidth : m_laneWidth;
}

qreal ChartViewItem::contentWidth() const {
    qreal w = m_rulerWidth + 16.0;
    for (std::size_t i = 0; i < m_columns.size(); ++i) w += columnWidth(i);
    return w;
}

void ChartViewItem::setScrollX(qreal v) {
    const qreal clamped =
        std::clamp(v, 0.0, std::max<qreal>(0.0, contentWidth() - width()));
    if (qFuzzyCompare(m_scrollX, clamped)) return;
    m_scrollX = clamped;
    emit scrollXChanged();
    update();
}

void ChartViewItem::clampScrollX() {
    const qreal clamped =
        std::clamp(m_scrollX, 0.0, std::max<qreal>(0.0, contentWidth() - width()));
    if (!qFuzzyCompare(m_scrollX, clamped)) {
        m_scrollX = clamped;
        emit scrollXChanged();
        update();
    }
}

void ChartViewItem::hoverMoveEvent(QHoverEvent* event) {
    QQuickPaintedItem::hoverMoveEvent(event);
    updateHover(event->position());
}

void ChartViewItem::hoverLeaveEvent(QHoverEvent* event) {
    QQuickPaintedItem::hoverLeaveEvent(event);
    if (!m_hoverText.isEmpty() || m_hoverY >= 0) {
        m_hoverText.clear();
        m_hoverMeasure = -1;
        m_hoverY = -1;
        emit hoverChanged();
        update();
    }
}

void ChartViewItem::updateHover(const QPointF& pos) {
    QString text;
    int measure = -1;
    const ChartSession* cs = sessionObj();
    if (cs && cs->chart() && m_measureHeight > 0 && pos.y() >= 0) {
        const qreal mf = measureAt(pos.y());
        if (mf >= 0 && mf < cs->measureCount()) {
            measure = static_cast<int>(std::floor(mf));
            text = QString::asprintf("%03d : %.2f", measure, mf - measure);
            // 命中 note → 追加 轨道 + 采样（只查相邻小节，控制开销）
            const beatbench::Chart& chart = *cs->chart();
            const qreal noteH = std::max(5.0, m_measureHeight * 0.08);
            for (const auto& ev : chart.notes) {
                if (static_cast<int>(ev.measure) < measure - 1 ||
                    static_cast<int>(ev.measure) > measure + 1)
                    continue;
                const int col = columnFor(ev.value.lane, ev.value.sample.id);
                if (col < 0 || static_cast<std::size_t>(col) >= m_colRects.size())
                    continue;
                const QRectF& r = m_colRects[col];
                const qreal y = yOf(ev.measure + posDouble(ev.pos));
                const QRectF hit(r.x() + 2, y - noteH, r.width() - 4, noteH);
                if (hit.contains(pos)) {
                    text += QStringLiteral(" · ") + m_columns[col].label;
                    if (ev.value.kind == beatbench::NoteKind::Landmine) {
                        text += QStringLiteral(" · 地雷");
                    } else {
                        const QString idText = QString::number(ev.value.sample.id, 36)
                                                   .toUpper()
                                                   .rightJustified(2, QLatin1Char('0'));
                        text += QStringLiteral(" · #WAV") + idText;
                    }
                    break;
                }
            }
        }
    }
    if (text != m_hoverText || measure != m_hoverMeasure || m_hoverY != pos.y()) {
        m_hoverText = text;
        m_hoverMeasure = measure;
        m_hoverY = pos.y();
        emit hoverChanged();
        update();
    }
}

void ChartViewItem::geometryChange(const QRectF& newGeometry, const QRectF& oldGeometry) {
    QQuickPaintedItem::geometryChange(newGeometry, oldGeometry);
    // 高度/宽度变化 → 重夹滚动并重绘（初装时 height()==0 的 clamp 依赖这里兜底）
    if (!qFuzzyCompare(newGeometry.height(), oldGeometry.height())) {
        const qreal before = m_scrollY;
        clampScroll();
        if (!qFuzzyCompare(before, m_scrollY)) emit scrollYChanged();
        update();
    }
    if (!qFuzzyCompare(newGeometry.width(), oldGeometry.width())) clampScrollX();
}

void ChartViewItem::clampScroll() {
    const qreal clamped = std::clamp(m_scrollY, 0.0, std::max<qreal>(0.0, contentHeight() - height()));
    if (!qFuzzyCompare(m_scrollY, clamped)) {
        m_scrollY = clamped;
        emit scrollYChanged();
    }
}

qreal ChartViewItem::yOf(qreal measureFloat) const {
    const qreal c = measureFloat * m_measureHeight;
    return (m_topHigh ? (contentHeight() - c) : c) - m_scrollY;
}

qreal ChartViewItem::measureAt(qreal screenY) const {
    const qreal c = screenY + m_scrollY;  // 屏幕 → 内容坐标
    return m_topHigh ? (contentHeight() - c) / m_measureHeight
                     : c / m_measureHeight;
}

qreal ChartViewItem::posDouble(const beatbench::Rational& r) const {
    return static_cast<qreal>(r.num) / static_cast<qreal>(r.den);
}

QColor ChartViewItem::noteColor(const beatbench::Lane& lane) const {
    const ThemeManager* th = themeObj();
    if (!th) return QColor(QStringLiteral("#8b9cf8"));
    if (lane.kind == beatbench::LaneKind::Scratch) return th->scratch();
    if (lane.kind == beatbench::LaneKind::Bgm) return th->textMuted();  // 灰 = 自动播放轨，非击打
    if (lane.kind == beatbench::LaneKind::Key) {
        switch ((lane.index - 1) % 4) {  // 键 1/5 → n1，2/6 → n2，3/7 → n3，4 → n4
            case 1: return th->n2();
            case 2: return th->n3();
            case 3: return th->n4();
            default: return th->n1();
        }
    }
    return th->textMuted();  // Pedal 等罕见轨：弱化色
}

double ChartViewItem::bpmAt(const beatbench::Chart& chart, int measure) const {
    double bpm = 130.0;  // 缺省 BPM（对齐 TimingEngine）
    if (const auto it = chart.meta.find("BPM"); it != chart.meta.end())
        bpm = std::atof(it->second.c_str());
    // 事件按 (measure,pos) 升序（doc/04 §6 约定）；取 measure 之前最后事件 + 本小节 pos0
    for (const auto& ev : chart.bpm_events) {
        if (static_cast<int>(ev.measure) > measure) break;
        if (static_cast<int>(ev.measure) < measure ||
            (ev.measure == static_cast<std::uint32_t>(measure) && ev.pos.num == 0)) {
            bpm = ev.value.value;
        }
    }
    return bpm;
}

double ChartViewItem::beatsOf(const beatbench::Chart& chart, int measure) const {
    double beats = 4.0;  // 缺省 4/4（四分拍；ch02 口径见 doc/04 §6）
    for (const auto& ev : chart.measure_events) {
        if (static_cast<int>(ev.measure) == measure) beats = ev.value.beats;
    }
    return beats;
}

int ChartViewItem::columnFor(const beatbench::Lane& lane, std::uint32_t bgmSampleId) const {
    for (std::size_t i = 0; i < m_columns.size(); ++i) {
        const auto& c = m_columns[i];
        if (c.bgaLayer >= 0) continue;  // BGA 图层列不匹配 note
        if (c.lane != lane) continue;
        if (c.bgm) {
            // 聚合列（bgmId==0）命中所有；展开列按 #WAV id 精确匹配
            if (c.bgmId == 0 || c.bgmId == bgmSampleId) return static_cast<int>(i);
            continue;
        }
        return static_cast<int>(i);
    }
    return -1;
}

int ChartViewItem::columnForBga(int layer) const {
    for (std::size_t i = 0; i < m_columns.size(); ++i) {
        if (m_columns[i].bgaLayer == layer) return static_cast<int>(i);
    }
    return -1;
}

QString ChartViewItem::columnLabel(const beatbench::Lane& lane,
                                   const QString& displayName) const {
    if (!m_showChannelIds) return displayName;
    const std::string ch = beatbench::bms::bms_channel_for(lane, false,
                                                           beatbench::NoteKind::Normal);
    if (!ch.empty()) return QString::fromStdString(ch);
    const std::string dm =
        beatbench::bms::bms_channel_for(lane, false, beatbench::NoteKind::Landmine);
    if (!dm.empty()) return QString::fromStdString(dm);
    return displayName;
}

void ChartViewItem::rebuildColumns() {
    m_columns.clear();
    if (const ChartSession* cs = sessionObj()) {
        const beatbench::Chart* chart = cs->chart();
        if (chart) {
            int maxKey1 = 0, maxKey2 = 0;
            bool scratch1 = false, pedal1 = false, scratch2 = false, pedal2 = false;
            std::vector<std::uint32_t> bgmIds;   // 背景轨实际使用的 #WAV id（展开分列用）
            for (const auto& ev : chart->notes) {
                const auto& l = ev.value.lane;
                if (l.kind == beatbench::LaneKind::Bgm) {
                    const auto id = ev.value.sample.id;
                    if (std::find(bgmIds.begin(), bgmIds.end(), id) == bgmIds.end())
                        bgmIds.push_back(id);
                } else if (l.kind == beatbench::LaneKind::Key) {
                    (l.player == 0 ? maxKey1 : maxKey2) =
                        std::max(l.player == 0 ? maxKey1 : maxKey2, static_cast<int>(l.index));
                } else if (l.kind == beatbench::LaneKind::Scratch) {
                    (l.player == 0 ? scratch1 : scratch2) = true;
                } else if (l.kind == beatbench::LaneKind::Pedal) {
                    (l.player == 0 ? pedal1 : pedal2) = true;
                }
            }
            std::sort(bgmIds.begin(), bgmIds.end());  // 通道从小到大（= #WAV id 升序）
            const auto add = [this](const beatbench::Lane& lane, const QString& label,
                                    bool bgmCol = false, bool p2 = false,
                                    std::uint32_t bgmId = 0, int bgaLayer = -1,
                                    bool bpm = false, bool stop = false) {
                m_columns.push_back({lane, label, bgmCol, p2, bgmId, bgaLayer, bpm, stop});
            };
            // 元事件轨（固定置左、轨窄，iBMSC 式）：BPM / STOP——始终显示（无事件也要能看拍位）
            add({0, beatbench::LaneKind::Pedal, 0}, QStringLiteral("BPM"), false, false, 0, -1,
                true);
            add({0, beatbench::LaneKind::Pedal, 0}, QStringLiteral("STOP"), false, false, 0, -1,
                false, true);
            // 1P：皿 → 键 1..max → 踏板；BGA 图层（更多轨道，iBMSC 式）；BGM 轨；2P 同构
            if (scratch1) {
                const beatbench::Lane l{0, beatbench::LaneKind::Scratch, 0};
                add(l, columnLabel(l, QStringLiteral("S")));
            }
            for (int k = 1; k <= maxKey1; ++k) {
                const beatbench::Lane l{0, beatbench::LaneKind::Key,
                                        static_cast<std::uint8_t>(k)};
                add(l, columnLabel(l, QString::number(k)));
            }
            if (pedal1) {
                const beatbench::Lane l{0, beatbench::LaneKind::Pedal, 0};
                add(l, columnLabel(l, QStringLiteral("P")));
            }
            if (m_showExtras) {
                // BGA 图层通道列（固定四列，iBMSC 布局与顺序：BGA=04(base) LAYER=06
                // POOR=07，扩展 LAYER2=0A）；默认显示名字，勾「通道 ID」时显示通道号。
                // 图层→通道：layer 0=base(04) 1=poor(07) 2=layer(06) 3=layer2(0A)。
                static constexpr int kOrder[4] = {0, 2, 1, 3};  // 显示顺序：base→layer→poor→layer2
                static const char* const kName[4] = {"BGA", "POOR", "LAYER", "LAYER2"};
                static const char* const kCh[4] = {"04", "07", "06", "0A"};
                for (const int layer : kOrder) {
                    const QString label =
                        m_showChannelIds ? QString::fromLatin1(kCh[layer])
                                         : QString::fromLatin1(kName[layer]);
                    add({0, beatbench::LaneKind::Bgm, 0}, label, false, false, 0, layer);
                }
            }
            if (!bgmIds.empty()) {
                const beatbench::Lane bgmLane{0, beatbench::LaneKind::Bgm, 0};
                if (!m_bgmExpanded) {
                    add(bgmLane, columnLabel(bgmLane, QStringLiteral("BGM")), true);
                } else {
                    for (const auto id : bgmIds) {
                        // 展示为 BMS 2 位 id 文本（01/0A/1A…，避免与键列数字混淆）
                        const QString idText =
                            QString::number(id, 36).toUpper().rightJustified(2, QLatin1Char('0'));
                        add(bgmLane, idText, true, false, id);
                    }
                }
            }
            if (scratch2 || maxKey2 > 0 || pedal2) {
                if (scratch2) {
                    const beatbench::Lane l{1, beatbench::LaneKind::Scratch, 0};
                    add(l, columnLabel(l, QStringLiteral("2P·S")), false, true);
                }
                for (int k = 1; k <= maxKey2; ++k) {
                    const beatbench::Lane l{1, beatbench::LaneKind::Key,
                                            static_cast<std::uint8_t>(k)};
                    add(l, columnLabel(l, QStringLiteral("2P·%1").arg(k)), false, true);
                }
                if (pedal2) {
                    const beatbench::Lane l{1, beatbench::LaneKind::Pedal, 0};
                    add(l, columnLabel(l, QStringLiteral("2P·P")), false, true);
                }
            }
        }
    }
    emit columnCountChanged();
    update();
}

void ChartViewItem::drawHint(QPainter* p, const QString& text) {
    const ThemeManager* th = themeObj();
    QFont f = p->font();
    f.setPixelSize(th ? th->fsBase() : 13.0);
    p->setFont(f);
    p->setPen(th ? th->textFaint() : QColor(QStringLiteral("#6b7484")));
    p->drawText(QRectF(0, 0, width(), height()), Qt::AlignCenter, text);
}

void ChartViewItem::paint(QPainter* p) {
    const qreal w = width();
    const qreal h = height();
    const ThemeManager* th = themeObj();
    if (w <= 0 || h <= 0) return;
    p->fillRect(QRectF(0, 0, w, h), th ? th->bg() : QColor(QStringLiteral("#0b0d10")));

    const ChartSession* cs = sessionObj();
    if (!cs || !cs->chart() || cs->measureCount() <= 0) {
        drawHint(p, QStringLiteral("打开谱面开始编辑（Ctrl+O）"));
        return;
    }
    const beatbench::Chart& chart = *cs->chart();
    const int mCount = cs->measureCount();
    const qreal rulerW = m_rulerWidth;

    // ---- 轨道列布局（左侧标尺 + BPM/STOP 固定窄轨 + 车道区；超宽时水平滚动 scrollX） ----
    m_colRects.assign(m_columns.size(), QRectF());
    qreal metaRight = rulerW;  // 元事件轨右缘（BPM/STOP；网格/STOP 带/悬停线从这起）
    {
        const qreal metaW = metaTrackWidth();
        qreal laneTotal = 0.0;
        for (std::size_t i = 0; i < m_columns.size(); ++i)
            if (!(m_columns[i].bpm || m_columns[i].stop)) laneTotal += columnWidth(i);
        const qreal laneArea = w - rulerW - metaW;
        qreal laneX;
        if (laneTotal <= laneArea) {
            laneX = rulerW + metaW + (laneArea - laneTotal) / 2.0;  // 未超宽：居中
        } else {
            laneX = rulerW + metaW - m_scrollX;  // 超宽：按 scrollX 平移（底部滚动条 / Shift+滚轮）
        }
        qreal x = rulerW;
        for (std::size_t i = 0; i < m_columns.size(); ++i) {
            const bool meta = m_columns[i].bpm || m_columns[i].stop;
            const qreal cw = columnWidth(i);
            m_colRects[i] = QRectF(meta ? x : laneX, 0, cw, h);
            if (meta) x += cw;
            else laneX += cw;
        }
        metaRight = rulerW + metaW;
    }

    // ---- 可见小节范围（内容坐标 → 拍位） ----
    int first = 0, last = mCount - 1;
    {
        qreal cLo, cHi;
        if (m_topHigh) {
            cLo = contentHeight() - (m_scrollY + h);
            cHi = contentHeight() - m_scrollY;
        } else {
            cLo = m_scrollY;
            cHi = m_scrollY + h;
        }
        first = std::max(0, static_cast<int>(std::floor(cLo / m_measureHeight)));
        last = std::min(mCount - 1,
                        static_cast<int>(std::ceil(cHi / m_measureHeight)) - 1);
        if (first > last) return;  // 视口在谱面末之后（空窗口）
    }

    // ---- 小节底纹（交叉间隔） ----
    QColor alt = th->surface();
    alt.setAlpha(70);
    for (int m = first; m <= last; ++m) {
        if (m % 2 == 1) p->fillRect(QRectF(0, yOf(m), w, m_measureHeight), alt);
    }

    // ---- 轨道列底色 / 分隔 ----
    for (std::size_t i = 0; i < m_columns.size(); ++i) {
        const QRectF& r = m_colRects[i];
        const Column& col = m_columns[i];
        QColor tint;
        if (col.bgm) {
            tint = th->wave();
            if (col.bgmId != 0) tint.setAlpha(18);  // 展开列：更淡的底色（2026-08 跟进，保对比度）
        } else if (col.lane.kind == beatbench::LaneKind::Scratch) {
            tint = th->accent();
            tint.setAlpha(26);  // ≈ preview .lane.scratch 7%
        } else if (col.p2) {
            tint = th->primary();
            tint.setAlpha(13);
        } else if (col.bgaLayer >= 0) {
            tint = th->primarySoft();  // BGA 图层列（更多轨道）
            tint.setAlpha(22);
        }
        if (tint.alpha() > 0) p->fillRect(r, tint);
        p->setPen(QPen(th->border(), 1.0));
        p->drawLine(r.topLeft(), r.bottomLeft());
    }

    // ---- 网格（小节强线 + 槽位弱线） ----
    QPen strong(th->border());
    strong.setWidthF(1.0);
    QColor weakC = th->border();
    weakC.setAlpha(90);
    QPen weak(weakC);
    weak.setWidthF(1.0);
    for (int m = first; m <= last; ++m) {
        if (m_gridDiv > 1) {
            for (int i = 1; i < m_gridDiv; ++i) {
                p->setPen(weak);
                p->drawLine(QPointF(metaRight, yOf(m + i / static_cast<qreal>(m_gridDiv))),
                            QPointF(w, yOf(m + i / static_cast<qreal>(m_gridDiv))));
            }
        }
        // 拍子线（[num]/[den]：每 num 个 den 分音符一条；默认 [1]/[4] = 每 4 分音符）
        {
            const double beats = std::max(beatsOf(chart, m), 0.001);
            const double step = (4.0 * m_beatNum) / (m_beatDen * beats);
            if (step > 0.001 && step < 0.9999) {
                QColor beatC = th->border();
                beatC.setAlpha(150);
                p->setPen(QPen(beatC, 1.0));
                for (double k = step; k < 0.9999; k += step)
                    p->drawLine(QPointF(metaRight, yOf(m + k)), QPointF(w, yOf(m + k)));
            }
        }
        p->setPen(strong);
        p->drawLine(QPointF(metaRight, yOf(m)), QPointF(w, yOf(m)));
    }

    // ---- 小节标尺（左列：小节号，贴小节起始线下方；BPM 数值移到右邻 BPM 轨） ----
    p->fillRect(QRectF(0, 0, rulerW, h), th->surface());
    p->setPen(QPen(th->border(), 1.0));
    p->drawLine(QPointF(rulerW, 0), QPointF(rulerW, h));
    p->setFont(m_rulerFont);
    for (int m = first; m <= last; ++m) {
        const qreal y0 = yOf(m);
        p->setPen(th->textMuted());
        p->drawText(QRectF(3, y0 + 1, rulerW - 6, 13),
                    Qt::AlignLeft | Qt::AlignTop, QString::asprintf("%03d", m));
    }

    // ---- BPM / STOP 元事件轨（固定置左窄列；标记画在实际 (measure,pos)，值贴事件下缘） ----
    const int bpmColI = [&] {
        for (std::size_t i = 0; i < m_columns.size(); ++i)
            if (m_columns[i].bpm) return static_cast<int>(i);
        return -1;
    }();
    const int stopColI = [&] {
        for (std::size_t i = 0; i < m_columns.size(); ++i)
            if (m_columns[i].stop) return static_cast<int>(i);
        return -1;
    }();
    if (bpmColI >= 0 && static_cast<std::size_t>(bpmColI) < m_colRects.size()) {
        const QRectF r = m_colRects[bpmColI];
        p->setFont(m_rulerFont);
        const auto drawBpm = [&](double value, qreal y) {
            if (y < -14 || y > h + 14) return;
            QColor tick = th->accent();
            tick.setAlpha(200);
            p->fillRect(QRectF(r.x() + 1, y - 1, r.width() - 2, 2), tick);
            p->setPen(th->accent2());
            p->drawText(QRectF(r.x() + 2, y + 2, r.width() - 3, 12),
                        Qt::AlignLeft | Qt::AlignTop, QString::number(value, 'g', 6));
        };
        for (const auto& ev : chart.bpm_events) {
            if (static_cast<int>(ev.measure) < first - 1 ||
                static_cast<int>(ev.measure) > last + 1)
                continue;
            drawBpm(ev.value.value, yOf(ev.measure + posDouble(ev.pos)));
        }
        // 无 (0,0) BPM 事件 → 头 #BPM 基准值画在 0 小节起点
        bool hasStart = false;
        for (const auto& ev : chart.bpm_events)
            if (ev.measure == 0 && ev.pos.num == 0) { hasStart = true; break; }
        if (!hasStart && first == 0) {
            double bpm = 130.0;
            if (const auto it = chart.meta.find("BPM"); it != chart.meta.end())
                bpm = std::atof(it->second.c_str());
            drawBpm(bpm, yOf(0));
        }
    }
    if (stopColI >= 0 && static_cast<std::size_t>(stopColI) < m_colRects.size()) {
        const QRectF r = m_colRects[stopColI];
        p->setFont(m_rulerFont);
        for (const auto& ev : chart.stop_events) {
            if (static_cast<int>(ev.measure) < first - 1 ||
                static_cast<int>(ev.measure) > last + 1)
                continue;
            const qreal y = yOf(ev.measure + posDouble(ev.pos));
            if (y < -14 || y > h + 14) continue;
            QColor tick = th->warning();
            tick.setAlpha(200);
            p->fillRect(QRectF(r.x() + 1, y - 1, r.width() - 2, 2), tick);
            p->setPen(th->warning());
            QString t = QString::number(ev.value.duration_us / 1e6, 'f', 2);
            while (t.endsWith(QLatin1Char('0')) && t.contains(QLatin1Char('.'))) t.chop(1);
            if (t.endsWith(QLatin1Char('.'))) t.chop(1);
            p->drawText(QRectF(r.x() + 2, y + 2, r.width() - 3, 12),
                        Qt::AlignLeft | Qt::AlignTop, t);
        }
    }

    // ---- STOP 段（按当前 BPM 换算为小节分数；精确秒换算 = TimingEngine，秒标尺后置） ----
    for (const auto& ev : chart.stop_events) {
        if (static_cast<int>(ev.measure) < first || static_cast<int>(ev.measure) > last)
            continue;
        const qreal y = yOf(ev.measure + posDouble(ev.pos));
        const double frac = (ev.value.duration_us / 1e6 * bpmAt(chart, ev.measure) / 60.0) /
                            std::max(beatsOf(chart, ev.measure), 0.001);
        QColor sc = th->warning();
        sc.setAlpha(64);
        // hi-top：后一时间在上方 → 带需跨 [y(尾), y(头)]（min..max）
        const qreal y2 = yOf(ev.measure + posDouble(ev.pos) + frac);
        p->fillRect(QRectF(metaRight, std::min(y, y2), w - metaRight,
                           std::max<qreal>(std::abs(y2 - y), 2.0)),
                    sc);
    }

    // ---- note / LN / 地雷（note 底边 = 实际时间点） ----
    const qreal noteH = std::max(5.0, m_measureHeight * 0.08);
    // 描边标签（文件名模式/控制轨）：文字居中于格子、允许越出 note，浅色描边保证
    // 超出部分落在深底上也清晰可读（用户反馈 2026-09：纯深色文字在深底对比度不足）。
    const auto drawHaloLabel = [&](const QRectF& cell, const qreal y, const QString& label,
                                   const QColor& halo) {
        if (label.isEmpty()) return;
        p->setFont(m_noteLabelFont);
        const QFontMetrics fm(m_noteLabelFont);
        const int tw = fm.horizontalAdvance(label);
        const qreal baseline = (y - noteH / 2.0) + (fm.ascent() - fm.descent()) / 2.0;
        QPainterPath tp;
        tp.addText(QPointF(cell.center().x() - tw / 2.0, baseline), m_noteLabelFont, label);
        p->setPen(QPen(halo, 1.4, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
        p->drawPath(tp);
        p->fillPath(tp, QBrush(th->bg()));
    };
    // note 内采样标签：id 只显示 00-ZZ（无 #WAV 前缀，贴 note 内裁剪）；文件名去扩展名
    const auto drawSampleLabel = [&](const QRectF& colRect, const qreal y,
                                     const beatbench::Note& note) {
        if (m_noteSampleMode == 0 || note.kind == beatbench::NoteKind::Landmine) return;
        QString label;
        if (m_noteSampleMode == 1) {
            label = QString::number(note.sample.id, 36).toUpper().rightJustified(
                2, QLatin1Char('0'));
        } else {
            const auto it = chart.samples.find({beatbench::SampleKind::Wav, note.sample.id});
            if (it == chart.samples.end()) return;
            label = QString::fromStdString(it->second.file);
            const int dot = label.lastIndexOf(QLatin1Char('.'));
            if (dot > 0) label.truncate(dot);  // 去扩展名（.wav/.ogg…）
        }
        if (label.isEmpty()) return;
        if (m_noteSampleMode == 2) {
            // 文件名可能比 note 长：不裁剪、居中、同轨浅色描边（深底也清晰）
            drawHaloLabel(colRect, y, label, noteColor(note.lane));
            return;
        }
        p->save();
        p->setFont(m_noteLabelFont);
        p->setPen(th->bg());  // 深色文字（note 底色为亮色）
        const QRectF nr(colRect.x() + 2, y - noteH, colRect.width() - 4, noteH);
        p->setClipRect(nr);
        const int tw = QFontMetrics(m_noteLabelFont).horizontalAdvance(label);
        const int flags = (tw <= nr.width() ? Qt::AlignCenter : Qt::AlignLeft) |
                          Qt::AlignVCenter | Qt::TextSingleLine;
        p->drawText(nr, flags, label);
        p->restore();
    };
    for (std::size_t i = 0; i < chart.notes.size(); ++i) {
        const auto& ev = chart.notes[i];
        if (static_cast<int>(ev.measure) < first || static_cast<int>(ev.measure) > last)
            continue;
        const int col = columnFor(ev.value.lane, ev.value.sample.id);
        if (col < 0) continue;
        const QRectF& r = m_colRects[col];
        const qreal y = yOf(ev.measure + posDouble(ev.pos));
        const auto& note = ev.value;

        if (note.kind == beatbench::NoteKind::Landmine) {
            // 菱形地雷（底尖 = 实际时间点；≈ preview .note.mine 45°）
            QPainterPath path;
            const qreal s = 5.0;
            path.moveTo(r.center().x(), y - 2.0 * s);
            path.lineTo(r.center().x() + s, y - s);
            path.lineTo(r.center().x(), y);
            path.lineTo(r.center().x() - s, y - s);
            path.closeSubpath();
            p->fillPath(path, th->mine());
            continue;
        }

        if (note.ln_pair) {
            const auto& partner = chart.notes[*note.ln_pair];
            const bool isHead =
                ev.measure < partner.measure ||
                (ev.measure == partner.measure &&
                 posDouble(ev.pos) < posDouble(partner.pos));
            if (isHead) {
                const qreal y2 = yOf(partner.measure + posDouble(partner.pos));
                // hi-top：尾在小节上方（y2 < y）→ 体跨 min..max
                const qreal ya = std::min(y, y2), yb = std::max(y, y2);
                p->fillRect(QRectF(r.center().x() - 3.0, ya, 6.0,
                                   std::max<qreal>(yb - ya, 4.0)),
                            th->ln());
                p->fillRect(QRectF(r.x() + 2, y - noteH, r.width() - 4, noteH),
                            noteColor(partner.value.lane));
                p->fillRect(QRectF(r.x() + 2, y2 - noteH, r.width() - 4, noteH),
                            noteColor(partner.value.lane));
                drawSampleLabel(r, y, note);
            } else if (static_cast<int>(partner.measure) < first ||
                       static_cast<int>(partner.measure) > last) {
                // 头在可视范围外：只画尾帽
                p->fillRect(QRectF(r.x() + 2, y - noteH, r.width() - 4, noteH),
                            noteColor(note.lane));
                drawSampleLabel(r, y, note);
            }
            continue;
        }

        p->fillRect(QRectF(r.x() + 2, y - noteH, r.width() - 4, noteH),
                    noteColor(note.lane));
        drawSampleLabel(r, y, note);
    }

    // ---- BGA 图层事件（更多轨道；固定四列：BGA/LAYER/POOR/LAYER2 = 04/06/07/0A） ----
    if (m_showExtras) {
        for (const auto& ev : chart.bga_events) {
            if (static_cast<int>(ev.measure) < first ||
                static_cast<int>(ev.measure) > last)
                continue;
            const int col = columnForBga(ev.value.layer);
            if (col < 0) continue;
            const QRectF& r = m_colRects[col];
            const qreal y = yOf(ev.measure + posDouble(ev.pos));
            QColor c;
            switch (ev.value.layer) {
                case 1: c = th->danger(); break;    // poor
                case 2: c = th->primary(); break;   // layer
                case 3: c = th->accent2(); break;   // layer2
                default: c = th->n1(); break;       // base
            }
            c.setAlpha(200);
            p->fillRect(QRectF(r.x() + 3, y - noteH, r.width() - 6, noteH), c);
            // 控制轨单元格同样显示 id/数值（用户反馈 2026-09）：id = #BGAxx 十进制数值
            if (m_noteSampleMode != 0) {
                QString label;
                if (m_noteSampleMode == 1) {
                    label = QString::number(ev.value.image.id).rightJustified(2, QLatin1Char('0'));
                } else {
                    const auto it =
                        chart.samples.find({beatbench::SampleKind::Bmp, ev.value.image.id});
                    if (it != chart.samples.end()) {
                        label = QString::fromStdString(it->second.file);
                        const int dot = label.lastIndexOf(QLatin1Char('.'));
                        if (dot > 0) label.truncate(dot);  // 去扩展名
                    }
                }
                if (!label.isEmpty()) drawHaloLabel(r, y, label, c);
            }
        }
    }

    // ---- 悬停参考线（状态栏拍位位置） ----
    if (m_hoverY >= 0 && m_hoverY < h) {
        QColor hov = th->accent();
        hov.setAlpha(60);
        p->setPen(QPen(hov, 1.0));
        p->drawLine(QPointF(metaRight, m_hoverY), QPointF(w, m_hoverY));
    }

    // ---- 轨道列标签（视口顶部固定横条，不随内容滚动；BGM 列 accent 色提示可点击） ----
    QColor lbg = th->surface3();
    lbg.setAlpha(200);
    p->fillRect(QRectF(0, 0, rulerW, 18), lbg);
    p->setFont(m_rulerFont);
    for (std::size_t i = 0; i < m_columns.size(); ++i) {
        p->fillRect(QRectF(m_colRects[i].x(), 0, m_colRects[i].width(), 18), lbg);
        const Column& col = m_columns[i];
        p->setPen(col.bgm ? th->accent2()
                          : col.stop ? th->warning()
                                     : col.bpm ? th->accent2()
                                               : th->textMuted());
        p->drawText(QRectF(m_colRects[i].x(), 0, m_colRects[i].width(), 18), Qt::AlignCenter,
                    col.label);
    }
}

}  // namespace beatbench::app
