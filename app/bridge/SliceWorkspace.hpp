// SPDX-License-Identifier: GPL-3.0-only
// 切音工作台数据桥（M6.1）：参考音频（外部 stem.wav）+ MIDI（notes.mid）的装载与持有。
//
// 定位（doc/02 §3 M6.1）：与编辑页平级的切音工作台的数据侧——「导入工作台」：
// 外部音频解码（→ 波形金字塔 + 时长/采样率）+ MIDI 解析（→ note 表）+ offset 微调
// （全局；逻辑在此桥换算出「+offset」后的秒）。播放/seek 委托 AudioEngine
// （参考音频专用 PcmPlayback，与谱面播放独立）。MIDI 解析走 core `midi` 模块
// （headless 可测；本桥只做文件读取与 QML 适配）。
//
// 双语言纪律（doc/08 §2）：解码/解析/数据持有在 C++，QML 只消费信号/属性。
// 线程：解码在 QThreadPool（不卡 UI），完成回 UI 线程装载。
#pragma once

#include <QObject>
#include <QString>
#include <QVariantList>
#include <QVariantMap>

#include <memory>
#include <vector>

#include "beatbench/audio/ReferenceTrack.hpp"
#include "beatbench/core/midi/MidiFile.hpp"
#include "beatbench/core/slice/Slice.hpp"

namespace beatbench::app {

class AudioEngine;
class ChartSession;

class SliceWorkspace : public QObject {
    Q_OBJECT
    Q_PROPERTY(QString audioPath READ audioPath NOTIFY audioChanged)
    Q_PROPERTY(bool hasAudio READ hasAudio NOTIFY audioChanged)
    Q_PROPERTY(qreal audioDurationSec READ audioDurationSec NOTIFY audioChanged)
    Q_PROPERTY(qreal audioSampleRate READ audioSampleRate NOTIFY audioChanged)
    Q_PROPERTY(QString midiPath READ midiPath NOTIFY midiChanged)
    Q_PROPERTY(bool hasMidi READ hasMidi NOTIFY midiChanged)
    Q_PROPERTY(QVariantList midiNotes READ midiNotes NOTIFY midiChanged)
    Q_PROPERTY(qreal offsetSec READ offsetSec WRITE setOffsetSec NOTIFY offsetChanged)
    Q_PROPERTY(bool busy READ busy NOTIFY busyChanged)
    Q_PROPERTY(QString statusText READ statusText NOTIFY statusChanged)
    // ---- M6.2 切片 ----
    Q_PROPERTY(QVariantList slices READ slices NOTIFY slicesChanged)
    Q_PROPERTY(bool hasSlices READ hasSlices NOTIFY slicesChanged)
    Q_PROPERTY(bool canUndoSlice READ canUndoSlice NOTIFY sliceHistoryChanged)
    Q_PROPERTY(bool canRedoSlice READ canRedoSlice NOTIFY sliceHistoryChanged)
    /// MIDI tempo（首个 tempo 事件 → BPM；无 MIDI/无 tempo → 120）。网格参数默认值。
    Q_PROPERTY(qreal midiTempoBpm READ midiTempoBpm NOTIFY midiChanged)

public:
    explicit SliceWorkspace(QObject* parent = nullptr);

    /// 播放/seek 委托目标（main.cpp 接线；不拥有）。
    void setAudioEngine(AudioEngine* engine);
    /// 当前谱面会话（main.cpp 接线；用于 #WAV 占用检测 + 输出目录；不拥有；可空）。
    void setChartSession(ChartSession* session);

    // ---- M6.3 导出（分片 → .wav + 可复制 BMS raw） ----
    /// 当前谱面已占用的 #WAV id（数值；无谱面 → 空）。供起始 id 默认值/占用视图。
    Q_INVOKABLE QVariantList occupiedWavIds() const;
    /// 下一个空闲 #WAV id（从 1 起跳过 occupied；无谱面 → 1）。
    Q_INVOKABLE int nextFreeWavId() const;
    Q_INVOKABLE int nextFreeWavIdForBase(int idBaseMode = 0, bool independentExport = false) const;
    Q_INVOKABLE int effectiveWavIdBase(int idBaseMode = 0) const;
    /// 写盘前预检：算出本次 layout 文件名，扫描 outDir 已有同前缀 wav。
    /// 不解码、不写盘。返回 {ok, outDir, prefix, planned, collisions, nextContinueIndex, error}。
    /// planned/collisions 为相对 prefix 的路径（如 slice_000.wav）。无切片也可扫盘（planned 空）。
    Q_INVOKABLE QVariantMap previewExportFiles(const QString& outDir, const QString& prefix) const;
    /// 导出：对每个「放置开关=开」的切片 → window()+fade → 写 <outDir>/<prefix>_<NNN>.wav，
    /// 分配 #WAV id（从 startId 起，跳过 occupied），并生成可复制 BMS raw。
    /// prefix = 落盘前缀（可含 `/` 或 `\` 作子目录，如 "slices/slice" 或 "slice"）；
    /// 自动识别正反斜杠。bpm/beatsPerMeasure/subdivision 用于拍位换算；offset 用当前 m_offsetSec。
    /// startMeasure = ch01 铺放起始小节（1-based；第 N 小节 = 文件 `#(N-1)01:`）。
    /// conflictPolicy：overwrite / continue / error（默认 error）。
    /// 有重名时 overwrite 按原名覆盖、continue 从已有最大序号 +1 连续编号、error 零落盘。
    /// 无重名时三种等价于按 layout 原名（首次仍从 000）写。文件序号与 #WAV id 独立。
    /// placeIntoChart（2026-09 用户「同时铺入编辑区」）：导出成功后把 raw 经 clipboard.paste
    /// （sub_line_mode="uniform"）直接写进当前谱面——子行接续（目标小节段已有最高子行 +1 起，
    /// 不挤旧行、新内容跨小节同列），单 CompositeCommand = 一个撤销步；需已加载谱面。
    /// 返回 {ok, raw, count, error, startMeasure, endMeasure, placementText, nextStartId,
    ///       placed, placedNotes, placeError, outDir, collisions, conflictPolicy}。
    Q_INVOKABLE QVariantMap exportSlices(qreal bpm, int subdivision,
                                         int beatsPerMeasure, int startId,
                                         int startMeasure,
                                         const QString& outDir, const QString& prefix,
                                         qreal fadeMs, bool placeIntoChart = false,
                                         const QString& conflictPolicy = QStringLiteral("error"),
                                         int idBaseMode = 0, bool independentExport = false);
    /// 测试入口：同步装载参考音频（不经 QThreadPool / 不碰声卡）。生产路径仍用 loadAudioFile。
    bool loadAudioFileSyncForTest(const QString& path);
    /// 测试入口：直接注入切片表（绕过 detectSlices）。
    void setSlicesForTest(std::vector<beatbench::slice::Slice> slices,
                          std::vector<bool> enabled);
    /// 建议的铺放起始小节（1-based）：当前谱面已用小节数 + 1（下一空小节；
    /// 无谱面 → 1）。「起始小节」SpinBox 默认值用；夹逼 [1,999]。
    Q_INVOKABLE int suggestedStartMeasure() const;

    /// 导入参考音频（异步解码；完成 → 波形金字塔 + 交给 AudioEngine 预览）。
    /// 返回 false = 立即失败（无文件/格式不支持）；解码失败异步报 statusText。
    Q_INVOKABLE bool loadAudioFile(const QString& path);
    /// 导入 MIDI（同步；core midi 解析；失败 statusText + false）。
    Q_INVOKABLE bool loadMidiFile(const QString& path);
    /// 清除全部参考素材（音频 + MIDI + offset；停声卸载）。
    Q_INVOKABLE void clearAll();

    /// 微调偏移（秒；正 = MIDI 相对音频延后；全局）。
    qreal offsetSec() const { return m_offsetSec; }
    /// ⚠️ Q_INVOKABLE：QML 经 `setOffsetSec()` 直接调用（SpinBox onValueModified）。
    /// 若不加 Q_INVOKABLE，QML 调用会 TypeError——offset 调整静默失效（M6.1/M6.2 均受影响）。
    Q_INVOKABLE void setOffsetSec(qreal v);
    /// 原始秒 → 应用 offset 后的秒（QML 显示/波形刻度用）。
    Q_INVOKABLE double adjustedSec(double rawSec) const { return rawSec + m_offsetSec; }

    // ---- M6.2 切片 ----
    /// 生成切片：source = "grid" | "midi"；offset 用当前 m_offsetSec（全局）。
    /// bpm/subdivision 仅网格用；durationSec = 音频时长（夹逼/丢弃越界切片）；
    /// extendToNextOnset（默认 true）= MIDI 右边界到下一起始/音频末尾（与手动切片一致；
    /// 同起始和弦合并成一片），false = 按 note 结束切分（历史行为）。
    Q_INVOKABLE bool detectSlices(const QString& source, qreal bpm, int subdivision,
                                  qreal durationSec, bool extendToNextOnset = true);
    /// M6.5 瞬态检测（audio 源）：STFT complex-domain 谱通量（audio 库
    /// OnsetDetector）→ 检出点落手动点集合（kind=manual，可 Z 增删/Ctrl+Z 撤销）。
    /// sensitivity 1-10（越大越敏感）；minGapSec 相邻切点最小间隔；
    /// append=false 替换式（检出 0 点则不动，>0 清空后整轨按检出点重建），
    /// true 追加式（与现有手动点距 ≤10ms 的跳过）。异步（QThreadPool，busy 期间
    /// 拒绝重入；完成前更换参考音频 → 结果丢弃并提示）；无参考音频 → false + 提示。
    Q_INVOKABLE bool detectOnsetSlices(qreal sensitivity, qreal minGapSec, bool append);
    /// 测试入口：同步执行同一检测 + 落点路径（不经线程池）。
    bool detectOnsetSlicesSyncForTest(qreal sensitivity, qreal minGapSec, bool append);
    /// 清除切片（保留参考素材）。
    Q_INVOKABLE void clearSlices();
    /// 切片「放置」开关（M6.3 铺放预选；越界忽略）。
    Q_INVOKABLE void setSliceEnabled(int index, bool v);
    /// 2026-09 切片编辑（切片表行双击 → 编辑起始/持续）：改 startSec/endSec；
    /// 起点 ≥0、终点夹逼到音频尾（无音频则仅校验正有限值）；按 startSec 重排 + 重编号
    /// （保持表序/手动切分点判定一致）。成功返回 true；状态经 statusText。
    Q_INVOKABLE bool setSliceBounds(int index, double startSec, double durationSec);
    // ---- M6.4c 手动切分点（双击添加/切换、右键删除；快照变化不影响已有点） ----
    /// 双击：t 已是内部边界 → 合并（删除该切分点）；否则拆分包含它的切片（新增切分点）。
    /// 返回是否发生变更；状态经 statusText。t 建议先经波形 snapToGrid（网格模式）。
    Q_INVOKABLE bool toggleManualPoint(double t);
    /// 右键：仅当 t 命中内部边界时删除（合并两侧）；否则 no-op。
    Q_INVOKABLE bool removeManualPoint(double t);
    /// M6.4f 键盘（2026-09 用户 woslicer 系）：清除全部手动切分点（自动源切片边界不受影响；
    /// 边界集合在 setSliceBounds 等重排后可能过期——尽力清除，找不到的跳过）。
    /// 返回实际清除数；状态经 statusText。
    Q_INVOKABLE int clearManualPoints();
    /// 切音工作区撤销/重做（切片表 + 手动点；与谱面 undo 栈独立）。
    Q_INVOKABLE bool undoSliceEdit();
    Q_INVOKABLE bool redoSliceEdit();
    bool canUndoSlice() const { return !m_undo.empty(); }
    bool canRedoSlice() const { return !m_redo.empty(); }
    /// V：手动切分点集（秒，升序）复制到内部剪贴板；空集 → false + 提示。
    Q_INVOKABLE bool copyManualPoints();
    /// B：**整体替换**（woslicer 语义：清现有手动点 → 应用剪贴板集）；空剪贴板 → 0 + 提示。
    /// 粘贴点是"拆分式插入"（不 toggle，避免命中边界被合并）；恰与现有边界重合的点记入集合。
    /// 返回粘贴/生效点数。
    Q_INVOKABLE int pasteManualPoints();
    QVariantList slices() const;
    bool hasSlices() const { return !m_slices.empty(); }
    qreal midiTempoBpm() const;

    // ---- C++ 消费（SliceWaveformItem 等） ----
    /// 波形金字塔指针（无音频 → nullptr；音频层 ReferenceTrack 持有）。
    const beatbench::audio::WaveformPyramid* waveformPyramid() const {
        return m_track.valid() ? &m_track.waveform() : nullptr;
    }
    const std::vector<midi::MidiNote>& notes() const { return m_midi.notes; }
    double offsetSecD() const { return m_offsetSec; }
    /// M6.2 切片表（C++ 消费：SliceWaveformItem 画线等）。
    const std::vector<beatbench::slice::Slice>& slicesC() const { return m_slices; }
    /// 参考音轨（C++ 消费：M6.3 分片导出 window() 等；无音频 → invalid）。
    const beatbench::audio::ReferenceTrack& track() const { return m_track; }

    // ---- QML 波形查询（总览条同构接口） ----
    Q_INVOKABLE QVariantMap waveformInfo() const;
    Q_INVOKABLE QVariantMap waveformRange(qlonglong frameLo, qlonglong frameHi) const;

    QString audioPath() const { return m_audioPath; }
    bool hasAudio() const { return m_track.valid(); }
    qreal audioDurationSec() const;
    qreal audioSampleRate() const { return m_track.sampleRate(); }
    QString midiPath() const { return m_midiPath; }
    bool hasMidi() const { return !m_midiPath.isEmpty(); }
    QVariantList midiNotes() const;
    bool busy() const { return m_busy; }
    QString statusText() const { return m_statusText; }

signals:
    void audioChanged();
    void midiChanged();
    void offsetChanged();
    void busyChanged();
    void statusChanged();
    /// M6.2 切片表变化（生成/清除/放置开关）。
    void slicesChanged();
    void sliceHistoryChanged();
    /// 2026-09「同时铺入编辑区」成功（#WAV 定义已进谱面）→ QML 刷新采样面板
    /// （左 dock 采样列表不会因 contentChanged 自动重取——内容变化触发的是时间轴/
    /// 波形刷新，定义表需显式重拉 session.samples）。
    void samplesPlaced();

private:
    void setStatus(const QString& text);
    /// M6.4c 手动切分点：命中内部边界（startSec ≈ t，i>=1）的切片序号；无 → -1。
    int findBoundaryIndex(double t) const;
    /// 拆分包含 t 的切片（空表 → 先建整轨 [0,dur) 再拆；t 须在片内，曲内非边界）。
    /// 纯拆分不合并——pasteManualPoints 用（与 toggle 的"命中即合并"区分）。
    bool splitSliceAt(double t, bool recordUndo = true);
    /// 合并 i-1/i（删除边界）；不校验命中，直接操作。
    bool removeManualPointAt(std::size_t i, bool recordUndo = true);
    /// 重排 slice index（拆分/合并后）。
    void renumberSlices();
    /// M6.5 检出点落点（UI 线程；替换/追加 + 拆分式插入 + 手动点集合维护；
    /// 空点集零破坏返回 0）。返回生效点数。
    int applyOnsetPoints(std::vector<double> onsets, bool append);
    struct SliceSnapshot {
        std::vector<beatbench::slice::Slice> slices;
        std::vector<bool> enabled;
        std::vector<double> manualPoints;
    };
    void pushSliceUndo();
    void restoreSliceSnapshot(const SliceSnapshot& snap);
    /// 解码完成（UI 线程）：ok 且 track 有效 → 移入 m_track + 交 AudioEngine 预览；
    /// 失败 → statusText（原参考音轨保留）。
    void audioDecoded(bool ok, beatbench::audio::ReferenceTrack* track,
                      const QString& path);

    AudioEngine* m_engine = nullptr;
    ChartSession* m_chartSession = nullptr;  ///< 当前谱面（#WAV 占用/输出目录；不拥有）
    QString m_audioPath;
    QString m_midiPath;
    beatbench::audio::ReferenceTrack m_track;  ///< 参考音轨（PCM + 金字塔 + 统计；音频层）
    midi::MidiFile m_midi;  ///< core 解析结果（notes() 供绘制；midiNotes() 供 QML）
    // ---- M6.2 切片 ----
    std::vector<beatbench::slice::Slice> m_slices;  ///< 切片表（core 计算结果）
    std::vector<bool> m_sliceEnabled;               ///< 放置开关（与 m_slices 一一对应）
    // ---- M6.4f 手动切分点集合（键盘 Z/C/V/B；集合与内部边界相互维护、尽力同步） ----
    std::vector<double> m_manualPoints;  ///< 手动切分点（秒，≈内部边界；可能因表重排过期）
    std::vector<double> m_copiedPoints;  ///< V 复制的切分点集（粘贴源）
    std::vector<SliceSnapshot> m_undo;
    std::vector<SliceSnapshot> m_redo;
    qreal m_offsetSec = 0.0;
    bool m_busy = false;
    QString m_statusText;
};

}  // namespace beatbench::app
