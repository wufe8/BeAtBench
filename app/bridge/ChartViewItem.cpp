// SPDX-License-Identifier: GPL-3.0-only
// ChartViewItem 实现：QPainter 自绘竖向时间轴（标尺/网格/轨道列/note·LN·地雷）。
// 性能：只绘制可见小节范围（culling）；首屏 245 小节 ≈ 6 万 note 级谱面实测不足再迁 QSG
// （doc/08 §2 路线：QPainter 起步，瓶颈后迁）。
#include "bridge/ChartViewItem.hpp"

#include <QElapsedTimer>
#include <QHoverEvent>
#include <QPainter>
#include <QPainterPath>

#include <algorithm>
#include <cstdlib>
#include <cmath>
#include <unordered_set>

#include "bridge/ChartSession.hpp"
#include "bridge/ThemeManager.hpp"
#include "beatbench/core/bms/ChannelMap.hpp"

namespace beatbench::app {

namespace {
// note 高度：**随缩放浮动且带上下限**（2026-09 用户：不再固定，也非无界随 measureHeight 缩放）。
//   - 上限 = 12px（当前固定值；500% 缩放时不至于过大——旧 `m_measureHeight*0.08` 在 500%
//     时高达 38px，故必须封顶）；
//   - 下限 = 100% 缩放（96px）时相邻 **1/8** note 不重叠（1/8 槽距 = 96/8 = 12px，note 须 < 12px；
//     2026-09 用户：1/16 作下限太窄 → 放宽到 1/8，note 更大更好读）。
//   公式：note = clamp(measureHeight * kNoteHeightScale, kNoteHeightMin, kNoteHeightMax)。
//   100% → 96*0.1 = 9.6px（<12px，1/8 不重叠）；125%(120px) → 达上限 12px；再高保持 12px。
//   11 处 hit/paint/hover 共用，避免漂移（noteHeight() 单点计算）。
constexpr qreal kNoteHeightScale = 0.10;
constexpr qreal kNoteHeightMin = 8.0;
constexpr qreal kNoteHeightMax = 12.0;
/// note 横向内边距（距通道左/右缘的像素；缩小 = note 更宽；右侧分隔线仍可见）。
constexpr qreal kNoteHMargin = 1.0;
// note 选中框相对 note 的外扩：向左右各外扩 0.5px（略大于 note，明确高亮边界）。
constexpr qreal kNoteSelectExpand = 0.5;
}  // namespace

ChartViewItem::ChartViewItem(QQuickItem* parent) : QQuickPaintedItem(parent) {
    setAntialiasing(true);
    setAcceptHoverEvents(true);  // 状态栏鼠标位置 + note 信息（M2 跟进）
    m_rulerFont.setFamily(QStringLiteral("Consolas"));
    m_rulerFont.setPixelSize(11);
    m_noteLabelFont.setFamily(QStringLiteral("Consolas"));
    m_noteLabelFont.setPixelSize(11);
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
        // ⚠️ 只连「文档切换」与「内容变化」—不再连兼容信号 chartChanged：
        // refresh() 在内容变化时也发 chartChanged（旧兼容），若连着它会触发 QML
        // onChartChanged → scrollY 重置（放置/删除后视图跳回 001，2026-09 实测反馈）。
        connect(cs, &ChartSession::documentChanged, this, &ChartViewItem::onSessionChartChanged);
        // 内容变化（编辑放置/删除/撤销）：列结构可能变（新 lane），但**不重置滚动**
        connect(cs, &ChartSession::contentChanged, this,
                &ChartViewItem::onSessionContentChanged);
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

void ChartViewItem::onSessionContentChanged() {
    // 同文档内容变化（note.put/delete/move/undo…）：**重建列**（2026-09 用户确认：
    // 拖 note 到原本无该轨道的背景/BGA 列时，列必须出现否则 note 无处渲染=「消失」）。
    // 滚动位置不重置（rebuildColumns 不动 scrollX/Y；列宽变化时 contentWidth 由 clamp 兜底）。
    // 注：删光某列唯一 note → 该列消失（数据驱动列的固有行为，合乎 iBMSC 惯例）。
    rebuildColumns();
    emit contentHeightChanged();
    update();
}

void ChartViewItem::setSelection(const QVariantList& v) {
    if (m_selection == v) return;
    m_selection = v;
    emit selectionChanged();
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

void ChartViewItem::zoomAt(qreal screenY, qreal factor) {
    if (factor <= 0.0) return;
    const qreal anchorMf = measureAt(screenY);  // 锚点拍位（缩放前后不变）
    const qreal newH = qBound(24.0, m_measureHeight * factor, 480.0);
    if (qFuzzyCompare(m_measureHeight, newH)) return;
    m_measureHeight = newH;
    const qreal anchorContent = m_topHigh
                                    ? (contentHeight() - anchorMf * m_measureHeight)
                                    : (anchorMf * m_measureHeight);
    setScrollY(anchorContent - screenY);
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

void ChartViewItem::setGridDiv(int) {
    // 已废弃：snapNum/snapDen 取代（保留空实现，避免旧 QML 绑定崩溃）。
}

void ChartViewItem::setSnapNum(int v) {
    const int clamped = std::clamp(v, 1, 999);
    if (m_snapNum == clamped) return;
    m_snapNum = clamped;
    emit snapNumChanged();
    update();
}

void ChartViewItem::setSnapDen(int v) {
    const int clamped = std::clamp(v, 1, 192);
    if (m_snapDen == clamped) return;
    m_snapDen = clamped;
    emit snapDenChanged();
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

QString ChartViewItem::noteRefKey(std::uint32_t measure, const beatbench::Rational& pos,
                                  const beatbench::Lane& lane, std::uint32_t sample) {
    return QString::asprintf("%u|%lld|%lld|%d|%d|%d|%u", measure,
                             static_cast<long long>(pos.num), static_cast<long long>(pos.den),
                             static_cast<int>(lane.player), static_cast<int>(lane.kind),
                             static_cast<int>(lane.index), sample);
}

QVariantMap ChartViewItem::hitTest(qreal x, qreal y) const {
    QVariantMap res;
    res.insert(QStringLiteral("valid"), false);
    const ChartSession* cs = sessionObj();
    if (!cs || !cs->chart() || m_measureHeight <= 0.0 || y < 18.0) return res;
    int col = -1;
    for (std::size_t i = 0; i < m_colRects.size(); ++i) {
        const QRectF& r = m_colRects[i];
        if (x >= r.left() && x < r.right() && y >= r.top() && y < r.bottom()) {
            col = static_cast<int>(i);
            break;
        }
    }
    if (col < 0 || static_cast<std::size_t>(col) >= m_columns.size()) return res;
    const Column& c = m_columns[static_cast<std::size_t>(col)];
    // 仅元事件轨（BPM/STOP）不可放置；BGA 图层列（用户确认「不限格式，保存格式一样就允许」，
    // 见问题1）与 BGM 列均可放。
    if (c.bpm || c.stop) return res;
    const qreal mf = measureAt(y);
    if (mf < 0.0 || mf >= cs->measureCount()) return res;
    const int measure = static_cast<int>(std::floor(mf));
    qreal frac = mf - measure;
    // 吸附：粒度 = snapNum/snapDen 小节。槽位坐标 = frac × (snapDen/snapNum)，
    // 四舍五入到整槽，再折算回 num/den（约分由 Rational ctor 完成）。
    const int num = std::max(1, m_snapNum), den = std::max(1, m_snapDen);
    const int slotCount = std::max(1, den / num);  // 每小节槽数 = snapDen/snapNum（整数步长）
    int k = static_cast<int>(std::llround(frac * slotCount));
    if (k >= slotCount) k = slotCount - 1;
    if (k < 0) k = 0;
    // k/slots 小节 = (k * snapDen/slots) / snapDen 拍？——不：grid 是「槽/小节」，
    // 槽位步长 = snapNum/snapDen 小节，故 k 槽对应 frac = k * snapNum/snapDen。
    // 还原：槽 k 的时间 = k*(snapNum/snapDen) 小节 → num=k*snapNum, den=snapDen。
    res.insert(QStringLiteral("num"), static_cast<int>(k * num));
    res.insert(QStringLiteral("den"), den);
    res.insert(QStringLiteral("valid"), true);
    res.insert(QStringLiteral("measure"), measure);
    res.insert(QStringLiteral("lanePlayer"), QVariant::fromValue(c.lane.player));
    res.insert(QStringLiteral("laneIndex"), QVariant::fromValue(c.lane.index));
    QString kind = QStringLiteral("key");
    if (c.lane.kind == beatbench::LaneKind::Scratch) kind = QStringLiteral("scratch");
    else if (c.lane.kind == beatbench::LaneKind::Pedal) kind = QStringLiteral("pedal");
    else if (c.lane.kind == beatbench::LaneKind::Bgm) kind = QStringLiteral("bgm");
    res.insert(QStringLiteral("laneKind"), kind);
    res.insert(QStringLiteral("label"), c.label);
    res.insert(QStringLiteral("bgm_line"), c.bgmLine);
    res.insert(QStringLiteral("bgaLayer"), c.bgaLayer);
    // BGM 展开列：该列即固定 #WAV id（放置用它，不取当前采样）
    if (c.bgm && c.bgmId != 0) res.insert(QStringLiteral("sampleHint"), static_cast<int>(c.bgmId));
    return res;
}

qreal ChartViewItem::measureAtY(qreal y) const {
    const ChartSession* cs = sessionObj();
    if (!cs || !cs->chart() || m_measureHeight <= 0.0) return -1.0;
    const qreal mf = measureAt(y);
    return (mf < 0.0 || mf >= cs->measureCount()) ? -1.0 : mf;
}

QVariantMap ChartViewItem::laneAtX(qreal x) const {
    QVariantMap res;
    res.insert(QStringLiteral("valid"), false);
    if (!sessionObj() || !sessionObj()->chart()) return res;
    // 用给定 x 命中列（任意 y：列 rect 全高可命中，便于快速横向改轨）。取列的中点 y 带判定。
    for (std::size_t i = 0; i < m_colRects.size(); ++i) {
        const QRectF& r = m_colRects[static_cast<std::size_t>(i)];
        // 命中判定用 [left, right)（右开）：相邻列共点（如 x == 右列 left == 左列 right）
        // 应归右列（鼠标在列内而非边界悬空）；避免边界落在左列导致误判（问题1）。
        if (x < r.left() || x >= r.right()) continue;
        const Column& c = m_columns[static_cast<std::size_t>(i)];
        // 元事件轨（BPM/STOP）：2026-09 用户确认「格式可表示 id 就允许移动」——
        // 拖到该列 = note → timing 事件转换（id 不变），不再拒绝。
        if (c.bpm || c.stop) {
            res.insert(QStringLiteral("valid"), true);
            res.insert(QStringLiteral("metaKind"), c.bpm ? QStringLiteral("bpm")
                                                         : QStringLiteral("stop"));
            res.insert(QStringLiteral("lanePlayer"), QVariant::fromValue(c.lane.player));
            res.insert(QStringLiteral("laneKind"), QStringLiteral("key"));
            res.insert(QStringLiteral("laneIndex"), QVariant::fromValue(c.lane.index));
            res.insert(QStringLiteral("label"), c.label);
            res.insert(QStringLiteral("bgm_line"), -1);
            res.insert(QStringLiteral("bgaLayer"), -1);
            return res;
        }
        QString kind = QStringLiteral("key");
        if (c.lane.kind == beatbench::LaneKind::Scratch) kind = QStringLiteral("scratch");
        else if (c.lane.kind == beatbench::LaneKind::Pedal) kind = QStringLiteral("pedal");
        else if (c.lane.kind == beatbench::LaneKind::Bgm) kind = QStringLiteral("bgm");
        res.insert(QStringLiteral("valid"), true);
        res.insert(QStringLiteral("lanePlayer"), QVariant::fromValue(c.lane.player));
        res.insert(QStringLiteral("laneIndex"), QVariant::fromValue(c.lane.index));
        res.insert(QStringLiteral("laneKind"), kind);
        res.insert(QStringLiteral("label"), c.label);
        res.insert(QStringLiteral("bgm_line"), c.bgmLine);
        res.insert(QStringLiteral("bgaLayer"), c.bgaLayer);
        return res;
    }
    return res;
}

QVariantMap ChartViewItem::probe(qreal x, qreal y) const {
    QVariantMap res;
    res.insert(QStringLiteral("noteAt"), noteAt(x, y));
    res.insert(QStringLiteral("hitTest"), hitTest(x, y));
    res.insert(QStringLiteral("laneAtX"), laneAtX(x));
    // 列布局（诊断）：label + x 范围，用于倒推 BGM/BGA 列位置
    QVariantList cols;
    for (std::size_t i = 0; i < m_columns.size(); ++i) {
        if (i >= m_colRects.size()) continue;
        QVariantMap cm;
        cm.insert(QStringLiteral("label"), m_columns[i].label);
        cm.insert(QStringLiteral("x"), m_colRects[i].x());
        cm.insert(QStringLiteral("w"), m_colRects[i].width());
        cols.push_back(cm);
    }
    res.insert(QStringLiteral("columns"), cols);
    res.insert(QStringLiteral("contentsH"), contentHeight());
    res.insert(QStringLiteral("measureH"), m_measureHeight);
    return res;
}

QVariantMap ChartViewItem::noteAt(qreal x, qreal y) const {
    QVariantMap res;
    res.insert(QStringLiteral("valid"), false);
    const ChartSession* cs = sessionObj();
    if (!cs || !cs->chart() || m_measureHeight <= 0.0 || y < 18.0) return res;
    const beatbench::Chart& chart = *cs->chart();
    const qreal noteH = noteHeight();
    const int measure = static_cast<int>(std::floor(measureAt(y)));
    if (measure < 0 || measure >= cs->measureCount()) return res;
    for (const auto& ev : chart.notes) {
        if (static_cast<int>(ev.measure) < measure - 1 ||
            static_cast<int>(ev.measure) > measure + 1)
            continue;  // 只查相邻小节（控制开销；与 hover 一致）
        const int col = columnFor(ev.value.lane, ev.value.sample.id,
                                  static_cast<int>(ev.value.bgm_line));
        if (col < 0 || static_cast<std::size_t>(col) >= m_colRects.size()) continue;
        const QRectF& r = m_colRects[static_cast<std::size_t>(col)];
        const qreal ny = yOf(ev.measure + posDouble(ev.pos));
        const QRectF nr(r.x() + kNoteHMargin, ny - noteH,
                        r.width() - 2.0 * kNoteHMargin, noteH);
        if (!nr.contains(QPointF(x, y))) continue;
        res.insert(QStringLiteral("valid"), true);
        res.insert(QStringLiteral("measure"), static_cast<int>(ev.measure));
        QVariantMap pos;
        pos.insert(QStringLiteral("num"), static_cast<qlonglong>(ev.pos.num));
        pos.insert(QStringLiteral("den"), static_cast<qlonglong>(ev.pos.den));
        res.insert(QStringLiteral("pos"), pos);
        QVariantMap lane;
        lane.insert(QStringLiteral("player"), QVariant::fromValue(ev.value.lane.player));
        lane.insert(QStringLiteral("kind"),
                    ev.value.lane.kind == beatbench::LaneKind::Scratch
                        ? QStringLiteral("scratch")
                        : ev.value.lane.kind == beatbench::LaneKind::Pedal
                              ? QStringLiteral("pedal")
                              : ev.value.lane.kind == beatbench::LaneKind::Bgm
                                    ? QStringLiteral("bgm")
                                    : QStringLiteral("key"));
        lane.insert(QStringLiteral("index"), QVariant::fromValue(ev.value.lane.index));
        res.insert(QStringLiteral("lane"), lane);
        res.insert(QStringLiteral("sample"), static_cast<int>(ev.value.sample.id));
        // BGM 行序号（同值多行 Bgm note 消歧；前端选中/删除/移动必带）
        res.insert(QStringLiteral("bgm_line"), static_cast<int>(ev.value.bgm_line));
        // LN 选取模式（默认关）：命中 LN 任一段 → 返回配对段（lnPartner），
        // 前端据此自动多选两端（用户问题5）。配对段 = ln_pair 下标指向的 note。
        if (m_lnSelectMode && ev.value.ln_pair && *ev.value.ln_pair < chart.notes.size()) {
            const auto& p = chart.notes[*ev.value.ln_pair];
            QVariantMap pr;
            pr.insert(QStringLiteral("measure"), static_cast<int>(p.measure));
            QVariantMap ppos;
            ppos.insert(QStringLiteral("num"), static_cast<qlonglong>(p.pos.num));
            ppos.insert(QStringLiteral("den"), static_cast<qlonglong>(p.pos.den));
            pr.insert(QStringLiteral("pos"), ppos);
            QVariantMap plane;
            plane.insert(QStringLiteral("player"), QVariant::fromValue(p.value.lane.player));
            plane.insert(QStringLiteral("kind"),
                         p.value.lane.kind == beatbench::LaneKind::Scratch
                             ? QStringLiteral("scratch")
                             : p.value.lane.kind == beatbench::LaneKind::Pedal
                                   ? QStringLiteral("pedal")
                                   : p.value.lane.kind == beatbench::LaneKind::Bgm
                                         ? QStringLiteral("bgm")
                                         : QStringLiteral("key"));
            plane.insert(QStringLiteral("index"), QVariant::fromValue(p.value.lane.index));
            pr.insert(QStringLiteral("lane"), plane);
            pr.insert(QStringLiteral("sample"), static_cast<int>(p.value.sample.id));
            pr.insert(QStringLiteral("bgm_line"), static_cast<int>(p.value.bgm_line));
            res.insert(QStringLiteral("lnPartner"), pr);
        }
        return res;
    }
    return res;
}

QVariantList ChartViewItem::notesInRect(qreal x0, qreal y0, qreal x1, qreal y1) const {
    QVariantList out;
    const ChartSession* cs = sessionObj();
    if (!cs || !cs->chart()) return out;
    const QRectF sel(QPointF(std::min(x0, x1), std::min(y0, y1)),
                     QPointF(std::max(x0, x1), std::max(y0, y1)));
    const qreal mfLo = std::min(measureAt(sel.top()), measureAt(sel.bottom()));
    const qreal mfHi = std::max(measureAt(sel.top()), measureAt(sel.bottom()));
    const int m0 = std::max(0, static_cast<int>(std::floor(mfLo)) - 1);
    const int m1 = std::min(cs->measureCount() - 1,
                            static_cast<int>(std::ceil(mfHi)) + 1);
    const beatbench::Chart& chart = *cs->chart();
    const qreal noteH = noteHeight();
    std::vector<QVariantMap> hits;
    for (const auto& ev : chart.notes) {
        if (static_cast<int>(ev.measure) < m0 || static_cast<int>(ev.measure) > m1) continue;
        const int col = columnFor(ev.value.lane, ev.value.sample.id,
                                  static_cast<int>(ev.value.bgm_line));
        if (col < 0 || static_cast<std::size_t>(col) >= m_colRects.size()) continue;
        const QRectF& r = m_colRects[static_cast<std::size_t>(col)];
        const qreal y = yOf(ev.measure + posDouble(ev.pos));
        const QRectF nr(r.x() + kNoteHMargin, y - noteH,
                        r.width() - 2.0 * kNoteHMargin, noteH);
        if (!nr.intersects(sel)) continue;
        QVariantMap ref;
        ref.insert(QStringLiteral("measure"), static_cast<int>(ev.measure));
        QVariantMap pos;
        pos.insert(QStringLiteral("num"), static_cast<qlonglong>(ev.pos.num));
        pos.insert(QStringLiteral("den"), static_cast<qlonglong>(ev.pos.den));
        ref.insert(QStringLiteral("pos"), pos);
        QVariantMap lane;
        lane.insert(QStringLiteral("player"), QVariant::fromValue(ev.value.lane.player));
        lane.insert(QStringLiteral("kind"),
                    ev.value.lane.kind == beatbench::LaneKind::Scratch
                        ? QStringLiteral("scratch")
                        : ev.value.lane.kind == beatbench::LaneKind::Pedal
                              ? QStringLiteral("pedal")
                              : ev.value.lane.kind == beatbench::LaneKind::Bgm
                                    ? QStringLiteral("bgm")
                                    : QStringLiteral("key"));
        lane.insert(QStringLiteral("index"), QVariant::fromValue(ev.value.lane.index));
        ref.insert(QStringLiteral("lane"), lane);
        ref.insert(QStringLiteral("sample"), static_cast<int>(ev.value.sample.id));
        ref.insert(QStringLiteral("bgm_line"), static_cast<int>(ev.value.bgm_line));
        hits.push_back(std::move(ref));
    }
    // 稳定升序（(measure,pos) → lane → sample），满足「顺序无关」的前提下给可预期结果
    std::stable_sort(hits.begin(), hits.end(), [](const QVariantMap& a, const QVariantMap& b) {
        const qlonglong am = a[QStringLiteral("measure")].toLongLong();
        const qlonglong bm = b[QStringLiteral("measure")].toLongLong();
        if (am != bm) return am < bm;
        const auto an = a[QStringLiteral("pos")].toMap()[QStringLiteral("num")].toLongLong();
        const auto bn = b[QStringLiteral("pos")].toMap()[QStringLiteral("num")].toLongLong();
        if (an != bn) return an < bn;
        const auto ad = a[QStringLiteral("pos")].toMap()[QStringLiteral("den")].toLongLong();
        const auto bd = b[QStringLiteral("pos")].toMap()[QStringLiteral("den")].toLongLong();
        return ad != bd ? an * bd < bn * ad
                        : a[QStringLiteral("lane")]
                                  .toMap()[QStringLiteral("index")]
                                  .toLongLong() <
                              b[QStringLiteral("lane")]
                                  .toMap()[QStringLiteral("index")]
                                  .toLongLong();
    });
    for (auto& m : hits) out.push_back(std::move(m));
    return out;
}

void ChartViewItem::setNoteSampleMode(int v) {
    const int clamped = std::clamp(v, 0, 2);
    if (m_noteSampleMode == clamped) return;
    m_noteSampleMode = clamped;
    emit noteSampleModeChanged();
    update();
}

void ChartViewItem::setLnSelectMode(bool v) {
    if (m_lnSelectMode == v) return;
    m_lnSelectMode = v;
    emit lnSelectModeChanged();
    update();
}

void ChartViewItem::setShowExtras(bool v) {
    if (m_showExtras == v) return;
    m_showExtras = v;
    rebuildColumns();
    emit showExtrasChanged();
    update();
}

void ChartViewItem::setPerfLog(bool v) {
    if (m_perfLog == v) return;
    m_perfLog = v;
    emit perfLogChanged();
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
            text = QString::asprintf("%03d:%.2f", measure, mf - measure);
            // 命中 note → 追加 轨道 + 采样（只查相邻小节，控制开销）
            const beatbench::Chart& chart = *cs->chart();
            const qreal noteH = noteHeight();
            for (std::size_t evIdx = 0; evIdx < chart.notes.size(); ++evIdx) {
                const auto& ev = chart.notes[evIdx];
                if (static_cast<int>(ev.measure) < measure - 1 ||
                    static_cast<int>(ev.measure) > measure + 1)
                    continue;
                const int col = columnFor(ev.value.lane, ev.value.sample.id,
                                  static_cast<int>(ev.value.bgm_line));
                if (col < 0 || static_cast<std::size_t>(col) >= m_colRects.size())
                    continue;
                const QRectF& r = m_colRects[col];
                const qreal y = yOf(ev.measure + posDouble(ev.pos));
                const QRectF hit(r.x() + kNoteHMargin, y - noteH,
                                 r.width() - 2.0 * kNoteHMargin, noteH);
                if (hit.contains(pos)) {
                    // 2026-09（问题5）：显示**实际值**（note 真实 pos/通道/LN 通道），
                    // 而非鼠标位置或合并显示。格式（用户示例）：
                    //   003:0.31(1/4).3+LN(58).#WAV03
                    //   └ mouse pos ┘└实际pos┘└轨┘└LN通道┘└采样┘
                    QString laneText = m_columns[col].label;
                    if (ev.value.lane.kind == beatbench::LaneKind::Bgm && !m_bgmExpanded) {
                        laneText += QStringLiteral("·bgm%1").arg(
                            static_cast<int>(ev.value.bgm_line) + 1);
                    }
                    // note 实际 pos：(num/den) 分数
                    const QString posText =
                        QStringLiteral("(%1/%2)").arg(ev.pos.num).arg(ev.pos.den);
                    // LN：附加实际 BMS 通道（51-69 = 1P/2P LN 通道，LNTYPE 1）
                    // 2026-09：看**数据真值**而非配对状态——未配对的 LN 通道 note
                    // 也应显示 +LN(xx)（用户：实际确实是 LN 通道，只是缺伙伴）。
                    // ln_channel = 源通道就是 5x/6x（parser 只对 51-69 置位）；
                    // LNTYPE 2（#LNOBJ）配对 → ln_channel=false → 显示普通通道号。
                    QString lnText;
                    bool paired = ev.value.ln_pair && *ev.value.ln_pair < chart.notes.size() &&
                        chart.notes[*ev.value.ln_pair].value.ln_pair &&
                        *chart.notes[*ev.value.ln_pair].value.ln_pair == evIdx;
                    if (ev.value.ln_channel || paired) {
                        const std::string ch = beatbench::bms::bms_channel_for(
                            ev.value.lane, ev.value.ln_channel, NoteKind::Normal);
                        if (!ch.empty())
                            lnText = QStringLiteral("+LN(%1)").arg(QString::fromStdString(ch));
                    }
                    text += QStringLiteral(" · ") + posText + QStringLiteral(".") + laneText +
                            lnText;
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

qreal ChartViewItem::noteHeight() const {
    return std::clamp(m_measureHeight * kNoteHeightScale, kNoteHeightMin, kNoteHeightMax);
}

QColor ChartViewItem::noteColor(const beatbench::Lane& lane) const {
    const ThemeManager* th = themeObj();
    if (!th) return QColor(QStringLiteral("#8b9cf8"));
    // 用户配色（2026-09）：S(皿 ch16)=红；key 1/3/5/7(ch11/13/15/19)=白；其它 key 循环浅色。
    // ⚠️ 颜色全部收敛到 ThemeManager token（keyOdd/scratchNote/bgmNote/n1..n4）——
    //    换皮肤/贴图从 theme.json 注入（2026-09 用户问）。
    if (lane.kind == beatbench::LaneKind::Scratch)
        return th->scratchNote();  // 红（皿，高对比）
    if (lane.kind == beatbench::LaneKind::Key) {
        if (lane.index % 2 == 1)  // 键 1/3/5/7（奇数）→ 白（ch11/13/15/19）
            return th->keyOdd();
        // 偶数键（2/4/6/8）：保持浅紫蓝循环（与背景区分）
        switch (lane.index % 4) {
            case 2: return th->n2();
            default: return th->n1();
        }
    }
    if (lane.kind == beatbench::LaneKind::Bgm)
        return th->bgmNote();  // 背景轨 → 绿（与 BGA 层一致,自动播放）
    return th->textMuted();  // Pedal 等罕见轨：弱化色
}

QColor ChartViewItem::noteColor(const beatbench::Lane& lane, const beatbench::Note& note) const {
    QColor c = noteColor(lane);
    // LN 通道 note（ln_channel）加深 35%——包括未配完整的深色单点（lint 提示），
    // 与普通单点直接分辨（2026-09 用户）。
    if (note.ln_channel) c = c.darker(135);
    return c;
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

int ChartViewItem::columnFor(const beatbench::Lane& lane, std::uint32_t bgmSampleId,
                             int bgmLine) const {
    // 第一遍：找 BGM 列（聚合 bgmLine==-1 命中所有；展开列按 bgmLine 精确匹配）；
    // 跳过 BGA 图层列。非 BGM lane 按 lane 匹配。
    for (std::size_t i = 0; i < m_columns.size(); ++i) {
        const auto& c = m_columns[i];
        if (c.bgaLayer >= 0) continue;  // BGA 图层列不匹配 note
        if (c.lane != lane) continue;
        if (c.bgm) {
            // 聚合列（bgmLine==-1）：命中所有 Bgm note；展开列（bgmLine>=0）：按行号匹配
            if (c.bgmLine < 0) return static_cast<int>(i);
            if (c.bgmLine == bgmLine) return static_cast<int>(i);
            continue;
        }
        return static_cast<int>(i);
    }
    // 第二遍（2026-09 用户反馈）：Bgm note 拖到 BGA 图层列后，若无 BGM 列匹配，
    // 回退画到 BGA 图层列（第一个），**否则 note 无处渲染 =「消失」**。
    // 注：BGA 层列 lane 也是 {0,Bgm,0}；同 lane 但无 BGM 列时用它兜底显示。
    if (lane.kind == beatbench::LaneKind::Bgm) {
        for (std::size_t i = 0; i < m_columns.size(); ++i) {
            if (m_columns[i].bgaLayer >= 0 && m_columns[i].lane == lane)
                return static_cast<int>(i);
        }
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
                                    bool bpm = false, bool stop = false,
                                    int bgmLine = -1) {
                m_columns.push_back(
                    {lane, label, bgmCol, p2, bgmId, bgmLine, bgaLayer, bpm, stop});
            };
            // 元事件轨（固定置左、轨窄，iBMSC 式）：BPM / STOP——始终显示（无事件也要能看拍位）
            add({0, beatbench::LaneKind::Pedal, 0}, QStringLiteral("BPM"), false, false, 0, -1,
                true);
            add({0, beatbench::LaneKind::Pedal, 0}, QStringLiteral("STOP"), false, false, 0, -1,
                false, true);
            // 1P：皿 → 键 1..max → 踏板；2P 同构紧随（用户反馈 2026-09：2P 不应排到 BGA/BGM 后）；
            // 之后 BGA 图层（更多轨道，iBMSC 式）；BGM 轨最后（展开列 = 后台音轨目录）。
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
                    // 2026-09 用户确认：BGM 展开 = 按 ch01 **行序**分列（同小节多次读到的
                    // 01 通道 = 独立背景音轨），非按 #WAV id。行数 = 各 measure 的最大
                    // bgm_line+1（空行占位）；note 按 bgm_line 落列。
                    std::uint32_t maxLine = 0;
                    int maxLineSaw = -1;
                    for (const auto& ev : chart->notes) {
                        if (ev.value.lane.kind != beatbench::LaneKind::Bgm) continue;
                        if (static_cast<int>(ev.value.bgm_line) > maxLineSaw) {
                            maxLineSaw = static_cast<int>(ev.value.bgm_line);
                            maxLine = ev.value.bgm_line;
                        }
                    }
                    for (std::uint32_t line = 0; line <= maxLine; ++line) {
                        const QString label =
                            QStringLiteral("bgm%1").arg(static_cast<int>(line + 1));
                        add(bgmLane, label, true, false, 0, -1, false, false, line);
                    }
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
    QElapsedTimer perfTimer;
    if (m_perfLog) perfTimer.start();
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
        // 槽位弱线（= snap 粒度 snapNum/snapDen 小节；BMS 时间单位 = 切分槽位，显示与吸附同源。
        // snapDen/snapNum > 64 粒度过密（如 1/192），只画小节线不画弱线）
        const int num = std::max(1, m_snapNum), den = std::max(1, m_snapDen);
        const int slotCount = std::max(1, den / num);  // 每小节槽数（整数步长）
        if (slotCount > 1 && slotCount <= 64) {
            for (int i = 1; i < slotCount; ++i) {
                p->setPen(weak);
                p->drawLine(QPointF(metaRight, yOf(m + i / static_cast<qreal>(slotCount))),
                            QPointF(w, yOf(m + i / static_cast<qreal>(slotCount))));
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
    const qreal noteH = noteHeight();
    // 选中高亮：NoteRef 键集（框选/粘贴后 QML 回填 selection）
    std::unordered_set<std::string> selKeys;
    if (!m_selection.isEmpty()) {
        for (const auto& item : m_selection) {
            const QVariantMap m = item.toMap();
            const QVariantMap p = m.value(QStringLiteral("pos")).toMap();
            const QVariantMap l = m.value(QStringLiteral("lane")).toMap();
            const beatbench::Rational pos(p.value(QStringLiteral("num")).toLongLong(),
                                          p.value(QStringLiteral("den")).toLongLong());
            const QString kind = l.value(QStringLiteral("kind")).toString();
            beatbench::Lane lane;
            lane.player = static_cast<std::uint8_t>(
                l.value(QStringLiteral("player")).toLongLong());
            lane.index = static_cast<std::uint8_t>(
                l.value(QStringLiteral("index")).toLongLong());
            if (kind == QStringLiteral("scratch")) lane.kind = beatbench::LaneKind::Scratch;
            else if (kind == QStringLiteral("pedal")) lane.kind = beatbench::LaneKind::Pedal;
            else if (kind == QStringLiteral("bgm")) lane.kind = beatbench::LaneKind::Bgm;
            selKeys.insert(noteRefKey(static_cast<std::uint32_t>(
                                          m.value(QStringLiteral("measure")).toLongLong()),
                                      pos, lane,
                                      static_cast<std::uint32_t>(
                                          m.value(QStringLiteral("sample")).toLongLong()))
                               .toStdString());
        }
    }
    const auto isSelected = [&](const beatbench::Event<beatbench::Note>& ev) {
        return !selKeys.empty() &&
               selKeys.count(noteRefKey(ev.measure, ev.pos, ev.value.lane,
                                        ev.value.sample.id)
                                 .toStdString()) > 0;
    };
    // 描边标签（note id/文件名/控制轨）：文字居中于格子、允许越出 note（不裁剪——窄 note/矮 note
    // 也可读）。性能：raster 引擎下 QPainterPath(文本) 每帧每标签构建+描边+填充极慢（用户反馈
    // 文件名模式 <10fps）→ 改为 8 方向 0.6px 偏移 drawText（快）。2026-09 用户：**id 与文件名都
    // 要描边**（原来 id 模式贴 note 内裁剪、无描边，note 窄时看不清）→ 统一走 halo，不再区分
    // 短文本单次深色绘制（短文本也在亮 note 上叠同色 halo，贴深底溢出部分才可见，无副作用）。
    const auto drawHaloLabel = [&](const QRectF& cell, const qreal y, const QString& label,
                                   const QColor& halo) {
        if (label.isEmpty()) return;
        p->setFont(m_noteLabelFont);
        const QFontMetrics fm(m_noteLabelFont);
        const int tw = fm.horizontalAdvance(label);
        const qreal baseline = (y - noteH / 2.0) + (fm.ascent() - fm.descent()) / 2.0;
        const QPointF bp(cell.center().x() - tw / 2.0, baseline);
        // 8 方向浅色描边 + 中心深色（深底也清晰；亮 note 上描边同色不可见，无副作用）
        static constexpr qreal kR = 0.6;
        p->setPen(halo);
        for (int i = 0; i < 8; ++i) {
            const qreal dx = (i % 3 - 1) * kR;
            const qreal dy = (i / 3 - 1) * kR;
            if (dx == 0 && dy == 0) continue;
            p->drawText(bp + QPointF(dx, dy), label);
        }
        p->setPen(th->bg());
        p->drawText(bp, label);
    };
    // note 内采样标签：id 只显示 00-ZZ（无 #WAV 前缀）；文件名去扩展名。两者统一走 drawHaloLabel
    //（带描边、不裁剪——2026-09 用户：原来 id 模式贴 note 内裁剪，note 窄/矮时看不清）。
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
        // id / 文件名统一：居中、带描边、不裁剪（窄/矮 note 也可读）。
        drawHaloLabel(colRect, y, label, noteColor(note.lane));
    };
    m_lastVisibleNotes = 0;
    for (std::size_t i = 0; i < chart.notes.size(); ++i) {
        const auto& ev = chart.notes[i];
        if (static_cast<int>(ev.measure) < first || static_cast<int>(ev.measure) > last)
            continue;
        ++m_lastVisibleNotes;
        const int col = columnFor(ev.value.lane, ev.value.sample.id,
                                  static_cast<int>(ev.value.bgm_line));
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
            if (isSelected(ev)) {
                p->setPen(QPen(th->onAccent(), 1.2));
                p->setBrush(Qt::NoBrush);
                p->drawPath(path);
            }
            continue;
        }

        if (note.ln_pair) {
            const auto& partner = chart.notes[*note.ln_pair];
            // 2026-09（用户反馈）：LN 绘制不再依赖「谁是头尾」的固定角色——
            // ① 每端在**自己列**画自己的帽（移动后尾帽在尾的新列/新 y）；
            // ② 体（中段线）由**时间早的端**画：从早端 y 到晚端 y（画在早端列中心线，
            //    hi-top 下晚端在上 → 体跨 min..max）；
            // ③ 选中框每端都画（头尾均可见时有明确高亮）。
            const bool isEarlier =
                ev.measure < partner.measure ||
                (ev.measure == partner.measure &&
                 posDouble(ev.pos) < posDouble(partner.pos));
            // 自己帽（各列）；LN note 用加深色（2026-09）
            p->fillRect(QRectF(r.x() + kNoteHMargin, y - noteH,
                               r.width() - 2.0 * kNoteHMargin, noteH),
                        noteColor(note.lane, note));
            drawSampleLabel(r, y, note);
            // 体：时间早端画（连接两端 y；画在早端列）。
            // ⚠️ 防呆（2026-09 用户）：仅当两端**同通道**（同 lane + 非 BGM 或同 bgm_line）
            // 才画中段线——跨通道移动 LN 端后 ln_pair 已被 rebuild 清空，此处不再残留连线；
            // 若因异常残留跨通道 ln_pair，也**不画**（帽各自显示、lint 提示未配对）。
            const bool same_channel = note.lane == partner.value.lane &&
                (note.lane.kind != LaneKind::Bgm ||
                 note.bgm_line == partner.value.bgm_line);
            if (isEarlier && same_channel) {
                const qreal y2 = yOf(partner.measure + posDouble(partner.pos));
                // 早端列（本端列 r）；若 partner 在同列则中段线直接连，跨列时也画在本列
                const qreal ya = std::min(y, y2), yb = std::max(y, y2);
                p->fillRect(QRectF(r.center().x() - 3.0, ya, 6.0,
                                   std::max<qreal>(yb - ya, 4.0)),
                            th->ln());
            }
            // 选中框（每端）
            if (isSelected(ev)) {
                p->setPen(QPen(th->onAccent(), 1.2));
                p->setBrush(Qt::NoBrush);
                p->drawRect(QRectF(r.x() + kNoteHMargin - kNoteSelectExpand,
                                   y - noteH - 1.0,
                                   r.width() - 2.0 * kNoteHMargin + 2.0 * kNoteSelectExpand,
                                   noteH + 2.0));
            }
            continue;
        }

        // 普通单点（含 LN 通道但未配对：深色 + 无连线——数据已是 LN 通道，仅缺伙伴）
        p->fillRect(QRectF(r.x() + kNoteHMargin, y - noteH,
                           r.width() - 2.0 * kNoteHMargin, noteH),
                    noteColor(note.lane, note));
        drawSampleLabel(r, y, note);
        if (isSelected(ev)) {
            p->setPen(QPen(th->onAccent(), 1.2));
            p->setBrush(Qt::NoBrush);
            p->drawRect(QRectF(r.x() + kNoteHMargin - kNoteSelectExpand,
                               y - noteH - 1.0,
                               r.width() - 2.0 * kNoteHMargin + 2.0 * kNoteSelectExpand,
                               noteH + 2.0));
        }
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
            // 用户配色（2026-09）：BGA 层四列（04/06/07/0A）用绿色系；层间以深浅区分保对比。
            // ⚠️ 颜色收敛到 ThemeManager token（bgaBase/bgaPoor/bgaLayer/bgaLayer2）。
            QColor c;
            switch (ev.value.layer) {
                case 1: c = th->bgaPoor(); break;      // poor：深绿
                case 2: c = th->bgaLayer(); break;     // layer：亮绿
                case 3: c = th->bgaLayer2(); break;    // layer2：中绿
                default: c = th->bgaBase(); break;     // base：浅绿
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

    // ---- 性能采样（--perf-log；每帧一条落到 Qt 消息日志，无交互时帧少不丢数据） ----
    if (m_perfLog && perfTimer.isValid()) {
        qInfo("perf: paint=%lldms cols=%zu notes_visible=%d h=%f",
              static_cast<long long>(perfTimer.elapsed()), m_columns.size(),
              m_lastVisibleNotes, height());
    }
}

}  // namespace beatbench::app
