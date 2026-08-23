// SPDX-License-Identifier: GPL-3.0-only
// 竖向时间轴自绘视口（M2 第 5 步，doc/07 §3 步 5）：QQuickPaintedItem + QPainter。
// 绘制（v1）：小节标尺（小节号 + BPM 变化标记 + STOP 段）、槽位网格、轨道列
// （皿/1-7/BGM/2P，按谱面实际出现的 Lane 数据驱动）、note / LN 头尾 / 地雷
// 按 (measure,pos) 落位；方向默认「顶部=高小节」（preview.html，note 自上而下落）。
// 秒标尺与 BGM 波形铺底后置 Phase B（doc/07 §4）。
// 数据源 = ChartSession（core Chart + TimingEngine 真数据）；只读展示，无编辑（M3）。
// 滚动/缩放由 QML 侧 ChartView.qml 驱动（Flickable 无 onWheel，doc/04 §5）。
// 类型经 QML_ELEMENT 注册为 BeatBench 模块组件（皮肤 L3 按组件覆写的落点，doc/05 §8）。
#pragma once

#include <QColor>
#include <QFont>
#include <QQuickPaintedItem>
#include <QtQml/qqmlregistration.h>

#include <vector>

#include "beatbench/core/Chart.hpp"
#include "beatbench/core/Lane.hpp"

class QPainter;

namespace beatbench::app {

class ChartSession;
class ThemeManager;

class ChartViewItem : public QQuickPaintedItem {
    Q_OBJECT
    QML_ELEMENT

    Q_PROPERTY(QObject* session READ session WRITE setSession NOTIFY sessionChanged)
    Q_PROPERTY(QObject* theme READ theme WRITE setTheme NOTIFY themeChanged)
    Q_PROPERTY(qreal measureHeight READ measureHeight WRITE setMeasureHeight NOTIFY measureHeightChanged)
    Q_PROPERTY(qreal rulerWidth READ rulerWidth WRITE setRulerWidth NOTIFY rulerWidthChanged)
    Q_PROPERTY(qreal laneWidth READ laneWidth WRITE setLaneWidth NOTIFY laneWidthChanged)
    Q_PROPERTY(qreal scrollY READ scrollY WRITE setScrollY NOTIFY scrollYChanged)
    Q_PROPERTY(qreal contentHeight READ contentHeight NOTIFY contentHeightChanged)
    Q_PROPERTY(bool topHigh READ topHigh WRITE setTopHigh NOTIFY topHighChanged)
    /// 吸附粒度 = snapNum/snapDen 小节（分子分母皆可调；1/16 = 每小节 16 槽，3/16 = 3/16 步长）。
    Q_PROPERTY(int snapNum READ snapNum WRITE setSnapNum NOTIFY snapNumChanged)
    Q_PROPERTY(int snapDen READ snapDen WRITE setSnapDen NOTIFY snapDenChanged)
    Q_PROPERTY(int columnCount READ columnCount NOTIFY columnCountChanged)
    /// BGM 列点击展开 → 按 #WAV id 从小到大分列（iBMSC 式「背景轨分开显示」，BMS 笔记 ch01 注）
    Q_PROPERTY(bool bgmExpanded READ bgmExpanded WRITE setBgmExpanded NOTIFY bgmExpandedChanged)
    /// 轨道列头显示实际 BMS 通道 id（皿=16、键1=11…；工具条勾选 / Ctrl 临时，doc debug 用）
    Q_PROPERTY(bool showChannelIds READ showChannelIds WRITE setShowChannelIds NOTIFY showChannelIdsChanged)
    /// note 上显示所用采样：0=隐藏 1=显示 id（01） 2=显示文件名（#WAVxx 文件）
    Q_PROPERTY(int noteSampleMode READ noteSampleMode WRITE setNoteSampleMode NOTIFY noteSampleModeChanged)
    /// LN 选取模式（默认关）：开启后点选 LN 任一段，noteAt 返回配对的两段（lnPartner）。
    Q_PROPERTY(bool lnSelectMode READ lnSelectMode WRITE setLnSelectMode NOTIFY lnSelectModeChanged)
    /// 显示更多轨道（BGA 图层通道列，位于游玩轨与背景轨之间；iBMSC 式，doc 参考 local/doc 截图）
    Q_PROPERTY(bool showExtras READ showExtras WRITE setShowExtras NOTIFY showExtrasChanged)
    /// 水平滚动（列区；轨道列超宽时用——底部滚动条 / Shift+滚轮 / Shift+拖拽）
    Q_PROPERTY(qreal scrollX READ scrollX WRITE setScrollX NOTIFY scrollXChanged)
    Q_PROPERTY(qreal contentWidth READ contentWidth NOTIFY contentWidthChanged)
    /// 鼠标位置 + note 信息（状态栏展示；空 = 未悬停）
    Q_PROPERTY(QString hoverText READ hoverText NOTIFY hoverChanged)
    /// 选中 note 集合（NoteRef 语义：measure/pos/lane/sample；框选/粘贴后回填，绘制高亮）
    Q_PROPERTY(QVariantList selection READ selection WRITE setSelection NOTIFY selectionChanged)
    /// 性能检测：paint 帧耗时采样落日志（--perf-log；每 20 帧一条）
    Q_PROPERTY(bool perfLog READ perfLog WRITE setPerfLog NOTIFY perfLogChanged)

public:
    explicit ChartViewItem(QQuickItem* parent = nullptr);

    void paint(QPainter* painter) override;

    QObject* session() const { return m_session; }
    void setSession(QObject* session);
    QObject* theme() const { return m_theme; }
    void setTheme(QObject* theme);

    qreal measureHeight() const { return m_measureHeight; }
    void setMeasureHeight(qreal v);
    qreal rulerWidth() const { return m_rulerWidth; }
    void setRulerWidth(qreal v);
    qreal laneWidth() const { return m_laneWidth; }
    void setLaneWidth(qreal v);
    QVariantList selection() const { return m_selection; }
    void setSelection(const QVariantList& v);
    bool perfLog() const { return m_perfLog; }
    void setPerfLog(bool v);
    qreal scrollY() const { return m_scrollY; }
    void setScrollY(qreal v);
    qreal contentHeight() const;
    bool topHigh() const { return m_topHigh; }
    void setTopHigh(bool v);
    int gridDiv() const { return 0; }  // 已废弃（snapNum/snapDen 取代；保留避免旧绑定崩溃）
    void setGridDiv(int);
    int snapNum() const { return m_snapNum; }
    void setSnapNum(int v);
    int snapDen() const { return m_snapDen; }
    void setSnapDen(int v);
    int columnCount() const { return static_cast<int>(m_columns.size()); }
    bool bgmExpanded() const { return m_bgmExpanded; }
    void setBgmExpanded(bool v);
    bool showChannelIds() const { return m_showChannelIds; }
    void setShowChannelIds(bool v);
    int noteSampleMode() const { return m_noteSampleMode; }
    void setNoteSampleMode(int v);
    bool lnSelectMode() const { return m_lnSelectMode; }
    void setLnSelectMode(bool v);
    bool showExtras() const { return m_showExtras; }
    void setShowExtras(bool v);
    qreal scrollX() const { return m_scrollX; }
    void setScrollX(qreal v);
    qreal contentWidth() const;
    QString hoverText() const { return m_hoverText; }

    /// 命中 BGM 列头（视口顶部横条内）→ 列下标（-1 未命中）；QML 点击用于展开/折叠。
    Q_INVOKABLE int bgmHeaderIndexAt(qreal x) const;

    /// 屏幕坐标 → 可放放置点（M3 note.put 入参）：{valid, measure, num, den,
    /// lanePlayer, laneKind, laneIndex, label[, sampleHint]}。游玩轨（键/皿/踏板）与
    /// BGM 轨可放置（展开列带 sampleHint = 固定 id）；列头/元轨/BGA 无效。
    Q_INVOKABLE QVariantMap hitTest(qreal x, qreal y) const;

    /// 屏幕矩形内 note 枚举（clipboard.copy 的 selection 数组：{measure, pos:{num,den},
    /// lane:{player,kind,index}, sample}；按 (measure,pos) 稳定升序）。
    Q_INVOKABLE QVariantList notesInRect(qreal x0, qreal y0, qreal x1, qreal y1) const;

    /// 屏幕坐标 → 命中的 note（NoteRef 形状：{measure, pos:{num,den}, lane:{...}, sample}；
    /// valid=false = 空白）。选择/右键删除用；只查相邻小节（与 hover 同开销上限）。
    Q_INVOKABLE QVariantMap noteAt(qreal x, qreal y) const;

    /// 屏幕 y → 拍位（measure + pos 小数；时间轴工具（平移等）距离换算用）。
    Q_INVOKABLE qreal measureAtY(qreal y) const;

    /// 屏幕 x → 命中的可放置列（{valid, lanePlayer, laneKind, laneIndex}；横向改轨移动用）。
    /// BPM/STOP 列不可放置 → valid=false。
    Q_INVOKABLE QVariantMap laneAtX(qreal x) const;

    /// 诊断探针（--probe <x> <y>，调试）：返回 noteAt / laneAtX / hitTest 的合成结果，
    /// 用于定位选中/移动/放置的命中问题（如 BGM 背景轨内移动失败）。不含逻辑，仅诊断输出。
    Q_INVOKABLE QVariantMap probe(qreal x, qreal y) const;

signals:
    void sessionChanged();
    void themeChanged();
    void measureHeightChanged();
    void rulerWidthChanged();
    void laneWidthChanged();
    void scrollYChanged();
    void contentHeightChanged();
    void topHighChanged();
    void snapNumChanged();
    void snapDenChanged();
    void gridDivChanged();
    void columnCountChanged();
    void bgmExpandedChanged();
    void showChannelIdsChanged();
    void noteSampleModeChanged();
    void lnSelectModeChanged();
    void showExtrasChanged();
    void scrollXChanged();
    void contentWidthChanged();
    void hoverChanged();
    void selectionChanged();
    void perfLogChanged();
    /// 谱面切换（ChartSession.chartChanged 转发；QML 据此重定位滚动）。
    void chartChanged();

protected:
    void geometryChange(const QRectF& newGeometry, const QRectF& oldGeometry) override;
    void hoverMoveEvent(QHoverEvent* event) override;
    void hoverLeaveEvent(QHoverEvent* event) override;

private:
    struct Column {
        beatbench::Lane lane;
        QString label;
        bool bgm = false;            // 背景音轨列（ch01）
        bool p2 = false;             // 2P 列（浅主色底）
        std::uint32_t bgmId = 0;     // 遗留：按 #WAV id 分列（0 = 聚合）——已被 bgmLine 取代
        int bgmLine = -1;            // BGM 展开列行号（2026-09：按 ch01 行序分列，非按 id；
                                     // 0..N-1 = bgm1..bgmN；-1 = 聚合列/非 BGM）
        int bgaLayer = -1;           // BGA 图层列（0=base 1=poor 2=layer 3=layer2；-1 = 非 BGA）
        bool bpm = false;            // BPM 元事件轨（窄列，iBMSC 式；不随 scrollX 滚动）
        bool stop = false;           // STOP 元事件轨（同上）
    };

    void onSessionChartChanged();
    void onSessionContentChanged();
    void updateHover(const QPointF& pos);
    ChartSession* sessionObj() const;
    ThemeManager* themeObj() const;
    /// NoteRef 语义键（measure|num|den|player|kind|index|sample）：选中判定用。
    static QString noteRefKey(std::uint32_t measure, const beatbench::Rational& pos,
                              const beatbench::Lane& lane, std::uint32_t sample = 0);

    /// 轨道列重算（谱面切换/尺寸/展开状态变化时；按谱面实际出现的 Lane 数据驱动）。
    void rebuildColumns();
    int columnFor(const beatbench::Lane& lane, std::uint32_t bgmSampleId = 0,
                  int bgmLine = -1) const;
    /// BGA 事件 → 图层列（-1 = 无该层列，如 showExtras 关闭）。
    int columnForBga(int layer) const;
    void clampScrollX();
    /// 列头文本：showChannelIds 时 = BMS 实际通道号（bms_channel_for 反向映射）。
    QString columnLabel(const beatbench::Lane& lane, const QString& displayName) const;
    /// 列宽：BPM/STOP 元事件轨 = m_metaTrackWidth（窄于普通轨道），其余 = m_laneWidth。
    qreal columnWidth(std::size_t i) const;
    /// 固定元事件轨（BPM/STOP）总宽（不加 scrollX / 不参与居中）。
    qreal metaTrackWidth() const;

    /// 拍位（measureFloat = measure + pos，0 起）→ 屏幕 y（含方向翻转与滚动）。
    qreal yOf(qreal measureFloat) const;
    /// 屏幕 y → 拍位（yOf 的逆；状态栏鼠标位置用）。
    qreal measureAt(qreal screenY) const;
    qreal posDouble(const beatbench::Rational& r) const;
    void clampScroll();

    QColor noteColor(const beatbench::Lane& lane) const;
    double bpmAt(const beatbench::Chart& chart, int measure) const;
    double beatsOf(const beatbench::Chart& chart, int measure) const;

    void drawHint(QPainter* p, const QString& text);

    QObject* m_session = nullptr;
    QObject* m_theme = nullptr;
    qreal m_measureHeight = 96.0;
    qreal m_rulerWidth = 56.0;
    qreal m_laneWidth = 38.0;
    qreal m_metaTrackWidth = 36.0;  // BPM/STOP 元事件轨宽（窄于普通轨道，iBMSC 式）
    qreal m_scrollY = 0.0;
    bool m_topHigh = true;
    int m_snapNum = 1;   // 吸附粒度分子（槽位步长 = snapNum/snapDen 小节）
    int m_snapDen = 16;  // 吸附粒度分母
    bool m_bgmExpanded = false;
    bool m_showChannelIds = false;
    int m_noteSampleMode = 0;  // note 采样标签：0=隐藏 1=id 2=文件名
    bool m_lnSelectMode = false;  // LN 选取模式（默认关）：点 LN 任一段自动返回配对
    bool m_showExtras = false;  // BGA 图层通道列（d场景更多轨道）
    qreal m_scrollX = 0.0;
    bool m_perfLog = false;  // paint 帧耗时采样（--perf-log）
    std::vector<Column> m_columns;
    std::vector<QRectF> m_colRects;  // 最近一次 paint 的列 rect（列头点击命中用）
    QFont m_rulerFont;
    QFont m_noteLabelFont;  // note 内采样标签（9px）
    QString m_hoverText;   // 鼠标位置 + note 信息（状态栏）
    int m_hoverMeasure = -1;
    qreal m_hoverY = -1.0;  // 悬停线（屏幕 y；-1 = 无）
    QVariantList m_selection;  // 选中 note 集合（NoteRef 语义；绘制高亮用）
    int m_lastVisibleNotes = 0;  // 最近一次 paint 的可见 note 数（--perf-log）
};

}  // namespace beatbench::app
