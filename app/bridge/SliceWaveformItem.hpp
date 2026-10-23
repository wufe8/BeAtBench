// SPDX-License-Identifier: GPL-3.0-only
// 切音工作台波形视图（M6.1）：参考音频全曲波形 + MIDI note 刻度 + 播放头 + 点击/拖动 seek。
// 数据源 = SliceWorkspace（波形金字塔 + note 表 + offset）；播放头位置由 QML 定时
// 读 audioEngine.refPositionSec 传入（~20Hz）。皮肤：QPainter 自绘 + Theme token。
// M6.4 换行视口（用户 2026-09）：波形按 rowSec（每行时长）换行成多行文本式排版，
// 滚轮/方向键整行滚动，Ctrl+滚轮缩放（改 rowSec）；行界 = rowSec 整数倍（可预测编辑）。
// 行内绘制复用：波形列 / 中央轴 / 拍子网格（offset 锚点）/ MIDI 线 / 切片线 / 播放头。
#pragma once

#include <QtQml/qqmlregistration.h>

#include <QQuickPaintedItem>

class QWheelEvent;

namespace beatbench::audio {
class WaveformPyramid;
}

namespace beatbench::app {

class SliceWorkspace;
class ThemeManager;

class SliceWaveformItem : public QQuickPaintedItem {
    Q_OBJECT
    QML_ELEMENT

    Q_PROPERTY(QObject* workspace READ workspace WRITE setWorkspace NOTIFY workspaceChanged)
    Q_PROPERTY(QObject* theme READ theme WRITE setTheme NOTIFY themeChanged)
    /// 播放头秒（<0 = 不显示；QML 定时刷新）。
    Q_PROPERTY(qreal playheadSec READ playheadSec WRITE setPlayheadSec NOTIFY playheadSecChanged)
    // ---- M6.2 实时拍子网格参考线（offset/BPM/snap 调参的视觉反馈） ----
    /// 显示开关（QML 绑定「切片源=网格」时 true）。
    Q_PROPERTY(bool gridVisible READ gridVisible WRITE setGridVisible NOTIFY gridVisibleChanged)
    /// 网格 BPM（QML bpmBox 绑定；>0 才画）。
    Q_PROPERTY(qreal gridBpm READ gridBpm WRITE setGridBpm NOTIFY gridBpmChanged)
    /// 每拍细分（QML subBox 绑定；>=1）。
    Q_PROPERTY(int gridSubdivision READ gridSubdivision WRITE setGridSubdivision NOTIFY
                   gridSubdivisionChanged)
    /// 每小节拍数（网格参考线小节分组；默认 4 = 4/4；同 core GridConfig.beatsPerMeasure）。
    Q_PROPERTY(int gridBeatsPerMeasure READ gridBeatsPerMeasure WRITE setGridBeatsPerMeasure NOTIFY
                   gridBeatsPerMeasureChanged)
    /// MIDI note 刻度线显示（M6.3c：网格模式下默认关；QML「MIDI 线」开关 + Ctrl 临时切换）。
    Q_PROPERTY(bool midiVisible READ midiVisible WRITE setMidiVisible NOTIFY midiVisibleChanged)
    // ---- M6.4 换行视口 ----
    /// 每行时长（秒；QML 由缩放档位换算：4 小节×beatsPerMeasure×60/BPM 等；<=0 = 整曲一行兜底）。
    Q_PROPERTY(qreal rowSec READ rowSec WRITE setRowSec NOTIFY rowSecChanged)
    /// 首可见行号（整数；滚轮/方向键 ±1；越界在绘制/交互处夹逼）。
    Q_PROPERTY(int scrollRow READ scrollRow WRITE setScrollRow NOTIFY scrollRowChanged)
    /// 同屏行数（1-6；默认 4）。
    Q_PROPERTY(int visibleRows READ visibleRows WRITE setVisibleRows NOTIFY visibleRowsChanged)
    /// 缩放档位探针：当前行数（行首 + 可见行数；QML 滚动指示用）。
    Q_INVOKABLE int totalRows() const;
    /// 吸附到当前拍子网格线（cell = 拍/细分；offset 基准；夹逼 [0,时长]）。
    /// M6.4b：点击 seek /（后续）键盘移动与手动切片共用。
    Q_INVOKABLE double snapToGrid(double t) const;
    /// 当前选中拍子（秒；-1 = 无）。M6.4d：首次点击选中、再点击同一拍子 = 切分点切换；
    /// 键盘 ←→ 移动（后续）也写它。绘制：行内 teal 2px 竖带 + 顶部 tab。
    Q_PROPERTY(qreal selectedBeatSec READ selectedBeatSec WRITE setSelectedBeatSec NOTIFY
                   selectedBeatSecChanged)

   public:
    explicit SliceWaveformItem(QQuickItem* parent = nullptr);

    void paint(QPainter* painter) override;

    QObject* workspace() const { return m_workspace; }
    void setWorkspace(QObject* v);
    QObject* theme() const { return m_theme; }
    void setTheme(QObject* v);
    qreal playheadSec() const { return m_playheadSec; }
    void setPlayheadSec(qreal v);
    bool gridVisible() const { return m_gridVisible; }
    void setGridVisible(bool v);
    qreal gridBpm() const { return m_gridBpm; }
    void setGridBpm(qreal v);
    int gridSubdivision() const { return m_gridSubdivision; }
    void setGridSubdivision(int v);
    int gridBeatsPerMeasure() const { return m_gridBeatsPerMeasure; }
    void setGridBeatsPerMeasure(int v);
    bool midiVisible() const { return m_midiVisible; }
    void setMidiVisible(bool v);
    qreal rowSec() const { return m_rowSec; }
    void setRowSec(qreal v);
    int scrollRow() const { return m_scrollRow; }
    void setScrollRow(int v);
    int visibleRows() const { return m_visibleRows; }
    void setVisibleRows(int v);
    qreal selectedBeatSec() const { return m_selectedBeatSec; }
    void setSelectedBeatSec(qreal v);

   signals:
    void workspaceChanged();
    void themeChanged();
    void playheadSecChanged();
    void gridVisibleChanged();
    void gridBpmChanged();
    void gridSubdivisionChanged();
    void gridBeatsPerMeasureChanged();
    void midiVisibleChanged();
    void rowSecChanged();
    void scrollRowChanged();
    void visibleRowsChanged();
    void selectedBeatSecChanged();
    /// 点击/拖动 → 目标秒（QML 接 audioEngine.refSeek）。
    void seekRequested(double seconds);
    /// 滚轮（无 Ctrl）：dir = ±1（+1 = 向后翻行/看更晚）。QML 改 scrollRow。
    void scrollRequested(int dir);
    /// Ctrl+滚轮：dir = ±1（+1 = 放大/每行时长更短）。QML 改缩放档位。
    void zoomRequested(int dir);
    /// 双击（M6.4c 手动切分）：已吸附到拍子网格的秒（QML → SliceWorkspace::toggleManualPoint）。
    void manualToggleRequested(double seconds);
    /// 右键：已吸附的秒（QML → SliceWorkspace::removeManualPoint）。
    void manualDeleteRequested(double seconds);

   protected:
    void mousePressEvent(QMouseEvent* event) override;
    void mouseMoveEvent(QMouseEvent* event) override;
    void mouseReleaseEvent(QMouseEvent* event) override;
    void wheelEvent(QWheelEvent* event) override;

   private:
    SliceWorkspace* workspaceObj() const;
    ThemeManager* themeObj() const;
    /// x/y（widget 坐标）→ 绝对秒（按当前行视口换算）+ 网格吸附（gridVisible 时），发 seekRequested。
    void requestSeek(qreal x, qreal y);
    /// x/y → 绝对秒 + 吸附（网格模式）；手动切分用（双击/右键）。越界 → -1。
    double manualPointAt(qreal x, qreal y) const;
    /// 画一行：波形列 + 中央轴 + 网格 + MIDI 线 + 切片线 + 播放头（行内裁剪）。
    /// pcm/pcmFrames：深档直读原始 PCM（每像素 <256 采样时金字塔 256 采样/桶会
    /// 把高频波形糊成实心；直接扫 min/max 才能看到正弦周期）；nullptr = 走金字塔。
    void drawRow(QPainter* p, const QRectF& plot, double t0, double t1, const SliceWorkspace* ws,
                 const beatbench::audio::WaveformPyramid* pyr, const float* pcm,
                 std::size_t pcmFrames, qreal pxPerSec, qreal amp, double sr) const;
    /// 画行标签（左侧 gutter：小节号 + 秒；mono 小字）。
    void drawRowLabel(QPainter* p, const QRectF& row, double t0, double t1,
                      const ThemeManager* th) const;
    /// 画实时拍子网格参考线（BPM/细分/offset；等分）。范围 = [t0, t1)。
    void drawGridLines(QPainter* p, const QRectF& plot, double t0, double t1,
                       const SliceWorkspace* ws, qreal pxPerSec, const ThemeManager* th) const;
    /// 画起点（time=offset）显眼标记：顶部 tab + 秒数标签。
    void drawOriginMarker(QPainter* p, qreal x, const ThemeManager* th, double t,
                          const QRectF& plot) const;

    QObject* m_workspace = nullptr;
    QObject* m_theme = nullptr;
    qreal m_playheadSec = -1.0;
    // ---- M6.2 实时拍子网格 ----
    bool m_gridVisible = false;
    qreal m_gridBpm = 120.0;
    int m_gridSubdivision = 4;
    int m_gridBeatsPerMeasure = 4;
    bool m_midiVisible = true;  ///< MIDI note 刻度（M6.3c 网格模式默认关）
    // ---- M6.4 换行视口 ----
    qreal m_rowSec = 8.0;            ///< 每行时长（QML 缩放档位换算；<=0 → 整曲一行）
    int m_scrollRow = 0;             ///< 首可见行
    int m_visibleRows = 4;           ///< 同屏行数
    qreal m_selectedBeatSec = -1.0;  ///< 选中拍子（-1 = 无）
};

}  // namespace beatbench::app
