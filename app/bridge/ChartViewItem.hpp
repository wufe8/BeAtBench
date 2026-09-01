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
#include <QImage>
#include <QQuickPaintedItem>
#include <QtQml/qqmlregistration.h>

#include <vector>
#include <unordered_map>

#include "beatbench/core/Chart.hpp"
#include "beatbench/core/Lane.hpp"
#include "beatbench/core/timing/TimingEngine.hpp"

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
    /// 槽位弱线显示开关（「网格」按钮；默认开）。吸附计算不依赖此开关，仅控制画不画槽位线。
    Q_PROPERTY(bool showGrid READ showGrid WRITE setShowGrid NOTIFY showGridChanged)
    /// 水平滚动（列区；轨道列超宽时用——底部滚动条 / Shift+滚轮 / Shift+拖拽）
    Q_PROPERTY(qreal scrollX READ scrollX WRITE setScrollX NOTIFY scrollXChanged)
    Q_PROPERTY(qreal contentWidth READ contentWidth NOTIFY contentWidthChanged)
    /// 鼠标位置 + note 信息（状态栏展示；空 = 未悬停）
    Q_PROPERTY(QString hoverText READ hoverText NOTIFY hoverChanged)
    /// 选中 note 集合（NoteRef 语义：measure/pos/lane/sample；框选/粘贴后回填，绘制高亮）
    Q_PROPERTY(QVariantList selection READ selection WRITE setSelection NOTIFY selectionChanged)
    /// 选中 BGA/BPM/STOP 对象集合（{kind, measure, pos, layer?, sample?, value?}；绘制高亮）
    Q_PROPERTY(QVariantList metaSelection READ metaSelection WRITE setMetaSelection NOTIFY metaSelectionChanged)
    /// 性能检测：paint 帧耗时采样落日志（--perf-log；每 20 帧一条）
    Q_PROPERTY(bool perfLog READ perfLog WRITE setPerfLog NOTIFY perfLogChanged)
    /// M5.2 播放头：当前时间点（秒；负值 = 隐藏线）。暂停/停止也显示（当前时间点位置）。
    Q_PROPERTY(double playheadSec READ playheadSec WRITE setPlayheadSec NOTIFY playheadSecChanged)
    /// M5.2 播放头跟随（QML 开关；用户滚动自动关由 QML 侧置 false）。
    Q_PROPERTY(bool followPlayhead READ followPlayhead WRITE setFollowPlayhead NOTIFY followPlayheadChanged)
    /// M5.2 A-B 循环标记（秒；-1 = 未设）。paint 画虚线标记（A 绿 / B 橙）+ 左侧标签。
    Q_PROPERTY(double loopASec READ loopASec WRITE setLoopASec NOTIFY loopASecChanged)
    Q_PROPERTY(double loopBSec READ loopBSec WRITE setLoopBSec NOTIFY loopBSecChanged)
    /// 开头纯留白小节数（Overlay 画 A/B 标记时须扣留白——否则标记比实际值早 1-2 小节）。
    Q_PROPERTY(qreal leadMeasures READ leadMeasures CONSTANT)
    /// 滚动上限（topHigh 下滚到底 = 小节 0 落播放线 90%；滚动条/跟随 clamp 用）。
    Q_PROPERTY(qreal maxScrollY READ maxScrollY NOTIFY maxScrollYChanged)
    /// M5.2 视口光标（红线、固定视口底部 10%）读数：秒 + 小节号文本（"m"）。
    /// ⚠️ 源 = 「红线下方内容」（measureAt(0.9h)），**非播放时钟**——滚动视口内容滚过
    /// 红线 → 值随视口变；播放中跟随滚动内容 → 值自然前进。状态栏显示用（2026-09 用户）。
    Q_PROPERTY(qreal cursorSec READ cursorSec NOTIFY cursorChanged)
    Q_PROPERTY(QString cursorPosText READ cursorPosText NOTIFY cursorChanged)
    /// M6 编辑预览：放置工具（note/ln/mine）下鼠标悬浮 → 画 ghost note（"normal"/"ln"/"mine"；
    /// 空 = 关闭）。用户 2026-09：操作前先看到落点。
    Q_PROPERTY(QString previewNoteKind READ previewNoteKind WRITE setPreviewNoteKind NOTIFY previewNoteKindChanged)
    /// M6 编辑预览：拖拽移动选中 note → 各画 ghost @ 原始+moveDeltaF（位移拍位小数）。
    Q_PROPERTY(bool movePreview READ movePreview WRITE setMovePreview NOTIFY movePreviewChanged)
    Q_PROPERTY(qreal moveDeltaF READ moveDeltaF WRITE setMoveDeltaF NOTIFY moveDeltaFChanged)
    /// M6 编辑预览：拖拽起点/目标 lane（{player,kind,index}；跨通道移动时 ghost 落到目标列）。
    /// 命中 sourceLane 的 note 用 targetLane 画 ghost；其余只按时间位移。
    Q_PROPERTY(QVariantMap moveSourceLane READ moveSourceLane WRITE setMoveSourceLane NOTIFY moveSourceLaneChanged)
    Q_PROPERTY(QVariantMap moveTargetLane READ moveTargetLane WRITE setMoveTargetLane NOTIFY moveTargetLaneChanged)
    /// M6 编辑预览：拖起 note 的 BGM 子通道行号（-1=非 BGM）。BGM 虚拟子通道多选移动
    /// **保持相对距离**（channelOffset 的 bgm_line 版）——ghost 各 note 落 line = 自身 + 偏移。
    Q_PROPERTY(int moveSourceBgmLine READ moveSourceBgmLine WRITE setMoveSourceBgmLine NOTIFY moveSourceBgmLineChanged)

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
    QVariantList metaSelection() const { return m_metaSelection; }
    void setMetaSelection(const QVariantList& v);
    bool perfLog() const { return m_perfLog; }
    void setPerfLog(bool v);
    double playheadSec() const { return m_playheadSec; }
    void setPlayheadSec(double v);
    bool followPlayhead() const { return m_followPlayhead; }
    void setFollowPlayhead(bool v);
    double loopASec() const { return m_loopASec; }
    void setLoopASec(double v);
    double loopBSec() const { return m_loopBSec; }
    void setLoopBSec(double v);
    qreal leadMeasures() const { return m_leadMeasures; }
    /// M5.2 跟随：**硬锁定**——播放头无条件钉在视口 90%（底部 10% 固定高度）。
    /// QML 每 playbackChanged 调；播放中滚动内容让红线下方 = 播放时钟。
    Q_INVOKABLE bool followPlayheadTick();

    /// M5.2 播放红线的时间读数：红线（视口底部 10% 固定）下方的内容拍位 → 秒。
    /// ⚠️ 剪辑软件语义：红线固定视口，滚动视口内容滚过红线 → 读数随视口变。
    Q_INVOKABLE double playheadReadoutSec() const;
    /// 红线（视口底部 10%）下方内容的时间（秒）。滚动视口 → 内容滚过红线 → 值变。
    qreal cursorSec() const;
    /// 红线下方内容所在**小节号**文本（整数；节内拍位为连续量，精确约分分母太夸张，
    /// 用户「不想跳变或近似」→ 只给精确小节号 + 精确秒；无图/无 timing → 空）。
    QString cursorPosText() const;
    QString previewNoteKind() const { return m_previewNoteKind; }
    void setPreviewNoteKind(const QString& v);
    bool movePreview() const { return m_movePreview; }
    void setMovePreview(bool v);
    qreal moveDeltaF() const { return m_moveDeltaF; }
    void setMoveDeltaF(qreal v);
    QVariantMap moveSourceLane() const { return m_moveSourceLane; }
    void setMoveSourceLane(const QVariantMap& v);
    QVariantMap moveTargetLane() const { return m_moveTargetLane; }
    void setMoveTargetLane(const QVariantMap& v);
    int moveSourceBgmLine() const { return m_moveSourceBgmLine; }
    void setMoveSourceBgmLine(int v);
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
    bool showGrid() const { return m_showGrid; }
    void setShowGrid(bool v);
    qreal scrollX() const { return m_scrollX; }
    void setScrollX(qreal v);
    qreal contentWidth() const;
    QString hoverText() const { return m_hoverText; }

    /// 运行时换肤（doc/08 §3.3）：Theme token 变化后强制重绘（repaint）视口。
    /// QML 侧 catch Theme.tokensChanged 时调用；等价于调用底层 QQuickPaintedItem::update()。
    Q_INVOKABLE void refreshTheme() { update(); }

    /// 命中 BGM 列头（视口顶部横条内）→ 列下标（-1 未命中）；QML 点击用于展开/折叠。
    Q_INVOKABLE int bgmHeaderIndexAt(qreal x) const;

    /// 屏幕坐标 → 可放放置点（M3 note.put 入参）：{valid, measure, num, den,
    /// lanePlayer, laneKind, laneIndex, label[, sampleHint]}。游玩轨（键/皿/踏板）与
    /// BGM 轨可放置（展开列带 sampleHint = 固定 id）；列头/元轨/BGA 无效。
    Q_INVOKABLE QVariantMap hitTest(qreal x, qreal y) const;

    /// 以屏幕 y 为锚点缩放（2026-09：鼠标滚轮缩放时保持鼠标处拍位不动）。
    /// factor > 1 放大；内部换算锚点拍位 → 新 measureHeight → 反推 scrollY。
    Q_INVOKABLE void zoomAt(qreal screenY, qreal factor);

    /// M4.3c 波形总览跳转：秒 → 拍位（TimingEngine::position_at）→ 滚动视口
    /// （目标拍位居中；topHigh 已处理）。无 timing/超出 → no-op。
    Q_INVOKABLE void scrollToTime(double seconds);

    /// 定位到评分起点（小节 0）于播放线（视口 90%；topHigh）/顶部（非 topHigh）。
    /// 开头留白让小节 0 可落到 90% —— 打开谱面/跳转起点用它，避免「播放线在 0-1 小节时
    /// 播放头锁不住 90% → 开始播放跳变」（用户 2026-09）。
    Q_INVOKABLE void scrollToStart();

    /// M5 seek 交互：屏幕 y → 秒（秒标尺/波形条点击定位用）。measureAt(y) → Position →
    /// timing()->time_us / 1e6；无 timing / 留白区（拍位<0）→ 0。与cursorPosition 同源换算。
    Q_INVOKABLE double timeAtY(qreal y) const;

    /// M5 seek 交互：把秒对应的拍位滚到**红线（视口 90%）**下方（=scrollToStart 的任意拍位版）。
    /// 与红线=视口光标模型一致：seek 后红线读数 = 该时间。无 timing / 负秒 → no-op。
    Q_INVOKABLE void scrollCursorToSec(double seconds);

    /// 屏幕矩形内 note 枚举（clipboard.copy 的 selection 数组：{measure, pos:{num,den},
    /// lane:{player,kind,index}, sample}；按 (measure,pos) 稳定升序）。
    Q_INVOKABLE QVariantList notesInRect(qreal x0, qreal y0, qreal x1, qreal y1) const;

    /// 屏幕坐标 → 命中的 note（NoteRef 形状：{measure, pos:{num,den}, lane:{...}, sample}；
    /// valid=false = 空白）。选择/右键删除用；只查相邻小节（与 hover 同开销上限）。
    Q_INVOKABLE QVariantMap noteAt(qreal x, qreal y) const;

    /// 屏幕坐标 → 命中的「位置对象」（note / BGA / BPM / STOP 任一，kind 区分）。
    /// BGA/BPM/STOP 返回：{kind, measure, pos:{num,den}, layer?(bga), sample?(bga),
    /// value?(bpm/stop)}；note 复用 noteAt + kind="note"。valid=false = 空白。
    Q_INVOKABLE QVariantMap objectAt(qreal x, qreal y) const;

    /// 屏幕 y → 拍位（measure + pos 小数；时间轴工具（平移等）距离换算用）。
    Q_INVOKABLE qreal measureAtY(qreal y) const;

    /// 屏幕 x → 命中的可放置列（{valid, lanePlayer, laneKind, laneIndex}；横向改轨移动用）。
    /// 扩展（2026-09 跨命名空间移动）：额外带 bgmLine（BGM 展开列行号；-1 非 BGM）、
    /// bgaLayer（BGA 图层列 0..3；-1 非 BGA）、metaKind（"bpm"/"stop"；空 = 非元事件轨）。
    /// BPM/STOP 列不再拒绝（用户确认「格式可表示 id 就允许移动」——拖到该列 =
    /// note → timing 事件转换）；BGA 图层列同理（note → BGA 事件）。
    Q_INVOKABLE QVariantMap laneAtX(qreal x) const;

    /// 给定 note 引用（{lane:{player,kind,index}, bgm_line?}）→ 该 note 当前**显示列下标**
    /// （columnFor 封装：BGM 展开列按 bgm_line 精确匹配、玩乐列按 lane 匹配；-1 = 无对应列）。
    /// BGM 跨通道相对间距（连续轨道平移）用同一列序自然表达——不再受「bgm_line 对玩乐
    /// note 恒=0」的混淆影响（2026-09 用户：多轨拖进 BGM 应保持相对距离）。
    Q_INVOKABLE int columnIndexForRef(const QVariantMap& ref) const;

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
    void showGridChanged();
    void scrollXChanged();
    void contentWidthChanged();
    void hoverChanged();
    void selectionChanged();
    void metaSelectionChanged();
    void perfLogChanged();
    void playheadSecChanged();
    void followPlayheadChanged();
    void loopASecChanged();
    void loopBSecChanged();
    /// 视口光标（红线）读数变化：scrollY / contentHeight / topHigh / measureHeight /
    /// 尺寸（height）任一变化 → 红线下方内容拍位变 → 状态栏时间随之刷新。
    void cursorChanged();
    /// 滚动上限变化（height/contentHeight/topHigh/measureHeight 任一变化）。
    void maxScrollYChanged();
    void previewNoteKindChanged();
    void movePreviewChanged();
    void moveDeltaFChanged();
    void moveSourceLaneChanged();
    void moveTargetLaneChanged();
    void moveSourceBgmLineChanged();
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
    /// M6 预览：paint 尾部绘制 ghost note（放置 + 移动预览共用）。
    void drawPreview(QPainter* p) const;
    /// 画一个半透明 ghost note（normal 矩形 / mine 菱形 / ln 有帽矩形）。
    void drawNoteGhost(QPainter* p, qreal measureFloat, const beatbench::Lane& lane,
                       const QString& noteKind, std::uint32_t sample, int bgmLine) const;
    /// paint 前按当前 theme 刷新标尺/note 标签字体族（皮肤可换；避免硬编码 Consolas）。
    void applyThemeFonts(const ThemeManager* th);
    /// NoteRef 语义键（measure|num|den|player|kind|index|sample|bgm_line）：选中判定用。
    /// ⚠️ bgm_line 入键：BGM 同位置不同子通道行须彼此区分，否则选中会误高亮整组。
    static QString noteRefKey(std::uint32_t measure, const beatbench::Rational& pos,
                              const beatbench::Lane& lane, std::uint32_t sample = 0,
                              std::uint32_t bgm_line = 0);

    /// BGA/BPM/STOP 对象选中判定（按 kind/measure/pos/layer/sample 键；layer/sample -1 = n/a）。
    bool metaSelected(std::uint32_t measure, const beatbench::Rational& pos,
                      int layer, int sample) const;

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
    /// 拍位 → 内容坐标（不含 scrollY；含开头留白偏移；topHigh 感知）。
    /// contentHeight/yOf/measureAt/scrollToTime/followPlayheadTick 统一用它。
    qreal contentPosOf(qreal measureFloat) const;
    /// 滚动上限：topHigh 下滚到底 = 小节 0 落播放线(90%)——手动滚动不进入留白区，
    /// 否则红线读留白、起播对齐跳变（用户 2026-09「从底部起播仍跳变」）。
    qreal maxScrollY() const;
    /// 屏幕 y → 拍位（yOf 的逆；状态栏鼠标位置用）。留白区返回负拍位。
    qreal measureAt(qreal screenY) const;
    /// 红线（视口底部 10%）下方的内容拍位；无图/无 timing/无效 → false。
    bool cursorPosition(beatbench::Position& out) const;
    qreal posDouble(const beatbench::Rational& r) const;
    void clampScroll();
    /// note 高度：随缩放浮动但带上下限（clamp(measureHeight * scale, min, max)）。
    /// 小 = 不随放大无界膨胀；下限保证 100% 缩放下 1/16 相邻 note 可分。hit/paint/hover 共用。
    qreal noteHeight() const;
    /// 拍位 → snap 网格吸附（绝对目标；移动 ghost 预览用——未来落点对齐网格）。
    qreal snapMf(qreal mf) const;

    QColor noteColor(const beatbench::Lane& lane) const;
    /// LN note 专用色（2026-09 用户：位于 LN 轨的 note 加深，直接分辨单点/LN）。
    /// LN（ln_pair 存在）→ 在普通色上加深；否则等同 noteColor(lane)。
    QColor noteColor(const beatbench::Lane& lane, const beatbench::Note& note) const;
    double bpmAt(const beatbench::Chart& chart, int measure) const;
    double beatsOf(const beatbench::Chart& chart, int measure) const;

    void drawHint(QPainter* p, const QString& text);

    QObject* m_session = nullptr;
    QObject* m_theme = nullptr;
    qreal m_measureHeight = 96.0;
    qreal m_rulerWidth = 56.0;  // 左列：小节号 + 秒标尺（m:ss；2026-09 回收窄——双行字号 11px 够）
    qreal m_laneWidth = 38.0;
    qreal m_metaTrackWidth = 48.0;  // BPM/STOP 元事件轨宽（2026-09 36→48：BPM 线秒行 m:ss.cc 显示）
    qreal m_scrollY = 0.0;
    /// 开头纯留白小节数（评分起点 0 之前；给「硬锁定 90%」在 0 小节留缓冲，
    /// 否则播放头在 0-1 小节/视口底部时锁不住 → 开始播放跳变；用户 2026-09）。
    qreal m_leadMeasures = 2.0;
    bool m_startPending = false;  // 未布局时的起点定位待办（geometryChange 就绪后执行）
    bool m_topHigh = true;
    int m_snapNum = 1;   // 吸附粒度分子（槽位步长 = snapNum/snapDen 小节）
    int m_snapDen = 16;  // 吸附粒度分母
    bool m_bgmExpanded = false;
    /// BGM 展开子通道高水位（本会话单调不减，2026-09）：列数 = max(m_bgmMaxLine, 实际行)+1+PAD；
    /// 空白尾行保存时天然丢弃（bms_writer 按 max(bgm_line)+1 写），故只增不减安全，新谱面也能放。
    std::uint32_t m_bgmMaxLine = 0;
    bool m_showChannelIds = false;
    int m_noteSampleMode = 0;  // note 采样标签：0=隐藏 1=id 2=文件名
    bool m_lnSelectMode = false;  // LN 选取模式（默认关）：点 LN 任一段自动返回配对
    bool m_showExtras = false;  // BGA 图层通道列（d场景更多轨道）
    bool m_showGrid = true;   // 槽位弱线显示开关（「网格」按钮）
    qreal m_scrollX = 0.0;
    bool m_perfLog = false;  // paint 帧耗时采样（--perf-log）
    double m_playheadSec = -1.0;  // M5.2 播放头秒（负 = 隐藏；暂停/停止也显示）
    bool m_followPlayhead = true;  // M5.2 播放头跟随开关（默认开——用户拍板）
    double m_loopASec = -1.0;     // M5.2 A 循环点秒（-1 = 未设）
    double m_loopBSec = -1.0;     // M5.2 B 循环点秒（-1 = 未设）
    // —— M7 性能：背景 tile 缓存（网格/列底/底纹；滚动只 blit，不逐线画） ——
    // tile = 2 小节周期（偶数+奇数底纹交替）高 × 全宽；内容坐标首行含网格。
    // 无效化条件：theme/measureHeight/snap/列布局/宽度/方向变化。
    QImage m_bgTile;
    qreal m_bgTileW = -1.0;       // 缓存时宽度（无效化用）
    double m_bgTileMeasureH = -1.0;
    int m_bgTileSnapNum = -1;
    int m_bgTileSnapDen = -1;
    qreal m_bgTileScrollX = 0.0;  // 缓存时 scrollX（水平滚动列位变 → 列底色错位；2026-09 用户）
    bool m_bgTileDirty = true;    // 需求有效化（theme/列/缩放变化置位）
    void invalidateBgTile() { m_bgTileDirty = true; }
    void rebuildBgTile(const beatbench::Chart* chart, const ThemeManager* th,
                       qreal w, qreal h, qreal metaRight);
    bool m_debugLaneTint = false;  // 调试：打印 key 轨列底色（排查深色皮肤下 key 轨显黑）
    std::vector<Column> m_columns;
    std::vector<QRectF> m_colRects;  // 最近一次 paint 的列 rect（列头点击命中用）
    QFont m_rulerFont;
    QFont m_noteLabelFont;  // note 内采样标签（9px）
    // —— M7 性能：note 标签 halo 文本预渲染缓存（同一 label+色 → QImage；paint 一次
    // drawImage 替代 9 次 drawText——note 标签模式性能优化，2026-09） ——
    struct HaloKey {
        QString label;
        QRgb halo = 0;  // noteColor (halo)
        QRgb bg = 0;    // th->bg() (中心文字色)
        int fontPx = 10;
        bool operator==(const HaloKey& o) const {
            return label == o.label && halo == o.halo && bg == o.bg && fontPx == o.fontPx;
        }
    };
    struct HaloKeyHash {
        std::size_t operator()(const HaloKey& k) const {
            std::size_t h = std::hash<std::string>()(k.label.toStdString());
            h ^= static_cast<std::size_t>(k.halo) << 1;
            h ^= static_cast<std::size_t>(k.bg) << 2;
            h ^= static_cast<std::size_t>(k.fontPx) << 3;
            return h;
        }
    };
    std::unordered_map<HaloKey, QImage, HaloKeyHash> m_haloCache;
    void clearHaloCache() { m_haloCache.clear(); }
    QString m_hoverText;   // 鼠标位置 + note 信息（状态栏）
    int m_hoverMeasure = -1;
    qreal m_hoverY = -1.0;  // 悬停线（屏幕 y；-1 = 无）
    qreal m_hoverX = -1.0;  // 悬停 x（M6 预览：列命中用；-1 = 无）
    // —— M6 编辑预览（用户 2026-09）——
    QString m_previewNoteKind;  // 放置工具 ghost 类型（"normal"/"ln"/"mine"；空 = 关）
    bool m_movePreview = false;  // 移动预览总开关（拖拽时 QML 置 true）
    qreal m_moveDeltaF = 0.0;    // 移动预览位移（拍位小数）
    QVariantMap m_moveSourceLane;  // 拖起 lane（{player,kind,index}）
    QVariantMap m_moveTargetLane;  // 目标 lane（跨通道移动）；空 = 不改列
    int m_moveSourceBgmLine = -1;  // 拖起 note 的 BGM 子通道行号（-1=非 BGM；相对平移基准）
    QVariantList m_selection;  // 选中 note 集合（NoteRef 语义；绘制高亮用）
    QVariantList m_metaSelection;  // 选中 BGA/BPM/STOP 对象集合（绘制高亮用）
    int m_lastVisibleNotes = 0;  // 最近一次 paint 的可见 note 数（--perf-log）
};

}  // namespace beatbench::app
